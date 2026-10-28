#pragma once
#include <stdint.h>
#include <stddef.h>

// =====================================================================
// STORAGE.H — Persistência local
//
// Formato: JSON Lines (.jsonl) — um objeto JSON por linha, append-only.
// Escolhido em vez de um único array JSON: uma linha corrompida (ex.:
// corte de energia no meio da escrita) não invalida as linhas
// anteriores — cada linha é independente e pode ser pulada
// individualmente ao reprocessar o arquivo.
//
// A fonte de verdade é a flash interna do ESP32 (LittleFS) —
// sem cartão removível, sem fio solto, muito mais confiável pra
// sustentar a operação em tempo real.
//
// Falha da flash NUNCA bloqueia a cronometragem
// — todas as funções aqui são seguras de chamar mesmo se storageInit()
// falhar; elas só logam o erro e retornam false/no-op (e o resultado
// ainda assim não se perde, ver a fila de pendências em RAM abaixo).
// =====================================================================

struct RunRecord {
  char eventId[48];     // identificador único do evento (ver storageGenerateEventId)
  int competitionId;    // -1 = sem competição (corrida avulsa)
  int participantId;    // -1 = idem
  int day;               // -1 = idem
  int attempt;            // -1 = idem
  int64_t startTimestampUs;
  int64_t endTimestampUs;
  int64_t elapsedUs;
  char status[24];      // "pending_validation" | "invalid" | "dnf" 
};

// Contexto legível, buscado do servidor uma única vez ao
// iniciar a corrida (não a cada polling) — usado para nomear a
// pasta/arquivo na flash E para preencher os IDs reais enviados ao
// ranking online (o reenvio tardio precisa saber a corrida certa, não a
// que estiver armada na hora do reenvio).
struct RunContext {
  char competitionSlug[32];   // nome da competição, seguro para pasta (ex.: "Desafio_de_Verao_2026")
  char participantName[40];   // nome do participante, para o CSV legível
  int competitionId;
  int participantId;
  int day;
  int attempt;
  bool hasContext;             // false = sem contexto (ex.: rede indisponível) -> usa fallback plano/IDs -1
};

// Uma linha do resultado consolidado do dia — usada para reescrever o
// resultados.csv (nome já resolvido, legível) a partir de dados
// atualizados (ex.: depois que o juiz validou uma corrida).
struct DayResultRow {
  char participantName[40];
  int64_t elapsedUs;
  char status[24];
};

// Uma linha lida de volta de /<slug>/Dia_N/resultados.jsonl — mesmos
// campos de RunRecord, sem o nome do participante (o .jsonl nunca guarda
// nome, só IDs — ver storageAppendRun). Usada por quem precisa reler o
// que já foi gravado: ranking (soma/melhor tempo), day-status (contagem
// de tomadas) e validação de corrida (mudar o status de uma linha).
struct StoredRun {
  char eventId[48];
  int competitionId;
  int participantId;
  int day;
  int attempt;
  int64_t startTimestampUs;
  int64_t endTimestampUs;
  int64_t elapsedUs;
  char status[24];
};

void storageInit();

// =====================================================================
// Fila de pendências em RAM — quando a flash falha ao persistir
// uma corrida (memória cheia etc.), o resultado NÃO é mais
// descartado: fica aqui até a flash voltar a responder. É isso que permite
// o juiz validar uma corrida mesmo assim (ver web_server.cpp) e o
// espelhamento pra nuvem (cloud_sync.cpp) continuar recebendo o
// resultado mesmo que a gravação local tenha falhado.
//
// Tamanho fixo pequeno de propósito: corridas não acontecem rápido o
// bastante para esgotar isto em uso normal — é rede de segurança para
// uma falha transitória, não uma fila de produção.
// =====================================================================
#define MAX_PENDING_RUNS 8

int storagePendingCount();

