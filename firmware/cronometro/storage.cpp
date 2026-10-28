#include "storage.h"
#include "config.h"
#include "device.h"
#include "timer.h"
#include "json_utils.h"
#include "data_version.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

// A flash interna (LittleFS) é a fonte de verdade: sem cartão removível,
// sem fio solto, muito mais confiável pra sustentar a cronometragem em
// tempo real. Para levar os dados para o computador, use o Backup da página.
static bool flashOk = false;

// Versão standalone: não existe mais uma segunda task (networkTask) nem
// chamada HTTP alguma tocando os arquivos — todo o sistema roda numa única
// linha de execução (loop() principal, incluindo o WebServer, ver
// web_server.cpp), então o mutex que protegia os arquivos contra acesso
// concorrente entre Core 0 e Core 1 deixou de ser necessário.

void storageInit() {
  // 'true' = formata sozinha se a flash ainda não tiver um filesystem
  // válido (primeiro boot com este firmware, ou partição corrompida) —
  // seguro, só afeta a partição de dados, nunca o programa em si.
  if (!LittleFS.begin(true)) {
    Serial.println("[STORAGE] ERRO CRITICO: nao foi possivel montar a flash interna (LittleFS).");
    flashOk = false;
  } else {
    flashOk = true;
    Serial.println("[STORAGE] Flash interna (LittleFS) montada - fonte de verdade.");
  }

}

bool storageIsOk() {
  return flashOk;
}

void storageGenerateEventId(int64_t startTimestampUs, char* buffer, size_t bufferSize) {
  char deviceId[8];
  deviceGetShortId(deviceId, sizeof(deviceId));
  snprintf(buffer, bufferSize, "%s-%lld", deviceId, (long long)startTimestampUs);
}

// Traduz o status interno (canônico, em inglês — usado pela API/banco)
// para um rótulo em português, só para exibição no CSV. O valor
// armazenado no .jsonl NUNCA é traduzido — só a coluna visível do CSV,
// para não quebrar nenhuma comparação de string que dependa do original.
static const char* statusLabelPt(const char* status) {
  if (strcmp(status, "valid") == 0) return "Válido";
  if (strcmp(status, "invalid") == 0) return "Inválido";
  if (strcmp(status, "dnf") == 0) return "DNF";
  return "Pendente";
}

// Cria cada nível de uma pasta aninhada, um de cada vez — mesmo cuidado
// que já era necessário no SD (a LittleFS também não cria diretórios
// intermediários automaticamente).
static void ensureDir(const String &path) {
  int fromIndex = 1;
  while (true) {
    int slashIndex = path.indexOf('/', fromIndex);
    String partial = (slashIndex < 0) ? path : path.substring(0, slashIndex);
    if (!LittleFS.exists(partial)) {
      LittleFS.mkdir(partial);
    }
    if (slashIndex < 0) break;
    fromIndex = slashIndex + 1;
  }
}

static void ensureCsvHeader(const String &csvPath) {
  if (LittleFS.exists(csvPath)) return;
  File f = LittleFS.open(csvPath, FILE_WRITE);
  if (!f) return;
  f.println("Participante,Tempo,Resultado");
  f.close();
}

static String dayDirPath(const char* competitionSlug, int day) {
  return String("/") + competitionSlug + "/Dia_" + String(day);
}

// Constrói a linha JSON de um resultado — reaproveitada tanto na
// gravação normal (storageAppendRun) quanto na regravação completa do
// dia (storageRewriteDayJsonl), para nunca ter a mesma lógica de
// formatação em dois lugares.
static void buildRunJsonLine(const RunRecord &record, char* buf, size_t bufSize) {
  snprintf(buf, bufSize,
    "{\"event_id\":\"%s\",\"competition_id\":%d,\"participant_id\":%d,\"day\":%d,\"attempt\":%d,"
    "\"start_timestamp_us\":%lld,\"end_timestamp_us\":%lld,\"elapsed_us\":%lld,"
    "\"status\":\"%s\"}",
    record.eventId, record.competitionId, record.participantId, record.day, record.attempt,
    (long long)record.startTimestampUs, (long long)record.endTimestampUs, (long long)record.elapsedUs,
    record.status
  );
}

