#include "cloud_sync.h"
#include "config.h"
#include "wifi.h"
#include "data_store.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>

static const char* PREFS_NAMESPACE = "cloudsync";

// Curto de propósito: a tentativa acontece dentro da janela de 3s de
// RESULT_DISPLAY_HOLD_US (ver cloud_sync.h) — nunca deve segurar o
// loop() por mais tempo que isso, mesmo se a nuvem estiver totalmente
// inacessível (ex.: IP errado, porta fechada).
static const uint32_t HTTP_TIMEOUT_MS = 1500;

static CloudSyncConfig g_config = { "", "" };

// Robôs renomeados cuja correção ainda não chegou na nuvem -
// espelhado na NVS (chave "renPend") para sobreviver a reboot.
static int g_renamePending[MAX_CLOUD_RENAME_PENDING];
static int g_renamePendingCount = 0;

void cloudSyncInit() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true); // somente leitura
  memset(g_config.baseUrl, 0, sizeof(g_config.baseUrl));
  memset(g_config.deviceToken, 0, sizeof(g_config.deviceToken));
  prefs.getString("baseUrl", g_config.baseUrl, sizeof(g_config.baseUrl));
  prefs.getString("devToken", g_config.deviceToken, sizeof(g_config.deviceToken));
  size_t renBytes = prefs.getBytes("renPend", g_renamePending, sizeof(g_renamePending));
  g_renamePendingCount = (int)(renBytes / sizeof(int));
  prefs.end();
  if (g_renamePendingCount > 0) {
    Serial.printf("[CLOUD] %d robo(s) renomeado(s) aguardando atualizar a nuvem.\n", g_renamePendingCount);
  }

  Serial.print("[CLOUD] Modo Online: ");
  Serial.println((strlen(g_config.baseUrl) > 0 && strlen(g_config.deviceToken) > 0) ? "ativo" : "inativo");
}

CloudSyncConfig cloudSyncGetConfig() {
  return g_config;
}

void cloudSyncSetConfig(const CloudSyncConfig &cfg) {
  g_config = cfg;
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("baseUrl", cfg.baseUrl);
  prefs.putString("devToken", cfg.deviceToken);
  prefs.end();
}

static bool configIsUsable() {
  return strlen(g_config.baseUrl) > 0 && strlen(g_config.deviceToken) > 0;
}

static void buildAuthHeader(char* buf, size_t bufSize) {
  snprintf(buf, bufSize, "Bearer %s", g_config.deviceToken);
}

bool cloudSyncTestAuth(char* outMsg, size_t outMsgSize) {
  if (!configIsUsable()) {
    strlcpy(outMsg, "Preencha o endereço e a chave de acesso.", outMsgSize);
    return false;
  }
  if (!wifiIsStaConnected()) {
    strlcpy(outMsg, "O cronômetro está sem internet. Conecte a uma rede em Internet, acima.", outMsgSize);
    return false;
  }

  char url[160];
  snprintf(url, sizeof(url), "%s/api/v1/device/ping", g_config.baseUrl);

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(url);
  char authHeader[96];
  buildAuthHeader(authHeader, sizeof(authHeader));
  http.addHeader("Authorization", authHeader);

  int code = http.GET();
  http.end();

  if (code == 200) {
    strlcpy(outMsg, "Tudo certo: conectado ao ranking online.", outMsgSize);
    return true;
  }
  if (code == 401) {
    strlcpy(outMsg, "Chave de acesso incorreta.", outMsgSize);
  } else if (code == 503) {
    strlcpy(outMsg, "O ranking online ainda não foi configurado do lado dele.", outMsgSize);
  } else if (code > 0) {
    strlcpy(outMsg, "O ranking online respondeu com erro. Tente de novo.", outMsgSize);
  } else {
    strlcpy(outMsg, "Não foi possível conectar. Confira o endereço.", outMsgSize);
  }
  return false;
}

