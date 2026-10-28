#pragma once
#include <stdint.h>
#include "storage.h" // RunDeletedFn

// =====================================================================
// DATA_STORE.H — "Banco de dados" local (versão standalone)
//
// Substitui server/database.py + routers/competitions.py +
// routers/participants.py. Participantes, competições e o vínculo entre
// eles são pequenos o bastante para viver inteiros em RAM (tetos em
// config.h) — persistidos em três arquivos CSV na flash interna
// (LittleFS, ver storage.h/cpp), com nome e cabeçalho de
// coluna em português (/participantes.csv, /competicoes.csv,
// /vinculos.csv), carregados de volta no boot (dataStoreInit(), chamado
// depois de storageInit()). É esse carregamento que "recupera uma
// competição anterior" ao religar o ESP32: nada se perde, porque o
// estado inteiro já estava na flash.
//
// Corridas em si (resultados) NÃO entram aqui — continuam em
// /<slug>/Dia_N/resultados.jsonl, geridas por storage.h/cpp.
// =====================================================================

struct Team {
  int id;
  char name[40];
};

struct Participant {
  int id;
  char name[40];
  char externalCode[24];
  int teamId;
};

struct Competition {
  int id;
  char name[48];
  char slug[32];        // sempre único (nome normalizado + "_" + id)
  int numDays;
  int attemptsPerDay;
  char status[16];       // "draft" | "finished"
  int currentDay;
};

struct DayStatusParticipant {
  int participantId;
  char name[40];
  int attemptsDone;
  bool done;
};

void dataStoreInit();

// ------------------- Participantes -------------------
// Retorna o id novo, -1 se capacidade cheia, -2 se já existe robô com esse
// nome NESSA MESMA equipe (case-insensitive) — nomes repetidos entre
// equipes diferentes são permitidos.
int participantCreate(const char* name, const char* externalCode, int teamId);

// Retorna o id novo, -1 se capacidade cheia, -2 se já existe equipe com esse nome (case-insensitive).
int teamCreate(const char* name);
int teamCount();
const Team* teamAt(int index);
const Team* teamById(int id);
bool teamDelete(int id);
int participantCount();
const Participant* participantAt(int index);
const Participant* participantById(int id);
// Corrige nome/código/equipe de um robô já cadastrado. As
// corridas guardam só o id do robô, então ranking, histórico e exportação
// já passam a mostrar o dado corrigido. Retorna 0 em sucesso, -1 se o
// robô não existe, -2 se já existe outro robô com esse nome na equipe de
// destino, -3 se a equipe não existe, -4 se o nome é vazio ou inválido.
int participantUpdate(int id, const char* name, const char* externalCode, int teamId);
// Renomeia uma equipe. Retorna 0 em sucesso, -1 se não existe, -2 se já
// existe outra equipe com esse nome, -4 se o nome é vazio ou inválido.
int teamRename(int id, const char* name);
// Quantos robôs pertencem à equipe (teamDelete só aceita equipe vazia).
int teamParticipantCount(int teamId);
// Quantas corridas (gravadas + pendentes na fila) o robô tem, somando
// todas as competições; outCompetitions = em quantas competições.
int participantRunCount(int participantId, int &outCompetitions);
// Exclui o robô e TUDO ligado a ele: corridas em todos os dias
// de todas as competições (resultados.jsonl/.csv regravados sem elas),
// pendências na fila e inscrições. Não tem volta. Retorna 0 em sucesso,
// -1 se o robô não existe, -2 se ele está armado agora, -3 se há uma
// corrida em andamento (qualquer robô). outRunsDeleted = corridas apagadas.
// onDeleted (opcional) é chamado para cada corrida apagada, ANTES de ela
// sumir dos arquivos - usado para apagar também na nuvem.
int participantPurge(int id, int &outRunsDeleted, RunDeletedFn onDeleted = nullptr);
// Remove o participante e todos os vínculos dele com competições
// (histórico de corridas na flash é preservado, só fica "orfão").
bool participantDelete(int id);

// ------------------- Competições -------------------
// Retorna o novo id, ou -1 se a capacidade (MAX_COMPETITIONS) foi atingida.
int competitionCreate(const char* name, int numDays, int attemptsPerDay);
int competitionCount();
const Competition* competitionAt(int index);
const Competition* competitionById(int id);

