#include "web_server.h"
#include "web_page_gz.h" // gerado de pagina/index.html (ver firmware/tools/gerar_pagina.py)
#include "config.h"
#include "data_store.h"
#include "race_control.h"
#include "storage.h"
#include "ranking.h"
#include "data_version.h"
#include "cloud_sync.h"
#include "peripherals.h"
#include "timer.h"
#include "wifi.h"
#include "idle_screen.h"
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <new>

static WebServer server(HTTP_SERVER_PORT);

// ---------------------------------------------------------------------
// Senha do juiz. Sem senha configurada
// (padrão de fábrica): tudo aberto, como sempre. Com senha: qualquer um
// VÊ (ver isPublicRequest); para mexer, a página pede a senha na própria
// tela (POST /login) e recebe uma chave de sessão, que manda em todo
// pedido no cabeçalho X-Sessao. A chave é sorteada a cada troca de senha
// e guardada junto (sobrevive a quedas de energia; trocar a senha
// desconecta todo mundo).
//
// Limitação aceita: HTTP puro - quem "fareja" o Wi-Fi consegue ler a
// senha/chave. Barra o cenário real (alguém oportunista mexendo sem
// autorização), não um atacante analisando pacotes.
// ---------------------------------------------------------------------

static void sendError(int code, const char* detail);

static char g_judgePassword[32] = "";
static char g_judgeToken[33] = "";

static void judgeAuthSetPassword(const char* password);

static void judgeAuthInit() {
  Preferences prefs;
  prefs.begin("judgeauth", true);
  prefs.getString("password", g_judgePassword, sizeof(g_judgePassword));
  prefs.getString("token", g_judgeToken, sizeof(g_judgeToken));
  prefs.end();
  // Senha criada em versões antigas não tinha chave de sessão: cria agora,
  // senão o login aceita a senha e devolve chave vazia (fica pedindo de novo).
  if (g_judgePassword[0] != '\0' && g_judgeToken[0] == '\0') {
    char pw[sizeof(g_judgePassword)];
    strlcpy(pw, g_judgePassword, sizeof(pw));
    judgeAuthSetPassword(pw);
  }
}

static void judgeAuthSetPassword(const char* password) {
  strlcpy(g_judgePassword, password, sizeof(g_judgePassword));
  g_judgeToken[0] = '\0';
  if (g_judgePassword[0] != '\0') {
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) snprintf(g_judgeToken + i * 2, 3, "%02x", rnd[i]);
  }
  Preferences prefs;
  prefs.begin("judgeauth", false);
  prefs.putString("password", g_judgePassword);
  prefs.putString("token", g_judgeToken);
  prefs.end();
}

// Comparação em tempo constante (não vaza, pelo tempo de resposta,
// quantos caracteres do começo bateram).
static bool sameSecret(const String &given, const char* secret) {
  size_t n = strlen(secret);
  if (given.length() != n) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= (uint8_t)given[i] ^ (uint8_t)secret[i];
  return diff == 0;
}

static bool judgeSessionValid() {
  if (g_judgePassword[0] == '\0') return true; // sem senha configurada - comportamento de sempre
  return g_judgeToken[0] != '\0' && sameSecret(server.header("X-Sessao"), g_judgeToken);
}

// true = pode continuar; false = já respondeu 401 (a página abre o login).
static bool checkJudgeAuth() {
  if (judgeSessionValid()) return true;
  sendError(401, "Precisa entrar com a senha do juiz.");
  return false;
}

// Contra adivinhação da senha: depois de AUTH_MAX_FAILURES senhas erradas
// seguidas, recusa o login por AUTH_LOCKOUT_MS (sem delay(), que travaria
// o cronômetro). Quem já entrou continua usando normalmente.
static const uint8_t AUTH_MAX_FAILURES = 5;
static const uint32_t AUTH_LOCKOUT_MS = 30000;
static uint8_t g_authFailures = 0;
static uint32_t g_authLockedAtMs = 0;

// Cabeçalhos de proteção em toda resposta: não deixa outro site abrir o
// painel escondido num iframe (clickjacking) nem o navegador guardar
// dados da competição em cache.
static void sendSecurityHeaders(bool noStore) {
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  if (noStore) server.sendHeader("Cache-Control", "no-store");
}

// ---------------------------------------------------------------------
// Helpers de resposta
// ---------------------------------------------------------------------

static void sendJsonDoc(int code, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void sendError(int code, const char* detail) {
  JsonDocument doc;
  doc["detail"] = detail;
  sendJsonDoc(code, doc);
}

static void sendNoContent() {
  server.send(204, "application/json", "");
}

// Quebra o path em segmentos, sem regex — mesmo espírito de parsing
// manual já usado no resto do firmware (ver json_utils.cpp): o
// WebServer nativo do core ESP32 não tem roteamento com parâmetros
// (":id"), então isso substitui a necessidade de uma biblioteca de
// regex extra só para separar "/api/v1/competitions/3/participants".
static int splitPath(const String &uri, String* outSegs, int maxSegs) {
  int count = 0;
  int start = (uri.length() > 0 && uri[0] == '/') ? 1 : 0;
  while (start <= (int)uri.length() && count < maxSegs) {
    int slash = uri.indexOf('/', start);
    String seg = (slash < 0) ? uri.substring(start) : uri.substring(start, slash);
    if (seg.length() > 0) outSegs[count++] = seg;
    if (slash < 0) break;
    start = slash + 1;
  }
  return count;
}

static bool readJsonBody(JsonDocument &doc) {
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  return !err;
}

// ---------------------------------------------------------------------
// Cache de respostas caras (ranking / day-status / lista de corridas —
// as únicas rotas que varrem os arquivos, ver ranking.cpp/data_store.cpp
// /storage.cpp). A página do juiz consulta essas rotas a cada ~1.5s
// mesmo quando nada mudou; sem isso, o loop() principal (que também
// cuida dos sensores e do display) ficava perceptivelmente lento
// reabrindo/relendo arquivos a cada poll. Cada cache guarda a
// última resposta JÁ SERIALIZADA para a última competição consultada,
// junto da versão dos dados (ver data_version.h) em que foi calculada —
// só recalcula quando algo relevante realmente mudou.
// ---------------------------------------------------------------------

struct JsonCache {
  int competitionId = -1;
  uint32_t generation = 0xFFFFFFFF;
  String json;
};

static bool cacheServe(JsonCache &cache, int competitionId) {
  if (cache.competitionId != competitionId || cache.generation != dataVersionCurrent()) {
    return false;
  }
  server.send(200, "application/json", cache.json);
  return true;
}

static void cacheFill(JsonCache &cache, int competitionId, JsonDocument &doc) {
  cache.competitionId = competitionId;
  cache.generation = dataVersionCurrent();
  serializeJson(doc, cache.json);
  server.send(200, "application/json", cache.json);
}

// ---------------------------------------------------------------------
// Participantes
// ---------------------------------------------------------------------

static void writeParticipantJson(JsonObject o, const Participant* p) {
  o["id"] = p->id;
  o["name"] = p->name;
  o["external_code"] = p->externalCode;
  o["team_id"] = p->teamId;
}

static void handleGetParticipants() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  int n = participantCount();
  for (int i = 0; i < n; i++) {
    writeParticipantJson(arr.add<JsonObject>(), participantAt(i));
  }
  sendJsonDoc(200, doc);
}

static void handlePostParticipant() {
  JsonDocument in;
  readJsonBody(in);
  if (!in["name"].is<const char*>() || strlen((const char*)in["name"]) == 0) {
    sendError(400, "Informe o nome.");
    return;
  }
  if (!in["team_id"].is<int>() || !teamById((int)in["team_id"])) {
    sendError(400, "Escolha uma equipe.");
    return;
  }
  const char* name = in["name"];
  const char* code = in["external_code"] | "";
  int teamId = in["team_id"];

  int id = participantCreate(name, code, teamId);
  if (id == -2) {
    sendError(409, "Já existe um robô com esse nome nesta equipe.");
    return;
  }
  if (id < 0) {
    sendError(400, "Limite de robôs do cronômetro atingido.");
    return;
  }
  JsonDocument out;
  writeParticipantJson(out.to<JsonObject>(), participantById(id));
  sendJsonDoc(201, out);
}