static void buildStoredRunJsonLine(const StoredRun &row, char* buf, size_t bufSize) {
  snprintf(buf, bufSize,
    "{\"event_id\":\"%s\",\"competition_id\":%d,\"participant_id\":%d,\"day\":%d,\"attempt\":%d,"
    "\"start_timestamp_us\":%lld,\"end_timestamp_us\":%lld,\"elapsed_us\":%lld,"
    "\"status\":\"%s\"}",
    row.eventId, row.competitionId, row.participantId, row.day, row.attempt,
    (long long)row.startTimestampUs, (long long)row.endTimestampUs, (long long)row.elapsedUs,
    row.status
  );
}

// ---------------------------------------------------------------------
// Fila de pendências em RAM — ver storage.h para o porquê.
// ---------------------------------------------------------------------

struct PendingRun {
  RunRecord record;
  RunContext ctx;
  bool used;
};

static PendingRun g_pending[MAX_PENDING_RUNS];

static void pendingPush(const RunRecord &record, const RunContext &ctx) {
  for (int i = 0; i < MAX_PENDING_RUNS; i++) {
    if (!g_pending[i].used) {
      g_pending[i].record = record;
      g_pending[i].ctx = ctx;
      g_pending[i].used = true;
      dataVersionBump();
      Serial.print("[STORAGE] Resultado enfileirado em RAM (flash indisponivel), posicao ");
      Serial.println(i);
      return;
    }
  }
  Serial.println("[STORAGE] ERRO: fila de pendencias cheia - resultado descartado (nunca deveria acontecer em uso normal).");
}

int storagePendingCount() {
  int n = 0;
  for (int i = 0; i < MAX_PENDING_RUNS; i++) {
    if (g_pending[i].used) n++;
  }
  return n;
}

int storagePendingCountFor(int competitionId, int participantId, int day) {
  int n = 0;
  for (int i = 0; i < MAX_PENDING_RUNS; i++) {
    if (!g_pending[i].used) continue;
    const RunContext &c = g_pending[i].ctx;
    // repetida ("retried") não gasta a tomada - ver runUsesAttempt em data_store.cpp
    const char* st = g_pending[i].record.status;
    if (strcmp(st, "retried") == 0 || strcmp(st, "retry") == 0) continue;
    if (c.competitionId == competitionId && c.participantId == participantId && c.day == day) n++;
  }
  return n;
}

int storagePendingRemoveFor(int participantId, RunDeletedFn onDeleted) {
  int n = 0;
  for (int i = 0; i < MAX_PENDING_RUNS; i++) {
    if (g_pending[i].used && g_pending[i].ctx.participantId == participantId) {
      if (onDeleted) onDeleted(g_pending[i].ctx.competitionSlug, g_pending[i].record.eventId);
      g_pending[i].used = false;
      n++;
    }
  }
  return n;
}

RunRecord* storagePendingRecordAt(int index) {
  if (index < 0 || index >= MAX_PENDING_RUNS || !g_pending[index].used) return nullptr;
  return &g_pending[index].record;
}

RunContext* storagePendingContextAt(int index) {
  if (index < 0 || index >= MAX_PENDING_RUNS || !g_pending[index].used) return nullptr;
  return &g_pending[index].ctx;
}

bool storagePendingSetStatus(int index, const char* status) {
  if (index < 0 || index >= MAX_PENDING_RUNS || !g_pending[index].used) return false;
  strlcpy(g_pending[index].record.status, status, sizeof(g_pending[index].record.status));
  dataVersionBump();
  return true;
}