// ---------------------------------------------------------------------
// Fila de reenvio na flash - ver cloud_sync.h
// ---------------------------------------------------------------------

// Uma linha por corrida: "<competitionId>\t<dia>\t<event_id>".
static const char* RESEND_QUEUE_PATH = "/nuvem_reenvio.txt";
static const size_t RESEND_QUEUE_MAX_BYTES = 32 * 1024; // ~1000 corridas - teto de segurança

static bool resendQueueContains(const char* eventId) {
  File f = LittleFS.open(RESEND_QUEUE_PATH, FILE_READ);
  if (!f) return false;
  bool found = false;
  while (f.available() && !found) {
    String line = f.readStringUntil('\n');
    int tab = line.lastIndexOf('\t');
    if (tab >= 0 && line.substring(tab + 1) == eventId) found = true;
  }
  f.close();
  return found;
}

static void cloudSyncEnqueue(const RunRecord &record, const RunContext &ctx) {
  // A mesma corrida só entra uma vez (ex.: ao terminar e ao ser validada) -
  // no envio, o status mais novo é lido do arquivo do dia.
  if (resendQueueContains(record.eventId)) return;
  File f = LittleFS.open(RESEND_QUEUE_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("[CLOUD] ERRO: nao abriu a fila de reenvio.");
    return;
  }
  if (f.size() < RESEND_QUEUE_MAX_BYTES) {
    f.printf("%d\t%d\t%s\n", ctx.competitionId, ctx.day, record.eventId);
  } else {
    Serial.println("[CLOUD] Fila de reenvio cheia - resultado nao sera reenviado automaticamente.");
  }
  f.close();
}

// Tira da fila a primeira linha cujo event_id seja este.
static void resendQueueRemove(const char* eventId) {
  File f = LittleFS.open(RESEND_QUEUE_PATH, FILE_READ);
  if (!f) return;
  String kept;
  bool removed = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() == 0) continue;
    int tab = line.lastIndexOf('\t');
    if (!removed && tab >= 0 && line.substring(tab + 1) == eventId) { removed = true; continue; }
    kept += line;
    kept += '\n';
  }
  f.close();
  LittleFS.remove(RESEND_QUEUE_PATH);
  if (kept.length() == 0) return;
  File out = LittleFS.open(RESEND_QUEUE_PATH, FILE_WRITE);
  if (out) {
    out.print(kept);
    out.close();
  }
}

static bool cloudSyncSend(const RunRecord &record, const RunContext &ctx);