static void writeTeamJson(JsonObject o, const Team* t) {
  o["id"] = t->id;
  o["name"] = t->name;
  o["participant_count"] = teamParticipantCount(t->id);
}

static void handleGetTeams() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  int n = teamCount();
  for (int i = 0; i < n; i++) {
    writeTeamJson(arr.add<JsonObject>(), teamAt(i));
  }
  sendJsonDoc(200, doc);
}

static void handlePostTeam() {
  JsonDocument in;
  readJsonBody(in);
  if (!in["name"].is<const char*>() || strlen((const char*)in["name"]) == 0) {
    sendError(400, "Informe o nome.");
    return;
  }
  int id = teamCreate((const char*)in["name"]);
  if (id == -2) {
    sendError(409, "Já existe uma equipe com esse nome.");
    return;
  }
  if (id < 0) {
    sendError(400, "Limite de equipes do cronômetro atingido.");
    return;
  }
  JsonDocument out;
  writeTeamJson(out.to<JsonObject>(), teamById(id));
  sendJsonDoc(201, out);
}

// Correção de cadastro: campos ausentes no corpo mantêm o valor atual.
static void handlePatchParticipant(int id) {
  const Participant* p = participantById(id);
  if (!p) {
    sendError(404, "Robô não encontrado. Atualize a página.");
    return;
  }
  JsonDocument in;
  if (!readJsonBody(in)) {
    sendError(400, "Algo deu errado. Atualize a página e tente de novo.");
    return;
  }
  char name[sizeof(p->name)];
  char code[sizeof(p->externalCode)];
  strlcpy(name, in["name"] | p->name, sizeof(name));
  strlcpy(code, in["external_code"] | p->externalCode, sizeof(code));
  int teamId = in["team_id"] | p->teamId;

  char oldName[sizeof(p->name)];
  strlcpy(oldName, p->name, sizeof(oldName)); // p aponta pro cadastro, que o update altera

  int result = participantUpdate(id, name, code, teamId);
  if (result == -2) { sendError(409, "Já existe um robô com esse nome nesta equipe."); return; }
  if (result == -3) { sendError(400, "Escolha uma equipe."); return; }
  if (result == -4) { sendError(400, "Nome do robô vazio ou com aspas."); return; }
  if (result < 0) { sendError(404, "Robô não encontrado. Atualize a página."); return; }

  JsonDocument out;
  writeParticipantJson(out.to<JsonObject>(), participantById(id));
  // Nome mudou: corrige as corridas já enviadas pra nuvem.
  if (strcmp(oldName, name) != 0) {
    CloudRenameResult cloud = cloudSyncParticipantRenamed(id);
    out["cloud_rename"] = cloud == CLOUD_RENAME_SYNCED ? "synced" : cloud == CLOUD_RENAME_PENDING ? "pending" : "off";
  }
  sendJsonDoc(200, out);
}

static void handlePatchTeam(int id) {
  JsonDocument in;
  if (!readJsonBody(in) || !in["name"].is<const char*>()) {
    sendError(400, "Informe o nome.");
    return;
  }
  int result = teamRename(id, (const char*)in["name"]);
  if (result == -1) { sendError(404, "Equipe não encontrada. Atualize a página."); return; }
  if (result == -2) { sendError(409, "Já existe uma equipe com esse nome."); return; }
  if (result == -4) { sendError(400, "Nome da equipe vazio ou com aspas."); return; }

  JsonDocument out;
  writeTeamJson(out.to<JsonObject>(), teamById(id));
  sendJsonDoc(200, out);
}

static void handleDeleteTeam(int id) {
  if (!teamById(id)) {
    sendError(404, "Equipe não encontrada. Atualize a página.");
    return;
  }
  if (!teamDelete(id)) {
    sendError(409, "A equipe ainda tem robôs. Mova ou exclua os robôs antes.");
    return;
  }
  sendNoContent();
}

// POST /display-context - a página do juiz avisa em que tela está,
// ao trocar de tela e a cada ~10 s; é o que o display mostra quando não
// há robô armado nem validação pendente (ver idle_screen.h).
static void handlePostDisplayContext() {
  JsonDocument in;
  readJsonBody(in);
  idleScreenSetJudgeContext(in["view"] | "", in["competition_id"] | -1, in["phase"] | "");
  sendNoContent();
}

// GET /participants/{id}/runs-summary - quanto vai sumir se o robô for
// excluído (a página mostra isso na confirmação).
static void handleGetParticipantRunsSummary(int id) {
  if (!participantById(id)) {
    sendError(404, "Robô não encontrado. Atualize a página.");
    return;
  }
  int competitions = 0;
  int runs = participantRunCount(id, competitions);
  JsonDocument out;
  out["runs"] = runs;
  out["competitions"] = competitions;
  sendJsonDoc(200, out);
}

// Exclui o robô e TUDO ligado a ele: corridas, pendências e inscrições
// - e, com o modo Online ativo, as corridas já enviadas pra
// nuvem também (cada corrida apagada entra na fila de exclusões da
// nuvem antes de sumir dos arquivos).
static void handleDeleteParticipant(int id) {
  int runsDeleted = 0;
  int result = participantPurge(id, runsDeleted, cloudSyncQueueRunDeletion);
  if (result == -1) { sendError(404, "Robô não encontrado. Atualize a página."); return; }
  if (result == -2) { sendError(409, "Este robô está selecionado para correr. Troque o robô antes de excluir."); return; }
  if (result == -3) { sendError(409, "Há uma corrida em andamento. Espere terminar para excluir."); return; }
  CloudRenameResult cloud = cloudSyncFlushDeletes();
  JsonDocument out;
  out["runs_deleted"] = runsDeleted;
  out["cloud_delete"] = cloud == CLOUD_RENAME_SYNCED ? "synced" : cloud == CLOUD_RENAME_PENDING ? "pending" : "off";
  sendJsonDoc(200, out);
}

// ---------------------------------------------------------------------
// Competições
// ---------------------------------------------------------------------

static void writeCompetitionJson(JsonObject o, const Competition* c) {
  o["id"] = c->id;
  o["name"] = c->name;
  o["num_days"] = c->numDays;
  o["attempts_per_day"] = c->attemptsPerDay;
  o["status"] = c->status;
  o["current_day"] = c->currentDay;
}

static void handleGetCompetitions() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  int n = competitionCount();
  for (int i = 0; i < n; i++) {
    writeCompetitionJson(arr.add<JsonObject>(), competitionAt(i));
  }
  sendJsonDoc(200, doc);
}

static void handlePostCompetition() {
  JsonDocument in;
  readJsonBody(in);
  if (!in["name"].is<const char*>() || strlen((const char*)in["name"]) == 0) {
    sendError(400, "Informe o nome.");
    return;
  }
  const char* name = in["name"];
  int numDays = in["num_days"] | 1;
  int attemptsPerDay = in["attempts_per_day"] | 1;

  int id = competitionCreate(name, numDays, attemptsPerDay);
  if (id < 0) {
    sendError(400, "Limite de competições do cronômetro atingido.");
    return;
  }
  JsonDocument out;
  writeCompetitionJson(out.to<JsonObject>(), competitionById(id));
  sendJsonDoc(201, out);
}

static void handleGetCompetition(int id) {
  const Competition* c = competitionById(id);
  if (!c) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }
  JsonDocument out;
  writeCompetitionJson(out.to<JsonObject>(), c);
  sendJsonDoc(200, out);
}


