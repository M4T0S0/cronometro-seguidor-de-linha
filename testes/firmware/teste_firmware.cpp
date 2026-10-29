// Testes da lógica do firmware, rodando no computador (sem ESP32).
// Usa o código VERDADEIRO de firmware/cronometro; só a flash, a NVS, o
// relógio e o Serial são trocados pelas peças de testes/firmware/falsos.
// Rodar: sh testes/firmware/rodar.sh
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "LittleFS.h"
#include "Preferences.h"
#include "esp_timer.h"
#include "config.h"
#include "data_store.h"
#include "race_control.h"
#include "ranking.h"
#include "storage.h"

// ------------------------------------------------------------------
static int g_total = 0, g_falhas = 0;
#define CONFERE(cond) do { g_total++; if (!(cond)) { g_falhas++; \
  std::printf("    falhou na linha %d: %s\n", __LINE__, #cond); } } while (0)

struct Teste { const char* nome; void (*fn)(); };
static std::vector<Teste>& testes() { static std::vector<Teste> t; return t; }
#define TESTE(id, nome) static void id(); \
  static int reg_##id = (testes().push_back({nome, id}), 0); static void id()

// Cronômetro recém-ligado, com a memória vazia.
static void zerar() {
  fakeFiles().clear();
  fakeDirs().clear();
  fakePrefsInts().clear();
  raceControlInit();
  storageInit();
  dataStoreInit();
}

// Grava uma corrida do jeito que o firmware grava ao fim de uma tomada.
static void corrida(int compId, int partId, int day, int attempt, const char* status, double segundos) {
  const Competition* c = competitionById(compId);
  const Participant* p = participantById(partId);
  RunContext ctx = {};
  snprintf(ctx.competitionSlug, sizeof(ctx.competitionSlug), "%s", c->slug);
  snprintf(ctx.participantName, sizeof(ctx.participantName), "%s", p->name);
  ctx.competitionId = compId;
  ctx.participantId = partId;
  ctx.day = day;
  ctx.attempt = attempt;
  ctx.hasContext = true;

  RunRecord r = {};
  fakeTimeUs() += 60000000;  // cada corrida um minuto depois da anterior
  storageGenerateEventId(fakeTimeUs(), r.eventId, sizeof(r.eventId));
  r.competitionId = compId;
  r.participantId = partId;
  r.day = day;
  r.attempt = attempt;
  r.startTimestampUs = fakeTimeUs();
  r.elapsedUs = (int64_t)(segundos * 1000000);
  r.endTimestampUs = r.startTimestampUs + r.elapsedUs;
  snprintf(r.status, sizeof(r.status), "%s", status);
  storageAppendRun(r, ctx);
}

// Troca o status de uma corrida já gravada (o que o juiz faz ao validar).
static void decidir(int compId, int day, int linha, const char* status) {
  StoredRun* rows = storageDayRunsScratch();
  int n = storageReadDayRuns(competitionById(compId)->slug, day, rows, MAX_RUNS_PER_DAY);
  snprintf(rows[linha].status, sizeof(rows[linha].status), "%s", status);
  storageRewriteDayJsonl(competitionById(compId)->slug, day, rows, n);
}

struct Dia { DayStatusParticipant rows[MAX_PARTICIPANTS]; int n; bool completo; bool ultimo; };
static Dia diaDe(int compId) {
  Dia d;
  d.n = competitionDayStatus(compId, d.rows, MAX_PARTICIPANTS, d.completo, d.ultimo);
  return d;
}
static const DayStatusParticipant* robo(const Dia& d, int partId) {
  for (int i = 0; i < d.n; i++) if (d.rows[i].participantId == partId) return &d.rows[i];
  return nullptr;
}

// Competição de 1 dia e 3 tomadas com dois robôs inscritos.
struct Prova { int comp, equipe, a, b; };
static Prova prova(int dias = 1, int tomadas = 3) {
  Prova p;
  p.equipe = teamCreate("Equipe Falcão");
  p.a = participantCreate("Falcão 1", "", p.equipe);
  p.b = participantCreate("Coruja", "", p.equipe);
  p.comp = competitionCreate("Copa Pinhão", dias, tomadas);
  linkCreate(p.comp, p.a);
  linkCreate(p.comp, p.b);
  return p;
}

// ------------------------------------------------------------------
TESTE(cadastro_sobrevive, "cadastro continua igual depois de desligar e ligar") {
  int eq = teamCreate("Equipe Falcão");
  int r = participantCreate("Raio, o \"veloz\"", "R1", eq);
  int c = competitionCreate("Copa Pinhão 2026", 2, 3);
  linkCreate(c, r);

  dataStoreInit();  // "religa": tudo é relido dos arquivos da flash

  CONFERE(teamCount() == 1 && participantCount() == 1 && competitionCount() == 1);
  CONFERE(std::strcmp(teamById(eq)->name, "Equipe Falcão") == 0);
  CONFERE(std::strcmp(participantById(r)->name, "Raio, o \"veloz\"") == 0);  // vírgula e aspas no CSV
  CONFERE(std::strcmp(participantById(r)->externalCode, "R1") == 0);
  CONFERE(std::strcmp(competitionById(c)->name, "Copa Pinhão 2026") == 0);
  CONFERE(competitionById(c)->numDays == 2 && competitionById(c)->attemptsPerDay == 3);
  CONFERE(linkExists(c, r));
}

TESTE(ids_nunca_voltam, "id novo nunca repete um antigo, mesmo se um arquivo se perder") {
  int c1 = competitionCreate("Primeira", 1, 1);
  int r1 = participantCreate("Robô", "", teamCreate("Equipe"));
  fakeFiles().erase("/competicoes.csv");  // leitura falhou / arquivo corrompido
  fakeFiles().erase("/participantes.csv");
  dataStoreInit();
  CONFERE(competitionCreate("Segunda", 1, 1) > c1);
  CONFERE(participantCreate("Outro", "", 1) > r1);
}

TESTE(nomes_recusados, "nomes vazios, repetidos ou com aspas são recusados onde devem") {
  int eq = teamCreate("Equipe");
  CONFERE(teamCreate("") < 0);
  CONFERE(teamCreate("EQUIPE") == -2);  // repetido, sem diferenciar maiúscula
  CONFERE(participantCreate("Raio", "", eq) > 0);
  CONFERE(participantCreate("raio", "", eq) == -2);
  CONFERE(teamRename(eq, "Com \"aspas\"") == -4);
  CONFERE(competitionRename(competitionCreate("Prova", 1, 1), "") == -4);
}

TESTE(contagem_de_tomadas, "tomada repetida não conta; inválida e não completou contam") {
  Prova p = prova();
  corrida(p.comp, p.a, 1, 1, "valid", 12.0);
  corrida(p.comp, p.a, 1, 2, "retried", 9.0);
  corrida(p.comp, p.a, 1, 2, "invalid", 13.0);
  Dia d = diaDe(p.comp);
  CONFERE(robo(d, p.a)->attemptsDone == 2);
  CONFERE(!robo(d, p.a)->done);
  CONFERE(!d.completo);

  corrida(p.comp, p.a, 1, 3, "dnf", 0);
  d = diaDe(p.comp);
  CONFERE(robo(d, p.a)->done);
  CONFERE(!d.completo);  // o outro robô ainda não correu
}

TESTE(repetir_ultima_tomada, "Repetir tomada no último robô não encerra o dia (bug da Copa Pinhão)") {
  Prova p = prova(1, 1);
  corrida(p.comp, p.a, 1, 1, "valid", 11.0);
  corrida(p.comp, p.b, 1, 1, "pending_validation", 12.0);  // última corrida do dia
  decidir(p.comp, 1, 1, "retried");                         // juiz toca Repetir tomada
  Dia d = diaDe(p.comp);
  CONFERE(!d.completo);
  CONFERE(robo(d, p.b)->attemptsDone == 0);
}

TESTE(nao_compareceu_fecha_o_dia, "robô que não compareceu em todas as tomadas deixa o dia terminar") {
  Prova p = prova(1, 2);
  corrida(p.comp, p.a, 1, 1, "valid", 11.0);
  corrida(p.comp, p.a, 1, 2, "valid", 10.5);
  CONFERE(!diaDe(p.comp).completo);
  corrida(p.comp, p.b, 1, 1, "dnf", 0);  // Não compareceu
  corrida(p.comp, p.b, 1, 2, "dnf", 0);
  Dia d = diaDe(p.comp);
  CONFERE(d.completo);
  CONFERE(d.ultimo);
}

TESTE(proxima_tomada, "depois de uma tomada, a seguinte do mesmo robô fica selecionada") {
  Prova p = prova(1, 2);
  corrida(p.comp, p.a, 1, 1, "valid", 11.0);
  raceControlDisarm();
  maybeAdvanceAttempt(p.comp, p.a, 1);
  CONFERE(raceControlIsArmed());
  CONFERE(raceControlParticipantId() == p.a && raceControlAttempt() == 2);

  corrida(p.comp, p.a, 1, 2, "invalid", 12.0);
  raceControlDisarm();
  maybeAdvanceAttempt(p.comp, p.a, 1);
  CONFERE(!raceControlIsArmed());  // acabaram as tomadas: ninguém fica selecionado
}

TESTE(ranking, "ranking usa o melhor tempo válido e deixa quem não tem tempo no fim") {
  Prova p = prova(1, 3);
  int c = participantCreate("Raio", "", p.equipe);
  linkCreate(p.comp, c);
  corrida(p.comp, p.a, 1, 1, "valid", 12.0);
  corrida(p.comp, p.a, 1, 2, "invalid", 9.0);  // mais rápida, mas inválida: não conta
  corrida(p.comp, p.b, 1, 1, "valid", 11.0);
  corrida(p.comp, c, 1, 1, "dnf", 0);

  RankingRow rows[MAX_RANKING_ROWS];
  int n = computeRanking(p.comp, rows, MAX_RANKING_ROWS);
  CONFERE(n == 3);
  CONFERE(rows[0].participantId == p.b && rows[0].bestElapsedUs == 11000000 && rows[0].position == 1);
  CONFERE(rows[1].participantId == p.a && rows[1].bestElapsedUs == 12000000 && rows[1].position == 2);
  CONFERE(rows[2].participantId == c && rows[2].bestElapsedUs == -1);
  CONFERE(rows[1].attempts[0].statusCode == 'V' && rows[1].attempts[0].isBest);
  CONFERE(rows[1].attempts[1].statusCode == 'I' && !rows[1].attempts[1].isBest);
  CONFERE(rows[2].attempts[0].statusCode == 'D');
}

TESTE(corrida_ida_e_volta, "corrida gravada é lida de volta igual") {
  Prova p = prova();
  corrida(p.comp, p.a, 1, 1, "pending_validation", 12.345678);
  StoredRun* rows = storageDayRunsScratch();
  int n = storageReadDayRuns(competitionById(p.comp)->slug, 1, rows, MAX_RUNS_PER_DAY);
  CONFERE(n == 1);
  CONFERE(rows[0].participantId == p.a && rows[0].day == 1 && rows[0].attempt == 1);
  CONFERE(rows[0].elapsedUs == 12345678);
  CONFERE(std::strcmp(rows[0].status, "pending_validation") == 0);
  CONFERE(std::strlen(rows[0].eventId) > 0);
  decidir(p.comp, 1, 0, "valid");
  n = storageReadDayRuns(competitionById(p.comp)->slug, 1, rows, MAX_RUNS_PER_DAY);
  CONFERE(n == 1 && std::strcmp(rows[0].status, "valid") == 0 && rows[0].elapsedUs == 12345678);
}

TESTE(importacao, "importar planilha cria equipes e robôs sem duplicar") {
  RosterImportRow linhas[] = {
    {"Equipe A", "R1", "C1"},
    {"Equipe A", "r1", ""},       // mesmo robô (maiúscula não importa)
    {"", "Sem equipe", ""},
    {"Equipe B", "Com \"aspas\"", ""},
    {"Equipe B", "R2", ""},
  };
  int8_t st[5];
  RosterImportSummary s;
  rosterImport(linhas, 5, st, s);
  CONFERE(st[0] == ROSTER_CREATED && st[1] == ROSTER_EXISTING && st[2] == ROSTER_INVALID);
  CONFERE(st[3] == ROSTER_INVALID && st[4] == ROSTER_CREATED);
  CONFERE(s.teamsCreated == 2 && s.participantsCreated == 2 && s.participantsExisting == 1 && s.failed == 2);
  dataStoreInit();  // ficou gravado
  CONFERE(teamCount() == 2 && participantCount() == 2);
  rosterImport(linhas, 5, st, s);  // importar de novo não duplica
  CONFERE(participantCount() == 2 && s.participantsCreated == 0);
}

static int g_apagadas = 0;
static void contarApagada(const char*, const char*) { g_apagadas++; }

TESTE(excluir_robo, "excluir robô apaga as corridas dele e só as dele") {
  Prova p = prova();
  corrida(p.comp, p.a, 1, 1, "valid", 12.0);
  corrida(p.comp, p.b, 1, 1, "valid", 11.0);
  corrida(p.comp, p.a, 1, 2, "invalid", 13.0);
  int apagadas = 0;
  g_apagadas = 0;
  CONFERE(participantPurge(p.a, apagadas, contarApagada) == 0);
  CONFERE(apagadas == 2 && g_apagadas == 2);
  CONFERE(participantById(p.a) == nullptr);
  StoredRun* rows = storageDayRunsScratch();
  int n = storageReadDayRuns(competitionById(p.comp)->slug, 1, rows, MAX_RUNS_PER_DAY);
  CONFERE(n == 1 && rows[0].participantId == p.b);
}

TESTE(robo_selecionado_nao_exclui, "robô selecionado para correr não pode ser excluído") {
  Prova p = prova();
  raceControlArm(p.comp, p.a, 1, 1);
  int apagadas = 0;
  CONFERE(participantPurge(p.a, apagadas) == -2);
  CONFERE(participantById(p.a) != nullptr);
}

TESTE(avancar_dia, "avançar dia desseleciona o robô e o último dia encerra a competição") {
  Prova p = prova(2, 1);
  raceControlArm(p.comp, p.a, 1, 1);
  bool fim = false;
  int dia = 0;
  CONFERE(competitionAdvanceDay(p.comp, fim, dia));
  CONFERE(!fim && dia == 2 && competitionById(p.comp)->currentDay == 2);
  CONFERE(!raceControlIsArmed());
  CONFERE(competitionAdvanceDay(p.comp, fim, dia));
  CONFERE(fim && std::strcmp(competitionById(p.comp)->status, "finished") == 0);
}

TESTE(dias_travados, "dias e tomadas só mudam antes da primeira corrida; o nome muda sempre") {
  Prova p = prova(1, 3);
  CONFERE(competitionUpdate(p.comp, 2, 2) == 0);
  corrida(p.comp, p.a, 1, 1, "valid", 11.0);
  CONFERE(competitionUpdate(p.comp, 3, 3) == -2);
  CONFERE(competitionRename(p.comp, "Copa Pinhão Final") == 0);
  dataStoreInit();
  CONFERE(std::strcmp(competitionById(p.comp)->name, "Copa Pinhão Final") == 0);
  CONFERE(participantHasRunsInCompetition(p.comp, p.a));  // as corridas continuam ligadas (o slug não mudou)
}

// ------------------------------------------------------------------
int main() {
  int falharam = 0;
  for (const Teste& t : testes()) {
    zerar();
    int antes = g_falhas;
    t.fn();
    bool ok = g_falhas == antes;
    if (!ok) falharam++;
    std::printf("%s %s\n", ok ? "ok    " : "FALHOU", t.nome);
  }
  std::printf("\n%d testes, %d conferências, %d falharam\n", (int)testes().size(), g_total, falharam);
  return falharam ? 1 : 0;
}