// Manda a corrida mais antiga da fila. Retorna true se a fila andou
// (enviou, ou descartou uma corrida que não existe mais); false se não
// havia nada ou o envio falhou (fica para depois).
static bool resendQueueSendOne() {
  File f = LittleFS.open(RESEND_QUEUE_PATH, FILE_READ);
  if (!f) return false;
  String line;
  while (f.available() && line.length() == 0) { line = f.readStringUntil('\n'); line.trim(); }
  f.close();
  if (line.length() == 0) {
    LittleFS.remove(RESEND_QUEUE_PATH);
    return false;
  }
  int t1 = line.indexOf('\t');
  int t2 = line.lastIndexOf('\t');
  if (t1 <= 0 || t2 <= t1) {
    resendQueueRemove(line.substring(t2 + 1).c_str());
    return true;
  }
  int competitionId = line.substring(0, t1).toInt();
  int day = line.substring(t1 + 1, t2).toInt();
  String eventId = line.substring(t2 + 1);

  // Procura a corrida no arquivo do dia (fonte de verdade) ou, se ela
  // ainda não conseguiu ser gravada, na fila de pendências da flash.
  const Competition* comp = competitionById(competitionId);
  RunRecord rec = {};
  RunContext ctx = {};
  bool found = false;
  if (comp) {
    StoredRun* rows = storageDayRunsScratch();
    int n = storageReadDayRuns(comp->slug, day, rows, MAX_RUNS_PER_DAY);
    for (int i = 0; i < n && !found; i++) {
      if (eventId != rows[i].eventId) continue;
      strlcpy(rec.eventId, rows[i].eventId, sizeof(rec.eventId));
      rec.competitionId = rows[i].competitionId;
      rec.participantId = rows[i].participantId;
      rec.day = rows[i].day;
      rec.attempt = rows[i].attempt;
      rec.startTimestampUs = rows[i].startTimestampUs;
      rec.endTimestampUs = rows[i].endTimestampUs;
      rec.elapsedUs = rows[i].elapsedUs;
      strlcpy(rec.status, rows[i].status, sizeof(rec.status));
      found = true;
    }
    if (!found) {
      for (int i = 0; i < MAX_PENDING_RUNS && !found; i++) {
        RunRecord* r = storagePendingRecordAt(i);
        if (r && eventId == r->eventId) { rec = *r; found = true; }
      }
    }
  }
  // "retried" não existe na nuvem: a tomada refeita chega como corrida nova.
  if (!found || strcmp(rec.status, "retried") == 0 || strcmp(rec.status, "retry") == 0) {
    resendQueueRemove(eventId.c_str());
    return true;
  }
  const Participant* p = participantById(rec.participantId);
  strlcpy(ctx.competitionSlug, comp->slug, sizeof(ctx.competitionSlug));
  strlcpy(ctx.participantName, p ? p->name : "?", sizeof(ctx.participantName));
  ctx.competitionId = competitionId;
  ctx.participantId = rec.participantId;
  ctx.day = rec.day;
  ctx.attempt = rec.attempt;
  ctx.hasContext = true;

  if (!cloudSyncSend(rec, ctx)) return false;
  resendQueueRemove(eventId.c_str());
  return true;
}

static bool cloudSyncSend(const RunRecord &record, const RunContext &ctx) {
  const Competition* comp = competitionById(ctx.competitionId);
  const char* competitionName = comp ? comp->name : ctx.competitionSlug;
  // Nome ATUAL do robô, não o capturado na largada: uma corrida que ficou
  // na fila de reenvio pode sair depois de o robô ter sido renomeado.
  const Participant* currentParticipant = participantById(ctx.participantId);
  const char* participantName = currentParticipant ? currentParticipant->name : ctx.participantName;
  // Status local é "draft"|"finished" (data_store.h); a nuvem usa
  // "active"|"finished" pro mesmo conceito (ver database.py) - manda já
  // traduzido pra nuvem nunca precisar conhecer o vocabulário local.
  const char* competitionStatus = (comp && strcmp(comp->status, "finished") == 0) ? "finished" : "active";

  char url[160];
  snprintf(url, sizeof(url), "%s/api/v1/sync/runs", g_config.baseUrl);

  // Sem escapar aspas no nome do participante/competicao - mesma
  // convenção já usada em storage.cpp (buildRunJsonLine): assume que
  // nomes cadastrados não contêm aspas duplas.
  char body[512];
  snprintf(body, sizeof(body),
    "{\"competition_slug\":\"%s\",\"competition_name\":\"%s\",\"competition_status\":\"%s\","
    "\"event_id\":\"%s\",\"participant_name\":\"%s\",\"day\":%d,\"attempt\":%d,"
    "\"start_timestamp_us\":%lld,\"end_timestamp_us\":%lld,\"elapsed_us\":%lld,\"status\":\"%s\"}",
    ctx.competitionSlug, competitionName, competitionStatus, record.eventId,
    participantName, record.day, record.attempt,
    (long long)record.startTimestampUs, (long long)record.endTimestampUs, (long long)record.elapsedUs,
    record.status
  );

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  char authHeader[96];
  buildAuthHeader(authHeader, sizeof(authHeader));
  http.addHeader("Authorization", authHeader);

  int code = http.POST((uint8_t*)body, strlen(body));
  http.end();

  bool ok = (code == 200 || code == 201);
  if (ok) {
    Serial.print("[CLOUD] Resultado sincronizado (");
    Serial.print(record.eventId);
    Serial.println(").");
  } else {
    Serial.print("[CLOUD] Falha ao sincronizar (");
    Serial.print(record.eventId);
    Serial.print("): HTTP ");
    Serial.println(code);
  }
  return ok;
}