static void handlePatchCompetition(int id) {
  JsonDocument in;
  readJsonBody(in);

  // Nome: pode trocar a qualquer momento.
  if (in["name"].is<const char*>()) {
    int r = competitionRename(id, in["name"]);
    if (r == -1) {
      sendError(404, "Competição não encontrada. Atualize a página.");
      return;
    }
    if (r == -4) {
      sendError(400, "Nome da competição vazio ou com aspas.");
      return;
    }
    cloudSyncCompetitionStatus(id); // ranking online passa a mostrar o nome novo
  }
  // Dias/tomadas: só antes da primeira corrida.
  if (!in["num_days"].is<int>() && !in["attempts_per_day"].is<int>()) {
    JsonDocument out;
    writeCompetitionJson(out.to<JsonObject>(), competitionById(id));
    sendJsonDoc(200, out);
    return;
  }
  int numDays = in["num_days"] | 1;
  int attemptsPerDay = in["attempts_per_day"] | 1;

  int result = competitionUpdate(id, numDays, attemptsPerDay);
  if (result == -1) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }
  if (result == -2) {
    sendError(409, "Esta competição já tem corridas. Não dá mais para mudar dias e tomadas.");
    return;
  }
  JsonDocument out;
  writeCompetitionJson(out.to<JsonObject>(), competitionById(id));
  sendJsonDoc(200, out);
}

static void handleGetCompetitionParticipants(int competitionId) {
  if (!competitionById(competitionId)) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }
  int ids[MAX_PARTICIPANTS];
  int n = linkedParticipantIds(competitionId, ids, MAX_PARTICIPANTS);

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    const Participant* p = participantById(ids[i]);
    if (!p) continue;
    writeParticipantJson(arr.add<JsonObject>(), p);
  }
  sendJsonDoc(200, doc);
}

static void handlePostCompetitionParticipant(int competitionId) {
  JsonDocument in;
  readJsonBody(in);

  // Vários de uma vez (tela "Adicionar robôs") - um só
  // regravamento de vinculos.csv em vez de um por robô.
  if (in["participant_ids"].is<JsonArray>()) {
    JsonArray arr = in["participant_ids"];
    int n = arr.size();
    if (n == 0 || n > MAX_PARTICIPANTS) {
      sendError(400, "Marque pelo menos um robô.");
      return;
    }
    int* ids = new (std::nothrow) int[n];
    if (!ids) {
      sendError(500, "Algo deu errado. Atualize a página e tente de novo.");
      return;
    }
    for (int i = 0; i < n; i++) ids[i] = arr[i] | -1;
    bool full = false;
    int added = linkCreateMany(competitionId, ids, n, full);
    delete[] ids;
    if (added < 0) {
      sendError(404, "Competição não encontrada. Atualize a página.");
      return;
    }
    JsonDocument out;
    out["added"] = added;
    out["capacity_reached"] = full;
    sendJsonDoc(201, out);
    return;
  }

  if (!in["participant_id"].is<int>()) {
    sendError(400, "Algo deu errado. Atualize a página e tente de novo.");
    return;
  }
  int participantId = in["participant_id"];

  if (!competitionById(competitionId)) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }
  if (!participantById(participantId)) {
    sendError(404, "Robô não encontrado. Atualize a página.");
    return;
  }

  bool already = linkExists(competitionId, participantId);
  linkCreate(competitionId, participantId);

  JsonDocument out;
  out["competition_id"] = competitionId;
  out["participant_id"] = participantId;
  out["already_linked"] = already;
  sendJsonDoc(201, out);
}

// Importação da lista de equipes/robôs (cadastro geral, tela "Equipes e
// robôs") a partir de um arquivo CSV/JSON - só cadastra, não inscreve em
// competição nenhuma. O navegador lê o arquivo e manda já normalizado:
//   { "participants": [ { "team": "...", "name": "...", "code": "..." }, ... ] }
// Resposta: resumo + status por linha (mesma ordem do envio), pra página
// mostrar quais linhas falharam.
static const int MAX_IMPORT_ROWS = MAX_PARTICIPANTS * 2; // folga pra linhas repetidas/inválidas

static void handlePostParticipantsImport() {
  JsonDocument in;
  if (!readJsonBody(in) || !in["participants"].is<JsonArray>()) {
    sendError(400, "Não foi possível ler a lista do arquivo.");
    return;
  }
  JsonArray list = in["participants"];
  int n = list.size();
  if (n == 0) {
    sendError(400, "Nenhum robô na lista.");
    return;
  }
  if (n > MAX_IMPORT_ROWS) {
    sendError(400, "A lista é grande demais. Divida em arquivos menores.");
    return;
  }

  // Heap, só durante a importação (~2 KB): nem a pilha do loop() nem a
  // RAM estática do firmware (já no limite) têm folga pra isso.
  RosterImportRow* rows = new (std::nothrow) RosterImportRow[n];
  int8_t* status = new (std::nothrow) int8_t[n];
  if (!rows || !status) {
    delete[] rows;
    delete[] status;
    sendError(500, "O cronômetro não conseguiu processar o arquivo. Tente um arquivo menor.");
    return;
  }
  for (int i = 0; i < n; i++) {
    JsonObject o = list[i];
    rows[i].team = o["team"] | "";
    rows[i].name = o["name"] | "";
    rows[i].code = o["code"] | "";
  }

  RosterImportSummary summary;
  rosterImport(rows, n, status, summary);

  JsonDocument out;
  out["teams_created"] = summary.teamsCreated;
  out["participants_created"] = summary.participantsCreated;
  out["participants_existing"] = summary.participantsExisting;
  out["failed"] = summary.failed;
  JsonArray rowsOut = out["rows"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    const char* s = "created";
    switch (status[i]) {
      case ROSTER_EXISTING: s = "existing"; break;
      case ROSTER_INVALID: s = "invalid"; break;
      case ROSTER_FULL: s = "full"; break;
      default: break;
    }
    rowsOut.add(s);
  }
  delete[] rows;
  delete[] status;
  sendJsonDoc(200, out);
}

static void handleDeleteCompetitionParticipant(int competitionId, int participantId) {
  if (raceControlIsArmed() && raceControlCompetitionId() == competitionId &&
      raceControlParticipantId() == participantId) {
    sendError(409, "Este robô está selecionado para correr. Troque o robô antes de remover.");
    return;
  }
  if (participantHasRunsInCompetition(competitionId, participantId)) {
    sendError(409, "Este robô já correu nesta competição e não pode ser removido dela.");
    return;
  }
  linkDelete(competitionId, participantId);
  sendNoContent();
}

static JsonCache g_dayStatusCache;

static void handleGetDayStatus(int competitionId) {
  const Competition* c = competitionById(competitionId);
  if (!c) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }
  if (cacheServe(g_dayStatusCache, competitionId)) return;

  static DayStatusParticipant rows[MAX_PARTICIPANTS];
  bool dayComplete = false;
  bool isLastDay = false;
  int n = competitionDayStatus(competitionId, rows, MAX_PARTICIPANTS, dayComplete, isLastDay);

  JsonDocument doc;
  doc["current_day"] = c->currentDay;
  doc["num_days"] = c->numDays;
  doc["attempts_per_day"] = c->attemptsPerDay;
  doc["day_complete"] = dayComplete;
  doc["is_last_day"] = isLastDay;
  JsonArray parts = doc["participants"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = parts.add<JsonObject>();
    o["participant_id"] = rows[i].participantId;
    o["name"] = rows[i].name;
    o["attempts_done"] = rows[i].attemptsDone;
    o["attempts_per_day"] = c->attemptsPerDay;
    o["done"] = rows[i].done;
    const Participant* p = participantById(rows[i].participantId);
    const Team* t = p ? teamById(p->teamId) : nullptr;
    o["team_name"] = t ? t->name : "";
  }
  cacheFill(g_dayStatusCache, competitionId, doc);
}