// Corpo de escrita de fato — sem nenhum conhecimento da fila de
// pendências, para storageFlushPending() poder chamar isto diretamente
// sem risco de reenfileirar uma entrada que já está sendo processada
// (ver storageAppendRun() abaixo, que É o único lugar que enfileira).
static bool doPersistRun(const RunRecord &record, const RunContext &ctx) {
  if (!flashOk) return false;

  char timeStr[16];
  formatElapsedTime(record.elapsedUs, timeStr, sizeof(timeStr));

  String jsonlPath;
  String csvPath;
  bool useStructuredPath = ctx.hasContext && ctx.competitionSlug[0] != '\0' && ctx.day > 0;

  if (useStructuredPath) {
    String dirPath = dayDirPath(ctx.competitionSlug, ctx.day);
    ensureDir(dirPath);
    jsonlPath = dirPath + "/resultados.jsonl";
    csvPath = dirPath + "/resultados.csv";
  } else {
    jsonlPath = SD_QUEUE_FILENAME;
  }

  if (!LittleFS.exists(jsonlPath)) {
    File init = LittleFS.open(jsonlPath, FILE_WRITE);
    if (init) init.close();
  }

  File f = LittleFS.open(jsonlPath, FILE_APPEND);
  if (!f) {
    // Extremamente raro na flash interna (diferente do cartão SD de versões antigas) - se
    // acontecer mesmo assim, o resultado não é perdido: storageAppendRun()
    // enfileira em RAM e tenta de novo depois.
    Serial.print("[STORAGE] ERRO: falha ao abrir (flash) ");
    Serial.println(jsonlPath);
    return false;
  }

  char jsonLine[320];
  buildRunJsonLine(record, jsonLine, sizeof(jsonLine));
  f.println(jsonLine);
  f.flush();
  f.close();

  if (useStructuredPath) {
    ensureCsvHeader(csvPath);
    File csv = LittleFS.open(csvPath, FILE_APPEND);
    if (csv) {
      csv.printf("\"%s\",%s,%s\n", ctx.participantName, timeStr, statusLabelPt(record.status));
      csv.flush();
      csv.close();
    }
  }

  Serial.print("[STORAGE] Resultado persistido (flash): ");
  Serial.println(jsonlPath);
  dataVersionBump(); // invalida o cache de ranking/day-status/lista de corridas (ver data_version.h)
  return true;
}

bool storageAppendRun(const RunRecord &record, const RunContext &ctx) {
  if (doPersistRun(record, ctx)) return true;
  // Não persistiu agora - não descarta: fica na fila de
  // pendências até a flash voltar a responder (storageFlushPending()).
  pendingPush(record, ctx);
  return false;
}

// Orçamento de tempo total (não por tentativa) - com o cooldown de
// remontagem acima, o custo normal é quase zero (cada posição só faz um
// "return false" bem rápido); isto aqui é rede de segurança pro caso
// raro de a flash travar num mkdir/open individual - nunca deixa o loop() (e o
// WebServer local, que atende a validação do juiz) preso além disso.
static const uint32_t FLUSH_PENDING_BUDGET_MS = 300;

void storageFlushPending() {
  uint32_t startMs = millis();
  for (int i = 0; i < MAX_PENDING_RUNS; i++) {
    if (!g_pending[i].used) continue;
    if (millis() - startMs > FLUSH_PENDING_BUDGET_MS) {
      Serial.println("[STORAGE] Orcamento de tempo do flush de pendencias esgotado - continua na proxima corrida.");
      break;
    }
    if (doPersistRun(g_pending[i].record, g_pending[i].ctx)) {
      g_pending[i].used = false;
      dataVersionBump();
    }
  }
}

bool storageRewriteDayCsv(const char* competitionSlug, int day, const DayResultRow* rows, int count) {
  if (!flashOk || competitionSlug[0] == '\0' || day <= 0) {
    return false;
  }

  String dirPath = dayDirPath(competitionSlug, day);
  ensureDir(dirPath);
  String finalPath = dirPath + "/resultados.csv";
  String tmpPath = dirPath + "/resultados.csv.tmp";

  File f = LittleFS.open(tmpPath, FILE_WRITE);
  if (!f) {
    Serial.println("[STORAGE] ERRO: falha ao abrir CSV temporario (flash).");
    return false;
  }

  f.println("Participante,Tempo,Resultado");
  for (int i = 0; i < count; i++) {
    char timeStr[16];
    formatElapsedTime(rows[i].elapsedUs, timeStr, sizeof(timeStr));
    f.printf("\"%s\",%s,%s\n", rows[i].participantName, timeStr, statusLabelPt(rows[i].status));
  }
  f.flush();
  f.close();

  if (LittleFS.exists(finalPath)) {
    LittleFS.remove(finalPath);
  }
  LittleFS.rename(tmpPath, finalPath);

  Serial.print("[STORAGE] CSV do dia reescrito (");
  Serial.print(count);
  Serial.print(" linhas): ");
  Serial.println(finalPath);
  return true;
}