void cloudSyncTryNow(const RunRecord &record, const RunContext &ctx) {
  // Modo Offline (URL/token da nuvem em branco): não há pra onde mandar -
  // não enfileira (antes toda corrida entrava na fila e ela enchia à toa,
  // gerando o aviso de "fila cheia" a cada corrida).
  if (!configIsUsable() || !ctx.hasContext) return;
  if (!wifiIsStaConnected()) {
    cloudSyncEnqueue(record, ctx);
    return;
  }
  if (!cloudSyncSend(record, ctx)) cloudSyncEnqueue(record, ctx);
}

void cloudSyncFlushPending() {
  if (!configIsUsable() || !wifiIsStaConnected()) return;
  cloudSyncFlushRenames();
  cloudSyncFlushDeletes();
  // Algumas de uma vez (janela pós-corrida); o resto vai por cloudSyncPoll().
  for (int i = 0; i < 3; i++) {
    if (!resendQueueSendOne()) break;
  }
}

void cloudSyncPoll() {
  static uint32_t lastRunMs = 0;
  static uint32_t lastOtherMs = 0;
  if (!configIsUsable() || !wifiIsStaConnected()) return;
  uint32_t now = millis();
  // Uma corrida a cada 3 s - cada envio segura o loop no máximo ~1,5 s.
  if (now - lastRunMs >= 3000) {
    lastRunMs = now;
    if (LittleFS.exists(RESEND_QUEUE_PATH)) resendQueueSendOne();
  }
  // Renomeações/exclusões pendentes, bem de vez em quando.
  if (now - lastOtherMs >= 30000) {
    lastOtherMs = now;
    cloudSyncFlushRenames();
    if (LittleFS.exists("/nuvem_exclusoes.txt")) cloudSyncFlushDeletes();
  }
}

void cloudSyncCompetitionStatus(int competitionId) {
  if (!configIsUsable()) return;
  if (!wifiIsStaConnected()) return;
  cloudSyncFlushRenames();
  cloudSyncFlushDeletes();

  const Competition* comp = competitionById(competitionId);
  if (!comp) return;
  const char* competitionStatus = (strcmp(comp->status, "finished") == 0) ? "finished" : "active";

  char url[160];
  snprintf(url, sizeof(url), "%s/api/v1/sync/competition-status", g_config.baseUrl);

  char body[200];
  snprintf(body, sizeof(body),
    "{\"competition_slug\":\"%s\",\"competition_name\":\"%s\",\"competition_status\":\"%s\"}",
    comp->slug, comp->name, competitionStatus
  );

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  char authHeader[96];
  buildAuthHeader(authHeader, sizeof(authHeader));
  http.addHeader("Authorization", authHeader);

  int code = http.POST((uint8_t*)body, strlen(body));
  http.end();

  if (code == 200 || code == 201) {
    Serial.println("[CLOUD] Status da competicao sincronizado.");
  } else {
    Serial.print("[CLOUD] Falha ao sincronizar status da competicao: HTTP ");
    Serial.println(code);
  }
}

// ---------------------------------------------------------------------
// Robô renomeado - ver cloud_sync.h
// ---------------------------------------------------------------------

static void saveRenamePending() {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  if (g_renamePendingCount > 0) prefs.putBytes("renPend", g_renamePending, g_renamePendingCount * sizeof(int));
  else prefs.remove("renPend");
  prefs.end();
}