static void handlePostAdvanceDay(int competitionId) {
  bool finished = false;
  int currentDay = -1;
  if (!competitionAdvanceDay(competitionId, finished, currentDay)) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }

  // Avisa a nuvem do novo status - sem isto, uma
  // competição que termina aqui (sem mais nenhuma corrida depois pra
  // "carregar" o aviso via cloudSyncTryNow) nunca teria outra chance de
  // avisar - o dashboard ficaria "Em andamento" pra sempre.
  cloudSyncCompetitionStatus(competitionId);

  JsonDocument doc;
  doc["finished"] = finished;
  doc["current_day"] = currentDay;
  sendJsonDoc(200, doc);
}

static JsonCache g_rankingCache;

static void handleGetRanking(int competitionId) {
  if (!competitionById(competitionId)) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }
  if (cacheServe(g_rankingCache, competitionId)) return;

  static RankingRow rows[MAX_RANKING_ROWS];
  int n = computeRanking(competitionId, rows, MAX_RANKING_ROWS);

  JsonDocument doc;
  doc["competition_id"] = competitionId;
  doc["criteria"] = "best_time";
  JsonArray arr = doc["ranking"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["position"] = rows[i].position;
    o["participant_id"] = rows[i].participantId;
    o["participant_name"] = rows[i].participantName;
    o["external_code"] = rows[i].externalCode;
    o["best_elapsed_us"] = rows[i].bestElapsedUs;
    const Participant* pp = participantById(rows[i].participantId);
    const Team* tt = pp ? teamById(pp->teamId) : nullptr;
    o["team_name"] = tt ? tt->name : "";
    JsonArray attempts = o["attempts"].to<JsonArray>();
    for (int j = 0; j < rows[i].attemptCount; j++) {
      const AttemptCell &c = rows[i].attempts[j];
      if (!c.present) { attempts.add(nullptr); continue; }
      JsonObject a = attempts.add<JsonObject>();
      a["day"] = c.day;
      a["attempt"] = c.attempt;
      a["elapsed_us"] = c.elapsedUs;
      switch (c.statusCode) {
        case 'V': a["status"] = "valid"; break;
        case 'I': a["status"] = "invalid"; break;
        case 'D': a["status"] = "dnf"; break;
        case 'P': a["status"] = "pending_validation"; break;
        default:  a["status"] = "unknown"; break;
      }
      a["is_best"] = c.isBest;
    }
  }
  cacheFill(g_rankingCache, competitionId, doc);
}

// runId opaco (não é um id de banco — não existe mais banco): codifica
// competição/dia/linha-no-jsonl-do-dia para permitir localizar de volta
// a corrida exata em PATCH .../validation sem precisar de um índice
// separado. Ver storage.h (StoredRun) e handlePatchRunValidation abaixo.
static int encodeRunId(int competitionId, int day, int lineIndex) {
  return competitionId * 1000000 + day * 10000 + lineIndex;
}

static JsonCache g_runsCache;

static void handleGetRuns(int competitionId) {
  const Competition* c = competitionById(competitionId);
  if (!c) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }
  if (cacheServe(g_runsCache, competitionId)) return;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  StoredRun* rows = storageDayRunsScratch(); // buffer compartilhado (ver storage.h) — cada dia é totalmente convertido para JSON antes do próximo
  int totalEmitted = 0;
  for (int day = 1; day <= c->numDays && totalEmitted < MAX_RUNS_LIST_TOTAL; day++) {
    int n = storageReadDayRuns(c->slug, day, rows, MAX_RUNS_PER_DAY);
    for (int i = 0; i < n && totalEmitted < MAX_RUNS_LIST_TOTAL; i++) {
      const Participant* p = participantById(rows[i].participantId);
      JsonObject o = arr.add<JsonObject>();
      o["id"] = encodeRunId(competitionId, day, i);
      o["event_id"] = rows[i].eventId;
      o["competition_id"] = rows[i].competitionId;
      o["participant_id"] = rows[i].participantId;
      o["participant_name"] = p ? p->name : "?";
      o["day"] = rows[i].day;
      o["attempt"] = rows[i].attempt;
      o["start_timestamp_us"] = rows[i].startTimestampUs;
      o["end_timestamp_us"] = rows[i].endTimestampUs;
      o["elapsed_us"] = rows[i].elapsedUs;
      o["status"] = rows[i].status;
      totalEmitted++;
    }
  }

  // Mescla corridas presas na fila de pendências (flash indisponível
  // no momento) - sem isso o juiz nunca veria essa corrida pra
  // validar. Id negativo: nunca colide com encodeRunId() (sempre >= 0).
  for (int i = 0; i < MAX_PENDING_RUNS && totalEmitted < MAX_RUNS_LIST_TOTAL; i++) {
    RunRecord* rec = storagePendingRecordAt(i);
    RunContext* pctx = storagePendingContextAt(i);
    if (!rec || !pctx || pctx->competitionId != competitionId) continue;

    const Participant* p = participantById(pctx->participantId);
    JsonObject o = arr.add<JsonObject>();
    o["id"] = -(i + 1);
    o["event_id"] = rec->eventId;
    o["competition_id"] = pctx->competitionId;
    o["participant_id"] = pctx->participantId;
    o["participant_name"] = p ? p->name : pctx->participantName;
    o["day"] = pctx->day;
    o["attempt"] = pctx->attempt;
    o["start_timestamp_us"] = rec->startTimestampUs;
    o["end_timestamp_us"] = rec->endTimestampUs;
    o["elapsed_us"] = rec->elapsedUs;
    o["status"] = rec->status;
    totalEmitted++;
  }

  cacheFill(g_runsCache, competitionId, doc);
}

static void handlePatchPendingValidation(int pendingIndex) {
  RunRecord* record = storagePendingRecordAt(pendingIndex);
  RunContext* ctx = storagePendingContextAt(pendingIndex);
  if (!record || !ctx) {
    sendError(404, "Corrida não encontrada. Atualize a página.");
    return;
  }

  JsonDocument in;
  readJsonBody(in);
  const char* status = in["status"] | "";
  bool isRetry = (strcmp(status, "retry") == 0);
  bool validStatus = (strcmp(status, "valid") == 0 || strcmp(status, "invalid") == 0 || strcmp(status, "dnf") == 0 || isRetry);
  if (!validStatus) {
    sendError(400, "Algo deu errado. Atualize a página e tente de novo.");
    return;
  }
  const char* storedStatus = isRetry ? "retried" : status;

  storagePendingSetStatus(pendingIndex, storedStatus); // "retried", não "retry" - igual ao caminho normal

  // Copia os campos ANTES do flush — se persistir com sucesso agora, a
  // fila libera esta posição (ver storageFlushPending()) e os ponteiros
  // 'record'/'ctx' deixam de ser válidos.
  int respParticipantId = ctx->participantId;
  int respCompetitionId = ctx->competitionId;
  int respDay = ctx->day;
  int respAttempt = record->attempt;
  int64_t respElapsedUs = record->elapsedUs;
  char respEventId[48];
  strlcpy(respEventId, record->eventId, sizeof(respEventId));
  char respParticipantName[40];
  strlcpy(respParticipantName, ctx->participantName, sizeof(respParticipantName));

  if (isRetry) {
    // Mesma regra do caminho normal (handlePatchRunValidation): rearma
    // a MESMA tentativa e para aqui - NUNCA avança pra próxima (esse
    // era o bug: antes desta correção, retry sempre caía direto no
    // maybeAdvanceAttempt() abaixo, que arma a próxima tomada em vez de
    // refazer esta).
    ledSetRed(false);
    idleScreenValidationResolved();
    raceControlArm(respCompetitionId, respParticipantId, respDay, respAttempt);
    JsonDocument out;
    out["id"] = -(pendingIndex + 1);
    out["status"] = "retried";
    out["rearmed"] = true;
    sendJsonDoc(200, out);
    return;
  }

  ledSetRed(false); // veredito dado - não fica mais esperando (faltava neste caminho)
  idleScreenValidationResolved();

  // Reenvia pra nuvem com o veredito final - mesmo
  // motivo do caminho normal (handlePatchRunValidation) - ANTES do
  // flush, enquanto 'record'/'ctx' ainda apontam pra esta posição.
  cloudSyncTryNow(*record, *ctx);

  storageFlushPending(); // melhor esforço - persiste de vez se a flash já voltou

  // Tomada consumida (independente do veredito) -> arma a próxima
  // automaticamente, se houver (mesma regra do caminho normal, ver
  // handlePatchRunValidation abaixo).
  maybeAdvanceAttempt(respCompetitionId, respParticipantId, respDay);

  JsonDocument out;
  out["id"] = -(pendingIndex + 1);
  out["event_id"] = respEventId;
  out["competition_id"] = respCompetitionId;
  out["participant_id"] = respParticipantId;
  out["participant_name"] = respParticipantName;
  out["day"] = respDay;
  out["attempt"] = respAttempt;
  out["elapsed_us"] = respElapsedUs;
  out["status"] = status;
  sendJsonDoc(200, out);
}