// Faz o parse manual de uma linha do .jsonl de volta para StoredRun —
// mesma técnica de extractJsonString/Int usada para respostas HTTP (ver
// json_utils.h): o arquivo é sempre gerado por buildRunJsonLine/
// buildStoredRunJsonLine, formato fixo e controlado por nós.
static void parseStoredRunLine(const String &line, StoredRun &out) {
  extractJsonString(line, "event_id", out.eventId, sizeof(out.eventId));
  out.competitionId = extractJsonInt(line, "competition_id", -1);
  out.participantId = extractJsonInt(line, "participant_id", -1);
  out.day = extractJsonInt(line, "day", -1);
  out.attempt = extractJsonInt(line, "attempt", -1);
  out.startTimestampUs = extractJsonInt64(line, "start_timestamp_us", 0);
  out.endTimestampUs = extractJsonInt64(line, "end_timestamp_us", 0);
  out.elapsedUs = extractJsonInt64(line, "elapsed_us", 0);
  char statusBuf[24];
  extractJsonString(line, "status", statusBuf, sizeof(statusBuf));
  strlcpy(out.status, statusBuf[0] ? statusBuf : "pending_validation", sizeof(out.status));
}

static StoredRun g_dayRunsScratch[MAX_RUNS_PER_DAY];

StoredRun* storageDayRunsScratch() {
  return g_dayRunsScratch;
}

int storageReadDayRuns(const char* competitionSlug, int day, StoredRun* outRows, int maxRows) {
  if (!flashOk || !competitionSlug || competitionSlug[0] == '\0' || day <= 0) {
    return 0;
  }

  String jsonlPath = dayDirPath(competitionSlug, day) + "/resultados.jsonl";
  if (!LittleFS.exists(jsonlPath)) {
    return 0;
  }

  File f = LittleFS.open(jsonlPath, FILE_READ);
  if (!f) {
    return 0;
  }

  int count = 0;
  while (f.available() && count < maxRows) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    parseStoredRunLine(line, outRows[count]);
    count++;
  }
  f.close();
  return count;
}

bool storageRewriteDayJsonl(const char* competitionSlug, int day, const StoredRun* rows, int count) {
  if (!flashOk || !competitionSlug || competitionSlug[0] == '\0' || day <= 0) {
    return false;
  }

  String dirPath = dayDirPath(competitionSlug, day);
  ensureDir(dirPath);
  String finalPath = dirPath + "/resultados.jsonl";
  String tmpPath = dirPath + "/resultados.jsonl.tmp";

  File f = LittleFS.open(tmpPath, FILE_WRITE);
  if (!f) {
    Serial.println("[STORAGE] ERRO: falha ao abrir jsonl temporario (flash).");
    return false;
  }

  char jsonLine[320];
  for (int i = 0; i < count; i++) {
    buildStoredRunJsonLine(rows[i], jsonLine, sizeof(jsonLine));
    f.println(jsonLine);
  }
  f.flush();
  f.close();

  if (LittleFS.exists(finalPath)) {
    LittleFS.remove(finalPath);
  }
  LittleFS.rename(tmpPath, finalPath);

  Serial.print("[STORAGE] resultados.jsonl reescrito (");
  Serial.print(count);
  Serial.print(" linhas): ");
  Serial.println(finalPath);
  dataVersionBump(); // invalida o cache de ranking/day-status/lista de corridas (ver data_version.h)
  return true;
}