// Avança para o próximo dia, ou marca como finalizada se já era o
// último. Desarma a corrida atual se ela pertencer a esta competição
// (mesma rede de segurança do servidor antigo — nunca deixa nada
// "vazar" para a competição seguinte). Retorna false se o id não existe.
bool competitionAdvanceDay(int id, bool &outFinished, int &outCurrentDay);


// Edita dias/tomadas de uma competição já criada — só permitido se ela
// ainda não tiver nenhuma corrida registrada (proteção contra editar no
// meio da prova e bagunçar contagens já feitas). Retorna 0 em sucesso,
// -1 se a competição não existe, -2 se já existem corridas registradas.
int competitionUpdate(int id, int numDays, int attemptsPerDay);
// Troca só o nome (o slug/pasta das corridas não muda), mesmo com corridas
// registradas. 0 = ok, -1 = não existe, -4 = nome vazio/com aspas.
int competitionRename(int id, const char* name);

// ------------------- Vínculo participante <-> competição -------------------
bool linkExists(int competitionId, int participantId);
// Idempotente: se já existe, não duplica. Retorna false só se
// competição/participante não existirem ou a capacidade (MAX_LINKS)
// tiver sido atingida.
bool linkCreate(int competitionId, int participantId);
bool linkDelete(int competitionId, int participantId);
// true se o participante já tem alguma volta registrada (ou pendente)
// nesta competição — usado para bloquear exclusão (ver handleDeleteCompetitionParticipant).
bool participantHasRunsInCompetition(int competitionId, int participantId);
// Preenche outIds com os ids dos participantes vinculados (na ordem em
// que foram vinculados); retorna quantos.
int linkedParticipantIds(int competitionId, int* outIds, int maxOut);

// Vincula vários robôs de uma vez (tela "Adicionar robôs" da competição),
// gravando vinculos.csv UMA vez só. Ids inexistentes ou já vinculados são
// ignorados. Retorna quantos foram vinculados agora, ou -1 se a
// competição não existe. outFull = true se MAX_LINKS cortou a lista.
int linkCreateMany(int competitionId, const int* participantIds, int n, bool &outFull);

// ------------------- Importação de lista de competidores (cadastro) -------------------
// Uma linha do arquivo importado (CSV/JSON - o parse do arquivo em si é
// feito no navegador, ver renderRosterPage em pagina/index.html). A importação
// só CADASTRA equipes/robôs - inscrever numa competição é sempre na tela
// da competição.
struct RosterImportRow {
  const char* team;
  const char* name;
  const char* code; // pode ser "" (opcional)
};

enum RosterImportStatus : int8_t {
  ROSTER_CREATED = 0,  // robô novo cadastrado
  ROSTER_EXISTING = 1, // robô já existia (mesmo nome + equipe) - nada mudou
  ROSTER_INVALID = -1, // equipe ou nome vazio (ou com aspas duplas)
  ROSTER_FULL = -2,    // capacidade (MAX_TEAMS/MAX_PARTICIPANTS) atingida
};

struct RosterImportSummary {
  int teamsCreated;
  int participantsCreated;
  int participantsExisting;
  int failed;
};

// Cria equipes/robôs que ainda não existem (equipe casada por nome,
// robô por nome + equipe, ambos case-insensitive) e ignora os que já
// existem. Diferente de chamar teamCreate/participantCreate linha a
// linha, grava cada arquivo CSV na flash UMA vez só no final. outStatus
// (n posições) recebe o RosterImportStatus de cada linha.
void rosterImport(const RosterImportRow* rows, int n, int8_t* outStatus, RosterImportSummary &outSummary);

// ------------------- Status do dia -------------------
// Preenche outRows (até maxRows) com a contagem de tomadas de cada
// participante vinculado no dia ATUAL da competição; retorna quantos
// participantes foram preenchidos. dayComplete = todos os vinculados já
// completaram attemptsPerDay tomadas (e há pelo menos um vinculado).
int competitionDayStatus(int competitionId, DayStatusParticipant* outRows, int maxRows,
                          bool &dayComplete, bool &isLastDay);

// Se o participante ainda tiver tomadas restantes no dia (contando
// TODAS as tomadas já registradas, independente do status — mesma
// regra do servidor antigo), arma (travada) a próxima automaticamente
// via race_control. Chamada depois que uma corrida termina já resolvida
// (abortada) ou é validada pelo juiz.
void maybeAdvanceAttempt(int competitionId, int participantId, int day);