static void handlePatchRunValidation(int runId) {
  // Id negativo = corrida presa na fila de pendências em RAM, ainda não
  // persistida (ver handleGetRuns) - caminho totalmente
  // separado, não decodifica pelo esquema de encodeRunId() abaixo.
  if (runId < 0) {
    handlePatchPendingValidation(-runId - 1);
    return;
  }

  int competitionId = runId / 1000000;
  int rem = runId % 1000000;
  int day = rem / 10000;
  int lineIndex = rem % 10000;

  const Competition* c = competitionById(competitionId);
  if (!c) {
    sendError(404, "Corrida não encontrada. Atualize a página.");
    return;
  }

  JsonDocument in;
  readJsonBody(in);
  const char* status = in["status"] | "";
  bool isRetry = (strcmp(status, "retry") == 0);
  bool validStatus = (strcmp(status, "valid") == 0 || strcmp(status, "invalid") == 0 || strcmp(status, "dnf") == 0 || isRetry);
  if (!validStatus) {
    sendError(400, "Algo deu errado. Atualize a página e tente de novo.");
    return;
  }
  const char* storedStatus = isRetry ? "retried" : status;

  // Correção: juiz troca o veredito de uma corrida já decidida,
  // pela lista de corridas. Só grava - não mexe em LED, display nem arma
  // a próxima tomada (outro robô pode estar na pista).
  bool correction = in["correction"] | false;
  if (correction && isRetry) {
    sendError(400, "Algo deu errado. Atualize a página e tente de novo.");
    return;
  }
  if (correction && strcmp(c->status, "finished") == 0) {
    sendError(409, "A competição já foi encerrada. Não dá mais para mudar resultados.");
    return;
  }

  StoredRun* rows = storageDayRunsScratch(); // buffer compartilhado (ver storage.h)
  int n = storageReadDayRuns(c->slug, day, rows, MAX_RUNS_PER_DAY);
  if (lineIndex < 0 || lineIndex >= n) {
    sendError(404, "Corrida não encontrada. Atualize a página.");
    return;
  }

  // O id da corrida é a POSIÇÃO dela no arquivo do dia; excluir um robô
  // apaga linhas e desloca as seguintes. A página manda também o
  // event_id (único de verdade) - se não bater, a lista que ela tinha está
  // velha e o veredito iria pra corrida errada.
  const char* expectedEventId = in["event_id"] | "";
  if (expectedEventId[0] != '\0' && strcmp(expectedEventId, rows[lineIndex].eventId) != 0) {
    sendError(409, "A lista de corridas mudou. Confira e tente de novo.");
    return;
  }

  strlcpy(rows[lineIndex].status, storedStatus, sizeof(rows[lineIndex].status));
  storageRewriteDayJsonl(c->slug, day, rows, n);

  // Regrava também o CSV legível, com os nomes já resolvidos localmente.
  static DayResultRow csvRows[MAX_RUNS_PER_DAY];
  for (int i = 0; i < n; i++) {
    const Participant* p = participantById(rows[i].participantId);
    strlcpy(csvRows[i].participantName, p ? p->name : "?", sizeof(csvRows[i].participantName));
    csvRows[i].elapsedUs = rows[i].elapsedUs;
    strlcpy(csvRows[i].status, rows[i].status, sizeof(csvRows[i].status));
  }
  storageRewriteDayCsv(c->slug, day, csvRows, n);

  if (isRetry) {
    ledSetRed(false); // juiz decidiu - não fica mais esperando veredito
    idleScreenValidationResolved();
    raceControlArm(competitionId, rows[lineIndex].participantId, rows[lineIndex].day, rows[lineIndex].attempt);
    JsonDocument out;
    out["id"] = runId;
    out["status"] = "retried";
    out["rearmed"] = true;
    sendJsonDoc(200, out);
    return;
  }

  // Copia os campos da linha para variáveis locais ANTES de chamar
  // maybeAdvanceAttempt() — essa função também lê o dia via
  // storageDayRunsScratch() (mesmo buffer compartilhado), então 'rows'
  // não pode mais ser usado com segurança depois desta chamada.
  int respParticipantId = rows[lineIndex].participantId;
  int respCompetitionId = rows[lineIndex].competitionId;
  int respDay = rows[lineIndex].day;
  int respAttempt = rows[lineIndex].attempt;
  int64_t respElapsedUs = rows[lineIndex].elapsedUs;
  int64_t respStartTimestampUs = rows[lineIndex].startTimestampUs;
  int64_t respEndTimestampUs = rows[lineIndex].endTimestampUs;
  char respEventId[48];
  strlcpy(respEventId, rows[lineIndex].eventId, sizeof(respEventId));
  char respStatus[24];
  strlcpy(respStatus, rows[lineIndex].status, sizeof(respStatus));

  if (!correction) {
    ledSetRed(false); // veredito (valid/invalid/dnf) dado - não fica mais esperando
    idleScreenValidationResolved();
  }
  const Participant* validatedParticipant = participantById(respParticipantId);

  // Reenvia pra nuvem com o veredito final - sem isso,
  // a corrida ficaria "pending_validation" pra sempre do lado de lá,
  // já que cloudSyncTryNow só era chamado uma vez, no instante em que a
  // corrida termina (antes do juiz validar). Mesma janela seguraque o
  // resto (não existe corrida armada enquanto uma validação está
  // pendente - ver derivePhase() em pagina/index.html).
  RunRecord syncRecord;
  strlcpy(syncRecord.eventId, respEventId, sizeof(syncRecord.eventId));
  syncRecord.competitionId = respCompetitionId;
  syncRecord.participantId = respParticipantId;
  syncRecord.day = respDay;
  syncRecord.attempt = respAttempt;
  syncRecord.startTimestampUs = respStartTimestampUs;
  syncRecord.endTimestampUs = respEndTimestampUs;
  syncRecord.elapsedUs = respElapsedUs;
  strlcpy(syncRecord.status, respStatus, sizeof(syncRecord.status));

  RunContext syncCtx;
  strlcpy(syncCtx.competitionSlug, c->slug, sizeof(syncCtx.competitionSlug));
  strlcpy(syncCtx.participantName, validatedParticipant ? validatedParticipant->name : "?", sizeof(syncCtx.participantName));
  syncCtx.competitionId = respCompetitionId;
  syncCtx.participantId = respParticipantId;
  syncCtx.day = respDay;
  syncCtx.attempt = respAttempt;
  syncCtx.hasContext = true;
  cloudSyncTryNow(syncRecord, syncCtx);

  // Tomada consumida (independente do veredito) -> arma a próxima
  // automaticamente, se houver (mesma regra do servidor antigo).
  if (!correction) maybeAdvanceAttempt(competitionId, respParticipantId, day);

  const Participant* p = validatedParticipant;
  JsonDocument out;
  out["id"] = runId;
  out["event_id"] = respEventId;
  out["competition_id"] = respCompetitionId;
  out["participant_id"] = respParticipantId;
  out["participant_name"] = p ? p->name : "?";
  out["day"] = respDay;
  out["attempt"] = respAttempt;
  out["elapsed_us"] = respElapsedUs;
  out["status"] = respStatus;
  sendJsonDoc(200, out);
}