static void addRenamePending(int participantId) {
  for (int i = 0; i < g_renamePendingCount; i++) {
    if (g_renamePending[i] == participantId) return;
  }
  if (g_renamePendingCount >= MAX_CLOUD_RENAME_PENDING) {
    // Lista cheia: descarta o mais antigo (o reenvio normal de corridas
    // também corrige o nome, então nada fica errado para sempre).
    for (int i = 1; i < g_renamePendingCount; i++) g_renamePending[i - 1] = g_renamePending[i];
    g_renamePendingCount--;
  }
  g_renamePending[g_renamePendingCount++] = participantId;
  saveRenamePending();
}

static void removeRenamePending(int participantId) {
  int w = 0;
  for (int i = 0; i < g_renamePendingCount; i++) {
    if (g_renamePending[i] != participantId) g_renamePending[w++] = g_renamePending[i];
  }
  if (w != g_renamePendingCount) {
    g_renamePendingCount = w;
    saveRenamePending();
  }
}

// Manda o nome atual do robô para todas as corridas dele em cada
// competição (uma requisição por competição que tenha corridas dele).
// true = nuvem toda atualizada (ou nada a fazer).
static bool sendParticipantRename(int participantId) {
  const Participant* p = participantById(participantId);
  if (!p) return true; // robô excluído depois - não há mais o que renomear

  char url[160];
  snprintf(url, sizeof(url), "%s/api/v1/sync/participant-name", g_config.baseUrl);
  char authHeader[96];
  buildAuthHeader(authHeader, sizeof(authHeader));

  bool allOk = true;
  int nComp = competitionCount();
  for (int c = 0; c < nComp; c++) {
    const Competition* comp = competitionAt(c);
    JsonDocument doc;
    doc["competition_slug"] = comp->slug;
    doc["participant_name"] = p->name;
    JsonArray ids = doc["event_ids"].to<JsonArray>();
    for (int day = 1; day <= comp->numDays; day++) {
      StoredRun* rows = storageDayRunsScratch();
      int n = storageReadDayRuns(comp->slug, day, rows, MAX_RUNS_PER_DAY);
      for (int i = 0; i < n && ids.size() < 500; i++) {
        if (rows[i].participantId == participantId) ids.add((char*)rows[i].eventId); // char* = copiado
      }
    }
    if (ids.size() == 0) continue;

    String body;
    serializeJson(doc, body);
    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", authHeader);
    int code = http.POST(body);
    http.end();

    if (code == 200) {
      Serial.printf("[CLOUD] Nome do robo %d atualizado na nuvem (%s).\n", participantId, comp->slug);
    } else {
      Serial.printf("[CLOUD] Falha ao atualizar nome do robo %d na nuvem (%s): HTTP %d\n", participantId, comp->slug, code);
      allOk = false;
    }
  }
  return allOk;
}

CloudRenameResult cloudSyncParticipantRenamed(int participantId) {
  if (!configIsUsable()) return CLOUD_RENAME_OFF;
  addRenamePending(participantId); // primeiro grava - se a tentativa abaixo falhar, nada se perde
  if (wifiIsStaConnected() && sendParticipantRename(participantId)) {
    removeRenamePending(participantId);
    return CLOUD_RENAME_SYNCED;
  }
  return CLOUD_RENAME_PENDING;
}

void cloudSyncFlushRenames() {
  if (!configIsUsable() || !wifiIsStaConnected() || g_renamePendingCount == 0) return;
  int snapshot[MAX_CLOUD_RENAME_PENDING];
  int n = g_renamePendingCount;
  memcpy(snapshot, g_renamePending, n * sizeof(int));
  for (int i = 0; i < n; i++) {
    if (sendParticipantRename(snapshot[i])) removeRenamePending(snapshot[i]);
  }
}

// ---------------------------------------------------------------------
// Robô excluído - ver cloud_sync.h
// ---------------------------------------------------------------------