// Quantas pendências (ainda não persistidas na flash) existem para esta
// combinação competição/participante/dia — usado por
// competitionDayStatus()/maybeAdvanceAttempt() (data_store.cpp) para
// nunca oferecer/rearmar uma tomada que já está em andamento só porque
// a flash ainda não confirmou a tomada anterior.
int storagePendingCountFor(int competitionId, int participantId, int day);
// Avisado a cada corrida apagada na exclusão de um robô - é
// assim que os event_ids chegam até a fila de exclusões da nuvem antes
// de sumirem dos arquivos (ver cloudSyncQueueRunDeletion).
typedef void (*RunDeletedFn)(const char* competitionSlug, const char* eventId);

// Descarta da fila em RAM todas as corridas pendentes deste robô (qualquer
// competição/dia) - usado ao excluir o robô. Retorna quantas.
int storagePendingRemoveFor(int participantId, RunDeletedFn onDeleted = nullptr);

// Ponteiros diretos para a entrada N da fila (para handleGetRuns/
// handlePatchRunValidation lerem/mutarem sem copiar). nullptr se index
// for inválido ou a posição não estiver em uso.
RunRecord* storagePendingRecordAt(int index);
RunContext* storagePendingContextAt(int index);

// Muda o status de uma pendência (validação do juiz enquanto ainda não
// foi possível persistir na flash). Retorna false se index for inválido.
bool storagePendingSetStatus(int index, const char* status);

// Tenta persistir de novo cada pendência da fila (chama o mesmo caminho
// de escrita de storageAppendRun) — remove da fila só as que tiverem
// sucesso. Seguro de chamar a qualquer momento, mesmo com a fila vazia
// ou a flash ainda fora do ar.
void storageFlushPending();

// Persiste um resultado na flash (append). Se ctx.hasContext for true,
// grava em /<competitionSlug>/Dia_<day>/resultados.jsonl (fonte de
// verdade) E resultados.csv (leitura humana). Se hasContext for false,
// cai no arquivo plano original (SD_QUEUE_FILENAME) — só como fallback.
bool storageAppendRun(const RunRecord &record, const RunContext &ctx);

// Reescreve por completo o resultados.csv do dia informado, a partir de
// uma lista de linhas já resolvidas (nome + tempo + status ATUAL). Usa
// arquivo temporário + troca (rename) para nunca deixar o CSV num
// estado parcial/corrompido em caso de corte de energia no meio da
// escrita.
bool storageRewriteDayCsv(const char* competitionSlug, int day, const DayResultRow* rows, int count);

// Lê de volta todas as linhas de /<slug>/Dia_<day>/resultados.jsonl para
// 'outRows' (até 'maxRows' — ver MAX_RUNS_PER_DAY em config.h). Retorna
// quantas linhas foram lidas (0 se o arquivo não existe ou a flash está
// indisponível). Usada por ranking.cpp e data_store.cpp — nunca lê o
// .jsonl diretamente fora deste módulo, para manter o formato do
// arquivo encapsulado aqui.
int storageReadDayRuns(const char* competitionSlug, int day, StoredRun* outRows, int maxRows);

// Buffer de trabalho ÚNICO e COMPARTILHADO para ler as linhas de um dia
// (StoredRun[MAX_RUNS_PER_DAY] já usa ~16 KB — cada função que
// declarasse sua própria cópia estourava a DRAM disponível do ESP32,
// que também precisa de espaço para WiFi/BT e o heap do WebServer/
// ArduinoJson). Só existe UMA instância, exposta aqui; é seguro
// reutilizar entre chamadas desde que cada chamador termine de usar o
// conteúdo antes de chamar outra função que também leia um dia (ver
// comentário em ranking.cpp/data_store.cpp/web_server.cpp).
StoredRun* storageDayRunsScratch();

// Reescreve por completo o resultados.jsonl do dia (mesmo padrão
// temp+rename de storageRewriteDayCsv) — usada quando o juiz valida uma
// corrida (PATCH .../validation) e o status de UMA linha precisa mudar
// permanentemente na fonte de verdade, não só no CSV legível.
bool storageRewriteDayJsonl(const char* competitionSlug, int day, const StoredRun* rows, int count);

// Indica se a flash interna (LittleFS) está operacional — fonte de
// verdade.
bool storageIsOk();

// Gera um event_id único baseado no chip ID do ESP32 + timestamp de
// início da corrida.
void storageGenerateEventId(int64_t startTimestampUs, char* buffer, size_t bufferSize);