// ---------------------------------------------------------------------
// Configuração do modo Online — tela de Configurações da
// página local do juiz.
// ---------------------------------------------------------------------

static void writeCloudConfigJson(JsonObject o) {
  CloudSyncConfig cfg = cloudSyncGetConfig();
  o["base_url"] = cfg.baseUrl;
  // A chave nunca volta pra página (igual à senha do Wi-Fi): quem pegar o
  // celular do juiz não consegue copiá-la e mandar resultados falsos.
  o["has_token"] = cfg.deviceToken[0] != '\0';
  // "enabled" calculado, não configurável - Online é
  // simplesmente ter os dois campos preenchidos.
  o["online_active"] = (strlen(cfg.baseUrl) > 0 && strlen(cfg.deviceToken) > 0);
}

static void handleGetCloudConfig() {
  JsonDocument doc;
  writeCloudConfigJson(doc.to<JsonObject>());
  sendJsonDoc(200, doc);
}

static void handlePostCloudConfig() {
  JsonDocument in;
  readJsonBody(in);

  CloudSyncConfig cfg;
  strlcpy(cfg.baseUrl, in["base_url"] | "", sizeof(cfg.baseUrl));
  strlcpy(cfg.deviceToken, in["device_token"] | "", sizeof(cfg.deviceToken));
  // Chave em branco = manter a salva; endereço em branco = desligar tudo.
  if (cfg.deviceToken[0] == '\0' && cfg.baseUrl[0] != '\0') {
    strlcpy(cfg.deviceToken, cloudSyncGetConfig().deviceToken, sizeof(cfg.deviceToken));
  }

  cloudSyncSetConfig(cfg);
  cloudSyncFlushRenames(); // acabou de ficar Online? manda o que estava pendente
  cloudSyncFlushDeletes();

  JsonDocument out;
  writeCloudConfigJson(out.to<JsonObject>());
  sendJsonDoc(200, out);
}

static void handlePostCloudConfigTest() {
  char msg[96];
  bool ok = cloudSyncTestAuth(msg, sizeof(msg));
  JsonDocument doc;
  doc["ok"] = ok;
  doc["message"] = msg;
  sendJsonDoc(200, doc);
}

// ---------------------------------------------------------------------
// Rede Wi-Fi — rede externa (WPA2 pessoal / Enterprise), canal
// do AP próprio e status, na tela de Configurações. A senha salva nunca
// volta pra página (só "has_password").
// ---------------------------------------------------------------------

static const char* staSecurityName(uint8_t security) {
  if (security == WIFI_STA_PERSONAL) return "personal";
  if (security == WIFI_STA_ENTERPRISE) return "enterprise";
  return "none";
}

static void writeWifiConfigJson(JsonObject o) {
  WifiStaConfig cfg = wifiGetStaConfig();
  o["security"] = staSecurityName(cfg.security);
  o["ssid"] = cfg.ssid;
  o["username"] = cfg.username;
  o["eap_method"] = cfg.eapMethod == WIFI_EAP_TTLS ? "ttls" : "peap";
  o["has_password"] = cfg.password[0] != '\0';
  o["ap_channel_setting"] = wifiGetApChannelSetting();

  JsonObject st = o["status"].to<JsonObject>();
  static const char* STATE_NAMES[] = { "off", "connecting", "connected", "waiting" };
  st["state"] = STATE_NAMES[wifiStaState()];
  st["ssid"] = wifiStaSsid();
  char ip[20];
  wifiStaIp(ip, sizeof(ip));
  st["ip"] = ip;
  st["rssi"] = wifiStaRssi();
  st["last_error"] = wifiLastError();
  st["retry_in_s"] = wifiStaSecondsToRetry();
  st["channel"] = wifiCurrentChannel();
  st["ap_ssid"] = WIFI_AP_SSID;
  st["ap_password"] = wifiApPassword(); // rota protegida (só o juiz vê)
  wifiApIp(ip, sizeof(ip));
  st["ap_ip"] = ip;
  st["ap_clients"] = wifiApClientCount();
}

static void handleGetWifiConfig() {
  JsonDocument doc;
  writeWifiConfigJson(doc.to<JsonObject>());
  sendJsonDoc(200, doc);
}

static void handlePostWifiConfig() {
  JsonDocument in;
  if (!readJsonBody(in)) {
    sendError(400, "Algo deu errado. Atualize a página e tente de novo.");
    return;
  }

  if (in["ap_channel_setting"].is<int>()) {
    int ch = in["ap_channel_setting"];
    if (ch < 0 || ch > 13) {
      sendError(400, "Algo deu errado. Atualize a página e tente de novo.");
      return;
    }
    wifiSetApChannelSetting(ch);
  }

  if (in["security"].is<const char*>()) {
    WifiStaConfig current = wifiGetStaConfig();
    WifiStaConfig cfg = {};
    const char* sec = in["security"];
    if (strcmp(sec, "personal") == 0) cfg.security = WIFI_STA_PERSONAL;
    else if (strcmp(sec, "enterprise") == 0) cfg.security = WIFI_STA_ENTERPRISE;
    else cfg.security = WIFI_STA_NONE;
    cfg.eapMethod = strcmp(in["eap_method"] | "peap", "ttls") == 0 ? WIFI_EAP_TTLS : WIFI_EAP_PEAP;
    strlcpy(cfg.ssid, in["ssid"] | "", sizeof(cfg.ssid));
    strlcpy(cfg.username, in["username"] | "", sizeof(cfg.username));
    strlcpy(cfg.password, in["password"] | "", sizeof(cfg.password));

    if (cfg.security != WIFI_STA_NONE && cfg.ssid[0] == '\0') {
      sendError(400, "Informe o nome da rede.");
      return;
    }
    if (cfg.security == WIFI_STA_ENTERPRISE && cfg.username[0] == '\0') {
      sendError(400, "Informe o usuário (RA ou e-mail).");
      return;
    }
    // Senha em branco = manter a salva, se for a mesma rede/mesmo tipo.
    if (cfg.password[0] == '\0' && cfg.security == current.security && strcmp(cfg.ssid, current.ssid) == 0) {
      strlcpy(cfg.password, current.password, sizeof(cfg.password));
    }
    if (cfg.security == WIFI_STA_ENTERPRISE && cfg.password[0] == '\0') {
      sendError(400, "Informe a senha.");
      return;
    }
    wifiSetStaConfig(cfg);
  }

  JsonDocument out;
  writeWifiConfigJson(out.to<JsonObject>());
  sendJsonDoc(200, out);
}

// GET /wifi-scan            -> resultado atual (ou "scanning")
// GET /wifi-scan?refresh=1  -> começa uma busca nova
static void handleGetWifiScan() {
  if (server.hasArg("refresh")) wifiScanStart();

  int n = wifiScanResultCount();
  JsonDocument doc;
  doc["scanning"] = (n == -1);
  doc["failed"] = (n == -2);
  JsonArray arr = doc["networks"].to<JsonArray>();
  WifiScanEntry e;
  for (int i = 0; i < n; i++) {
    if (!wifiScanResultAt(i, e)) break;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = e.ssid;
    o["rssi"] = e.rssi;
    o["channel"] = e.channel;
    o["enterprise"] = e.enterprise;
    o["open"] = e.open;
  }
  sendJsonDoc(200, doc);
}

// ---------------------------------------------------------------------
// Exportação (JSON e CSV) — buffer de leitura de corridas COMPARTILHADO
// entre as duas rotas abaixo. Nunca rodam ao mesmo tempo (um único
// request por vez), então não faz sentido cada handler ter o seu -
// isso já causou overflow de DRAM (~33KB pra duas cópias de 150
// StoredRun cada, quando 1 já basta).
// ---------------------------------------------------------------------
static StoredRun exportDayRunsBuf[MAX_RUNS_PER_DAY];