// Uma linha por corrida a apagar na nuvem: "<slug>\t<event_id>".
static const char* DELETE_QUEUE_PATH = "/nuvem_exclusoes.txt";
static const size_t DELETE_QUEUE_MAX_BYTES = 32 * 1024; // ~800 corridas - teto de segurança da flash

void cloudSyncQueueRunDeletion(const char* competitionSlug, const char* eventId) {
  if (!configIsUsable() || !competitionSlug || !competitionSlug[0] || !eventId || !eventId[0]) return;
  File f = LittleFS.open(DELETE_QUEUE_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("[CLOUD] ERRO: nao abriu a fila de exclusoes da nuvem.");
    return;
  }
  if (f.size() < DELETE_QUEUE_MAX_BYTES) {
    f.printf("%s\t%s\n", competitionSlug, eventId);
  } else {
    Serial.println("[CLOUD] Fila de exclusoes da nuvem cheia - corrida nao sera apagada la.");
  }
  f.close();
}

static bool postRunsDelete(const char* slug, JsonArray ids) {
  JsonDocument doc;
  doc["competition_slug"] = slug;
  doc["event_ids"] = ids;
  String body;
  serializeJson(doc, body);

  char url[160];
  snprintf(url, sizeof(url), "%s/api/v1/sync/runs-delete", g_config.baseUrl);
  char authHeader[96];
  buildAuthHeader(authHeader, sizeof(authHeader));
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", authHeader);
  int code = http.POST(body);
  http.end();

  if (code == 200) {
    Serial.printf("[CLOUD] %u corrida(s) apagada(s) na nuvem (%s).\n", (unsigned)ids.size(), slug);
    return true;
  }
  Serial.printf("[CLOUD] Falha ao apagar corridas na nuvem (%s): HTTP %d\n", slug, code);
  return false;
}

CloudRenameResult cloudSyncFlushDeletes() {
  if (!configIsUsable()) return CLOUD_RENAME_OFF;
  if (!LittleFS.exists(DELETE_QUEUE_PATH)) return CLOUD_RENAME_SYNCED;
  if (!wifiIsStaConnected()) return CLOUD_RENAME_PENDING;

  // Lê a fila inteira (pequena) e agrupa por competição.
  File f = LittleFS.open(DELETE_QUEUE_PATH, FILE_READ);
  if (!f) return CLOUD_RENAME_PENDING;
  JsonDocument groups; // { "<slug>": ["<event_id>", ...], ... }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int tab = line.indexOf('\t');
    if (tab <= 0 || tab == (int)line.length() - 1) continue;
    String slug = line.substring(0, tab);
    String eventId = line.substring(tab + 1);
    JsonArray ids = groups[slug].is<JsonArray>() ? groups[slug].as<JsonArray>() : groups[slug].to<JsonArray>();
    ids.add(eventId);
  }
  f.close();

  String failed; // linhas que não foram e voltam pro arquivo
  for (JsonPair group : groups.as<JsonObject>()) {
    const char* slug = group.key().c_str();
    JsonArray all = group.value().as<JsonArray>();
    // A nuvem aceita até 500 por chamada.
    size_t total = all.size();
    for (size_t start = 0; start < total; start += 500) {
      JsonDocument chunkDoc;
      JsonArray chunk = chunkDoc.to<JsonArray>();
      for (size_t i = start; i < total && i < start + 500; i++) chunk.add(all[i].as<const char*>());
      if (!postRunsDelete(slug, chunk)) {
        for (JsonVariant id : chunk) {
          failed += slug;
          failed += '\t';
          failed += id.as<const char*>();
          failed += '\n';
        }
      }
    }
  }

  LittleFS.remove(DELETE_QUEUE_PATH);
  if (failed.length() == 0) return CLOUD_RENAME_SYNCED;
  File out = LittleFS.open(DELETE_QUEUE_PATH, FILE_WRITE);
  if (out) {
    out.print(failed);
    out.close();
  }
  return CLOUD_RENAME_PENDING;
}