// ---------------------------------------------------------------------
// Exportação em CSV — uma linha por corrida, achatada (equipe/robô já
// resolvidos por nome), pronta pra abrir direto no Excel/Sheets. Ao
// contrário do /export (JSON, estrutura relacional completa), este é
// só a tabela de resultados.
// ---------------------------------------------------------------------

static void handleGetExportCsv() {
  String out;
  out.reserve(4096);
  out += "equipe,robo,competicao,dia,tentativa,tempo,status\n";

  char timeStr[16];

  for (int i = 0; i < competitionCount(); i++) {
    const Competition* c = competitionAt(i);
    for (int day = 1; day <= c->numDays; day++) {
      int n = storageReadDayRuns(c->slug, day, exportDayRunsBuf, MAX_RUNS_PER_DAY);
      for (int j = 0; j < n; j++) {
        const Participant* part = participantById(exportDayRunsBuf[j].participantId);
        const Team* team = part ? teamById(part->teamId) : nullptr;
        formatElapsedTime(exportDayRunsBuf[j].elapsedUs, timeStr, sizeof(timeStr));

        out += "\"";
        out += (team ? team->name : "");
        out += "\",\"";
        out += (part ? part->name : "");
        out += "\",\"";
        out += c->name;
        out += "\",";
        out += String(exportDayRunsBuf[j].day);
        out += ",";
        out += String(exportDayRunsBuf[j].attempt);
        out += ",";
        out += timeStr;
        out += ",";
        out += exportDayRunsBuf[j].status;
        out += "\n";
      }
    }
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"cronometro_export.csv\"");
  server.send(200, "text/csv", out);
}

// ---------------------------------------------------------------------
// Exportação pra download  — junta tudo (equipes,
// participantes, competições e todas as corridas) num único JSON pro
// navegador baixar.
// ---------------------------------------------------------------------

static void handleGetExport() {
  JsonDocument doc;

  JsonArray teamsArr = doc["teams"].to<JsonArray>();
  for (int i = 0; i < teamCount(); i++) writeTeamJson(teamsArr.add<JsonObject>(), teamAt(i));

  JsonArray partsArr = doc["participants"].to<JsonArray>();
  for (int i = 0; i < participantCount(); i++) writeParticipantJson(partsArr.add<JsonObject>(), participantAt(i));

  JsonArray compsArr = doc["competitions"].to<JsonArray>();
  for (int i = 0; i < competitionCount(); i++) {
    const Competition* c = competitionAt(i);
    JsonObject co = compsArr.add<JsonObject>();
    co["id"] = c->id;
    co["name"] = c->name;
    co["status"] = c->status;
    co["num_days"] = c->numDays;
    co["attempts_per_day"] = c->attemptsPerDay;

    JsonArray runsArr = co["runs"].to<JsonArray>();
    for (int day = 1; day <= c->numDays; day++) {
      int n = storageReadDayRuns(c->slug, day, exportDayRunsBuf, MAX_RUNS_PER_DAY);
      for (int j = 0; j < n; j++) {
        JsonObject ro = runsArr.add<JsonObject>();
        ro["participant_id"] = exportDayRunsBuf[j].participantId;
        ro["day"] = exportDayRunsBuf[j].day;
        ro["attempt"] = exportDayRunsBuf[j].attempt;
        ro["elapsed_us"] = exportDayRunsBuf[j].elapsedUs;
        ro["status"] = exportDayRunsBuf[j].status;
      }
    }
  }

  String out;
  serializeJson(doc, out);
  server.sendHeader("Content-Disposition", "attachment; filename=\"cronometro_export.json\"");
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------
// Senha da página do juiz — ver checkJudgeAuth() acima.
// ---------------------------------------------------------------------

static void handleGetJudgeAuth() {
  JsonDocument doc;
  doc["enabled"] = strlen(g_judgePassword) > 0;
  sendJsonDoc(200, doc);
}

static void handlePostLogin() {
  if (g_authFailures >= AUTH_MAX_FAILURES) {
    if (millis() - g_authLockedAtMs < AUTH_LOCKOUT_MS) {
      sendError(429, "Muitas senhas erradas. Espere 30 segundos e tente de novo.");
      return;
    }
    g_authFailures = 0;
  }
  JsonDocument in;
  readJsonBody(in);
  String given = in["password"] | "";
  if (g_judgePassword[0] == '\0' || !sameSecret(given, g_judgePassword)) {
    if (g_judgePassword[0] != '\0' && ++g_authFailures >= AUTH_MAX_FAILURES) {
      g_authLockedAtMs = millis();
      Serial.println("[WEB] Senhas erradas demais - login bloqueado por 30 s.");
    }
    sendError(400, "Senha incorreta.");
    return;
  }
  g_authFailures = 0;
  JsonDocument out;
  out["token"] = g_judgeToken;
  sendJsonDoc(200, out);
}

static void handlePostJudgeAuth() {
  JsonDocument in;
  readJsonBody(in);
  const char* password = in["password"] | "";
  size_t len = strlen(password);
  if (len > 0 && len < 6) {
    sendError(400, "A senha precisa ter pelo menos 6 caracteres.");
    return;
  }
  if (len >= sizeof(g_judgePassword)) {
    sendError(400, "A senha pode ter no máximo 31 caracteres.");
    return;
  }
  judgeAuthSetPassword(password);

  JsonDocument out;
  out["enabled"] = strlen(g_judgePassword) > 0;
  out["token"] = g_judgeToken; // quem criou a senha continua dentro
  sendJsonDoc(200, out);
}

// ---------------------------------------------------------------------
// Arm / controle de corrida
// ---------------------------------------------------------------------

static void writeArmStateJson(JsonObject o) {
  bool armed = raceControlIsArmed();
  o["armed"] = armed;
  if (!armed) return;

  o["released"] = raceControlIsReleased();
  o["started"] = raceControlIsStarted();
  o["competition_id"] = raceControlCompetitionId();
  o["participant_id"] = raceControlParticipantId();
  o["day"] = raceControlDay();
  o["attempt"] = raceControlAttempt();

  const Participant* p = participantById(raceControlParticipantId());
  o["participant_name"] = p ? p->name : "?";
  const Competition* c = competitionById(raceControlCompetitionId());
  o["competition_name"] = c ? c->name : "?";
  if (raceControlIsStarted()) o["elapsed_us"] = timerElapsedUs(); // a página conta a partir daqui
}

static void handleGetArm() {
  JsonDocument doc;
  writeArmStateJson(doc.to<JsonObject>());
  sendJsonDoc(200, doc);
}

static void handlePostArm() {
  JsonDocument in;
  readJsonBody(in);
  int competitionId = in["competition_id"] | -1;
  int participantId = in["participant_id"] | -1;
  int day = in["day"] | -1;
  int attempt = in["attempt"] | -1;

  if (!competitionById(competitionId)) {
    sendError(404, "Competição não encontrada. Atualize a página.");
    return;
  }
  if (!participantById(participantId)) {
    sendError(404, "Robô não encontrado. Atualize a página.");
    return;
  }

  raceControlArm(competitionId, participantId, day, attempt);

  JsonDocument out;
  writeArmStateJson(out.to<JsonObject>());
  sendJsonDoc(200, out);
}

static void handlePostArmRelease() {
  if (!raceControlIsArmed()) {
    sendError(400, "Nenhum robô selecionado para liberar.");
    return;
  }
  raceControlRelease();
  JsonDocument doc;
  writeArmStateJson(doc.to<JsonObject>());
  sendJsonDoc(200, doc);
}

static void handlePostArmRetry() {
  if (!raceControlRequestRetry()) {
    sendError(400, "A corrida ainda não começou: o robô ainda não passou pela largada.");
    return;
  }
  JsonDocument doc;
  doc["retry_requested"] = true;
  sendJsonDoc(200, doc);
}

static void handlePostArmAbort() {
  if (!raceControlRequestAbort()) {
    sendError(400, "A corrida ainda não começou: o robô ainda não passou pela largada.");
    return;
  }
  JsonDocument doc;
  doc["abort_requested"] = true;
  sendJsonDoc(200, doc);
}

// Desarmar = o juiz selecionou o robô errado e quer voltar.
// Recusado com a corrida já em andamento - aí o caminho é abortar/repetir.
static void handleDeleteArm() {
  if (raceControlIsStarted()) {
    sendError(409, "A corrida já começou. Use Abortar ou Repetir tomada.");
    return;
  }
  raceControlDisarm();
  JsonDocument doc;
  doc["disarmed"] = true;
  sendJsonDoc(200, doc);
}

// ---------------------------------------------------------------------
// Roteador (chamado via server.onNotFound — ver comentário de splitPath)
// ---------------------------------------------------------------------

// Com senha ativa, qualquer um no Wi-Fi pode VER (ranking,
// corridas, Painel da TV); mexer em qualquer coisa e as telas de
// configuração/backup pedem a senha do juiz.
static bool isPublicRequest(const String &resource, HTTPMethod method) {
  if (resource == "login") return true;
  if (method != HTTP_GET) return false;
  return resource != "wifi-config" && resource != "wifi-scan" && resource != "cloud-config" &&
         resource != "export" && resource != "judge-auth";
}

static void handleApi() {
  sendSecurityHeaders(true);

  // Contra CSRF: um site qualquer aberto no celular do juiz não consegue
  // mandar este cabeçalho pro cronômetro (o navegador exige permissão
  // CORS, que o cronômetro nunca dá). Só a página do próprio cronômetro manda.
  if (server.method() != HTTP_GET && server.header("X-Cronometro") != "1") {
    sendError(403, "Algo deu errado. Atualize a página e tente de novo.");
    return;
  }

  String segs[8];
  int segCount = splitPath(server.uri(), segs, 8);
  HTTPMethod method = server.method();

  const String resource = segCount >= 3 ? segs[2] : String();
  if (!isPublicRequest(resource, method)) {
    // Quem só está olhando também manda o contexto do display; sem senha,
    // ignora calado em vez de abrir a caixa de login a cada 10 s.
    if (resource == "display-context" && !judgeSessionValid()) return sendNoContent();
    if (!checkJudgeAuth()) return;
  }

  if (segCount >= 3 && segs[0] == "api" && segs[1] == "v1") {

    if (resource == "participants") {
      if (segCount == 3 && method == HTTP_GET) return handleGetParticipants();
      if (segCount == 3 && method == HTTP_POST) return handlePostParticipant();
      if (segCount == 4 && segs[3] == "import" && method == HTTP_POST) return handlePostParticipantsImport();
      if (segCount == 4 && method == HTTP_DELETE) return handleDeleteParticipant(segs[3].toInt());
      if (segCount == 4 && method == HTTP_PATCH) return handlePatchParticipant(segs[3].toInt());
      if (segCount == 5 && segs[4] == "runs-summary" && method == HTTP_GET) return handleGetParticipantRunsSummary(segs[3].toInt());
    } else if (resource == "teams") {
      if (segCount == 3 && method == HTTP_GET) return handleGetTeams();
      if (segCount == 3 && method == HTTP_POST) return handlePostTeam();
      if (segCount == 4 && method == HTTP_PATCH) return handlePatchTeam(segs[3].toInt());
      if (segCount == 4 && method == HTTP_DELETE) return handleDeleteTeam(segs[3].toInt());
    } else if (resource == "competitions") {
      if (segCount == 3 && method == HTTP_GET) return handleGetCompetitions();
      if (segCount == 3 && method == HTTP_POST) return handlePostCompetition();
      if (segCount == 4 && method == HTTP_GET) return handleGetCompetition(segs[3].toInt());
      if (segCount == 4 && method == HTTP_PATCH) return handlePatchCompetition(segs[3].toInt());
      if (segCount == 5) {
        int id = segs[3].toInt();
        const String &sub = segs[4];
        if (sub == "participants" && method == HTTP_GET) return handleGetCompetitionParticipants(id);
        if (sub == "participants" && method == HTTP_POST) return handlePostCompetitionParticipant(id);
        if (sub == "day-status" && method == HTTP_GET) return handleGetDayStatus(id);
        if (sub == "advance-day" && method == HTTP_POST) return handlePostAdvanceDay(id);
        if (sub == "ranking" && method == HTTP_GET) return handleGetRanking(id);
        if (sub == "runs" && method == HTTP_GET) return handleGetRuns(id);
      }
      if (segCount == 6 && segs[4] == "participants" && method == HTTP_DELETE) {
        return handleDeleteCompetitionParticipant(segs[3].toInt(), segs[5].toInt());
      }
    } else if (resource == "runs") {
      if (segCount == 5 && segs[4] == "validation" && method == HTTP_PATCH) {
        return handlePatchRunValidation(segs[3].toInt());
      }
    } else if (resource == "arm") {
      if (segCount == 3 && method == HTTP_GET) return handleGetArm();
      if (segCount == 3 && method == HTTP_POST) return handlePostArm();
      if (segCount == 3 && method == HTTP_DELETE) return handleDeleteArm();
      if (segCount == 4 && segs[3] == "release" && method == HTTP_POST) return handlePostArmRelease();
      if (segCount == 4 && segs[3] == "abort" && method == HTTP_POST) return handlePostArmAbort();
      if (segCount == 4 && segs[3] == "retry" && method == HTTP_POST) return handlePostArmRetry();
    } else if (resource == "cloud-config") {
      if (segCount == 3 && method == HTTP_GET) return handleGetCloudConfig();
      if (segCount == 3 && method == HTTP_POST) return handlePostCloudConfig();
      if (segCount == 4 && segs[3] == "test" && method == HTTP_POST) return handlePostCloudConfigTest();
    } else if (resource == "display-context") {
      if (segCount == 3 && method == HTTP_POST) return handlePostDisplayContext();
    } else if (resource == "wifi-config") {
      if (segCount == 3 && method == HTTP_GET) return handleGetWifiConfig();
      if (segCount == 3 && method == HTTP_POST) return handlePostWifiConfig();
    } else if (resource == "wifi-scan") {
      if (segCount == 3 && method == HTTP_GET) return handleGetWifiScan();
    } else if (resource == "export") {
      if (segCount == 3 && method == HTTP_GET) {
        if (server.hasArg("format") && server.arg("format") == "csv") return handleGetExportCsv();
        return handleGetExport();
      }
    } else if (resource == "login") {
      if (segCount == 3 && method == HTTP_POST) return handlePostLogin();
    } else if (resource == "judge-auth") {
      if (segCount == 3 && method == HTTP_GET) return handleGetJudgeAuth();
      if (segCount == 3 && method == HTTP_POST) return handlePostJudgeAuth();
    }
  }

  sendError(404, "Algo deu errado. Atualize a página e tente de novo.");
}

static void handleRoot() {
  sendSecurityHeaders(false); // página aberta: as rotas é que pedem senha (ver isPublicRequest)
  // Página vai compactada: ~106 KB de HTML ocupam ~36 KB de
  // flash; todo navegador descompacta sozinho com Content-Encoding: gzip.
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

void webServerInit() {
  judgeAuthInit();
  if (wifiBootResetWasHeld() && g_judgePassword[0] != '\0') {
    judgeAuthSetPassword(""); // botão no boot também tira a senha - dono nunca fica trancado pra fora
    Serial.println("[WEB] Senha do painel removida pelo botao de reset.");
  }
  static const char* headerKeys[] = { "X-Cronometro", "X-Sessao" };
  server.collectHeaders(headerKeys, 2); // Authorization continua sendo coletado pelo próprio WebServer
  server.on("/", HTTP_GET, handleRoot);
  server.onNotFound(handleApi);
  server.begin(HTTP_SERVER_PORT);

  Serial.print("[WEB] Servidor HTTP local iniciado na porta ");
  Serial.println(HTTP_SERVER_PORT);
}

void webServerHandle() {
  server.handleClient();
}
