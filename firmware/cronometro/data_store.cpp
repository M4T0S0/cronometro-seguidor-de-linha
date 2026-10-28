#include "data_store.h"
#include "config.h"
#include "storage.h"
#include "race_control.h"
#include "json_utils.h"
#include "data_version.h"
#include "state_machine.h"
#include <new>
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <string.h>

// Namespace de NVS (flash interna) só pros PRÓXIMOS ids a distribuir -
// ver comentário em dataStoreInit() sobre o porquê disso não poder
// depender só de reler o CSV no boot.
static const char* ID_PREFS_NAMESPACE = "datastore";

static Participant g_participants[MAX_PARTICIPANTS];
static int g_participantCount = 0;
static int g_nextParticipantId = 1;

static Competition g_competitions[MAX_COMPETITIONS];
static int g_competitionCount = 0;
static int g_nextCompetitionId = 1;

struct LinkRow { int competitionId; int participantId; };
static LinkRow g_links[MAX_LINKS];
static int g_linkCount = 0;
static Team g_teams[MAX_TEAMS];
static int g_teamCount = 0;
static int g_nextTeamId = 1;

static void persistNextIds() {
  Preferences prefs;
  prefs.begin(ID_PREFS_NAMESPACE, false);
  prefs.putInt("nextCompId", g_nextCompetitionId);
  prefs.putInt("nextPartId", g_nextParticipantId);
  prefs.end();
}

// ---------------------------------------------------------------------
// CSV mínimo (campos entre aspas, "" escapa aspas dentro do campo) —
// usado só para os 3 arquivos de cadastro (participantes.csv,
// competicoes.csv, vinculos.csv), bem menores e mais estruturados que o
// resultados.csv "de leitura humana" gerado por storage.cpp. Não é um
// parser RFC4180 completo, mas cobre o caso real (nomes livres, sem
// controle de vírgula arbitrário do usuário).
// ---------------------------------------------------------------------

static String csvNextField(const String &line, int &pos) {
  if (pos >= (int)line.length()) return String("");
  String field;
  if (line[pos] == '"') {
    pos++;
    while (pos < (int)line.length()) {
      if (line[pos] == '"') {
        if (pos + 1 < (int)line.length() && line[pos + 1] == '"') {
          field += '"';
          pos += 2;
        } else {
          pos++;
          break;
        }
      } else {
        field += line[pos++];
      }
    }
  } else {
    while (pos < (int)line.length() && line[pos] != ',') {
      field += line[pos++];
    }
  }
  if (pos < (int)line.length() && line[pos] == ',') pos++;
  return field;
}

static void csvWriteField(File &f, const char* value) {
  f.print('"');
  for (const char* p = value; *p; p++) {
    if (*p == '"') f.print("\"\"");
    else f.print(*p);
  }
  f.print('"');
}

// ------------------- Persistência: participantes.csv -------------------
//
// Nomes de arquivo e cabeçalho de coluna em português — mesmo padrão já
// usado em resultados.csv (storage.cpp), para quem abrir o arquivo exportado
// direto num PC entender do que se trata sem precisar ler o código.
// load*File() descarta a primeira linha (cabeçalho) sem interpretá-la.

static void skipCsvHeader(File &f) {
  if (f.available()) f.readStringUntil('\n');
}

static void loadParticipantsFile() {
  g_participantCount = 0;
  g_nextParticipantId = 1;
  if (!storageIsOk() || !LittleFS.exists("/participantes.csv")) return;

  File f = LittleFS.open("/participantes.csv", FILE_READ);
  if (!f) return;
  skipCsvHeader(f);

  while (f.available() && g_participantCount < MAX_PARTICIPANTS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int pos = 0;
    String idStr = csvNextField(line, pos);
    String name = csvNextField(line, pos);
    String code = csvNextField(line, pos);
    String teamIdStr = csvNextField(line, pos);

    Participant &p = g_participants[g_participantCount++];
    p.id = idStr.toInt();
    strlcpy(p.name, name.c_str(), sizeof(p.name));
    strlcpy(p.externalCode, code.c_str(), sizeof(p.externalCode));
    p.teamId = teamIdStr.length() ? teamIdStr.toInt() : -1;
    if (p.id >= g_nextParticipantId) g_nextParticipantId = p.id + 1;
  }
  f.close();
}

static void saveParticipantsFile() {
  if (!storageIsOk()) return;
  File f = LittleFS.open("/participantes.csv.tmp", FILE_WRITE);
  if (!f) return;

  f.println("ID,Nome,Codigo Externo,EquipeId");
  for (int i = 0; i < g_participantCount; i++) {
    f.print(g_participants[i].id);
    f.print(',');
    csvWriteField(f, g_participants[i].name);
    f.print(',');
    csvWriteField(f, g_participants[i].externalCode);
    f.print(',');
    f.print(g_participants[i].teamId);
    f.println();
  }
  f.flush();
  f.close();

  if (LittleFS.exists("/participantes.csv")) LittleFS.remove("/participantes.csv");
  LittleFS.rename("/participantes.csv.tmp", "/participantes.csv");
}

// ------------------- Persistência: competicoes.csv -------------------

static void loadCompetitionsFile() {
  g_competitionCount = 0;
  g_nextCompetitionId = 1;
  if (!storageIsOk() || !LittleFS.exists("/competicoes.csv")) return;

  File f = LittleFS.open("/competicoes.csv", FILE_READ);
  if (!f) return;
  skipCsvHeader(f);

  while (f.available() && g_competitionCount < MAX_COMPETITIONS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int pos = 0;
    String idStr = csvNextField(line, pos);
    String name = csvNextField(line, pos);
    String slug = csvNextField(line, pos);
    String numDaysStr = csvNextField(line, pos);
    String attemptsStr = csvNextField(line, pos);
    String status = csvNextField(line, pos);
    String currentDayStr = csvNextField(line, pos);

    Competition &c = g_competitions[g_competitionCount++];
    c.id = idStr.toInt();
    strlcpy(c.name, name.c_str(), sizeof(c.name));
    strlcpy(c.slug, slug.c_str(), sizeof(c.slug));
    c.numDays = numDaysStr.toInt();
    c.attemptsPerDay = attemptsStr.toInt();
    strlcpy(c.status, status.c_str(), sizeof(c.status));
    c.currentDay = currentDayStr.toInt();
    if (c.id >= g_nextCompetitionId) g_nextCompetitionId = c.id + 1;
  }
  f.close();
}

static void saveCompetitionsFile() {
  if (!storageIsOk()) return;
  File f = LittleFS.open("/competicoes.csv.tmp", FILE_WRITE);
  if (!f) return;

  f.println("ID,Nome,Slug,Dias,Tomadas por Dia,Status,Dia Atual");
  for (int i = 0; i < g_competitionCount; i++) {
    const Competition &c = g_competitions[i];
    f.print(c.id);
    f.print(',');
    csvWriteField(f, c.name);
    f.print(',');
    csvWriteField(f, c.slug);
    f.print(',');
    f.print(c.numDays);
    f.print(',');
    f.print(c.attemptsPerDay);
    f.print(',');
    csvWriteField(f, c.status);
    f.print(',');
    f.print(c.currentDay);
    f.println();
  }
  f.flush();
  f.close();

  if (LittleFS.exists("/competicoes.csv")) LittleFS.remove("/competicoes.csv");
  LittleFS.rename("/competicoes.csv.tmp", "/competicoes.csv");
}

// ------------------- Persistência: vinculos.csv -------------------
// "Vínculo" = associação participante <-> competição (quem está inscrito
// em qual competição). Nome trocado de links.csv para isso ficar claro
// pra quem abrir o arquivo sem conhecer o código.

static void loadLinksFile() {
  g_linkCount = 0;
  if (!storageIsOk() || !LittleFS.exists("/vinculos.csv")) return;

  File f = LittleFS.open("/vinculos.csv", FILE_READ);
  if (!f) return;
  skipCsvHeader(f);

  while (f.available() && g_linkCount < MAX_LINKS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int pos = 0;
    String compIdStr = csvNextField(line, pos);
    String partIdStr = csvNextField(line, pos);

    g_links[g_linkCount].competitionId = compIdStr.toInt();
    g_links[g_linkCount].participantId = partIdStr.toInt();
    g_linkCount++;
  }
  f.close();
}

static void saveLinksFile() {
  if (!storageIsOk()) return;
  File f = LittleFS.open("/vinculos.csv.tmp", FILE_WRITE);
  if (!f) return;

  f.println("ID Competicao,ID Participante");
  for (int i = 0; i < g_linkCount; i++) {
    f.print(g_links[i].competitionId);
    f.print(',');
    f.print(g_links[i].participantId);
    f.println();
  }
  f.flush();
  f.close();

  if (LittleFS.exists("/vinculos.csv")) LittleFS.remove("/vinculos.csv");
  LittleFS.rename("/vinculos.csv.tmp", "/vinculos.csv");
}

static void loadTeamsFile() {
  g_teamCount = 0;
  g_nextTeamId = 1;
  if (!storageIsOk() || !LittleFS.exists("/equipes.csv")) return;

  File f = LittleFS.open("/equipes.csv", FILE_READ);
  if (!f) return;
  skipCsvHeader(f);

  while (f.available() && g_teamCount < MAX_TEAMS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int pos = 0;
    String idStr = csvNextField(line, pos);
    String name = csvNextField(line, pos);

    Team &t = g_teams[g_teamCount++];
    t.id = idStr.toInt();
    strlcpy(t.name, name.c_str(), sizeof(t.name));
    if (t.id >= g_nextTeamId) g_nextTeamId = t.id + 1;
  }
  f.close();
}

static void saveTeamsFile() {
  if (!storageIsOk()) return;
  File f = LittleFS.open("/equipes.csv.tmp", FILE_WRITE);
  if (!f) return;

  f.println("ID,Nome");
  for (int i = 0; i < g_teamCount; i++) {
    f.print(g_teams[i].id);
    f.print(',');
    csvWriteField(f, g_teams[i].name);
    f.println();
  }
  f.flush();
  f.close();

  if (LittleFS.exists("/equipes.csv")) LittleFS.remove("/equipes.csv");
  LittleFS.rename("/equipes.csv.tmp", "/equipes.csv");
}

int teamCreate(const char* name) {
  if (!name || name[0] == '\0') return -1;
  for (int i = 0; i < g_teamCount; i++) {
    if (strcasecmp(g_teams[i].name, name) == 0) return -2;
  }
  if (g_teamCount >= MAX_TEAMS) return -1;
  Team &t = g_teams[g_teamCount++];
  t.id = g_nextTeamId++;
  strlcpy(t.name, name, sizeof(t.name));
  saveTeamsFile();
  dataVersionBump();
  return t.id;
}

int teamCount() { return g_teamCount; }

const Team* teamAt(int index) {
  if (index < 0 || index >= g_teamCount) return nullptr;
  return &g_teams[index];
}

const Team* teamById(int id) {
  for (int i = 0; i < g_teamCount; i++) {
    if (g_teams[i].id == id) return &g_teams[i];
  }
  return nullptr;
}

// Aspas duplas quebrariam o JSON montado à mão em cloud_sync.cpp e
// storage.cpp (que assumem nomes sem aspas) - recusadas na edição.
static bool nameIsValid(const char* name) {
  return name && name[0] != '\0' && strchr(name, '"') == nullptr;
}

int teamRename(int id, const char* name) {
  if (!nameIsValid(name)) return -4;
  Team* target = nullptr;
  for (int i = 0; i < g_teamCount; i++) {
    if (g_teams[i].id == id) target = &g_teams[i];
    else if (strcasecmp(g_teams[i].name, name) == 0) return -2;
  }
  if (!target) return -1;
  if (strcmp(target->name, name) == 0) return 0; // nada mudou
  strlcpy(target->name, name, sizeof(target->name));
  saveTeamsFile();
  dataVersionBump();
  return 0;
}

int teamParticipantCount(int teamId) {
  int n = 0;
  for (int i = 0; i < g_participantCount; i++) {
    if (g_participants[i].teamId == teamId) n++;
  }
  return n;
}

bool teamDelete(int id) {
  for (int i = 0; i < g_participantCount; i++) {
    if (g_participants[i].teamId == id) return false;
  }
  int idx = -1;
  for (int i = 0; i < g_teamCount; i++) {
    if (g_teams[i].id == id) { idx = i; break; }
  }
  if (idx == -1) return false;
  for (int i = idx; i < g_teamCount - 1; i++) g_teams[i] = g_teams[i + 1];
  g_teamCount--;
  saveTeamsFile();
  dataVersionBump();
  return true;
}

void dataStoreInit() {
  loadTeamsFile();
  loadParticipantsFile();
  loadCompetitionsFile();
  loadLinksFile();

  // Os próximos ids NÃO dependem só de reler o CSV agora: se a leitura de competicoes.csv/
  // participantes.csv falhar PARCIALMENTE no boot (um mau contato
  // pode atrapalhar uma leitura tanto quanto uma escrita), g_nextXId voltaria
  // a um valor baixo e uma competição/participante NOVOS receberiam um
  // id JÁ USADO por um registro antigo - os vínculos antigos daquele id
  // (ainda em vinculos.csv) passariam a "vazar" pra competição nova sem
  // ninguém ter clicado em nada. Guardar o próximo id também na flash
  // interna (NVS, separada dos arquivos) e usar sempre o maior valor
  // entre o que a NVS lembra e o que o CSV mostrou agora fecha esse
  // buraco - o contador nunca anda pra trás.
  Preferences prefs;
  prefs.begin(ID_PREFS_NAMESPACE, false);
  int savedNextCompId = prefs.getInt("nextCompId", 1);
  int savedNextPartId = prefs.getInt("nextPartId", 1);
  if (savedNextCompId > g_nextCompetitionId) g_nextCompetitionId = savedNextCompId;
  if (savedNextPartId > g_nextParticipantId) g_nextParticipantId = savedNextPartId;
  prefs.putInt("nextCompId", g_nextCompetitionId);
  prefs.putInt("nextPartId", g_nextParticipantId);
  prefs.end();

  Serial.print("[DATA] Carregado da flash: ");
  Serial.print(g_participantCount);
  Serial.print(" participante(s), ");
  Serial.print(g_competitionCount);
  Serial.print(" competicao(oes), ");
  Serial.print(g_linkCount);
  Serial.println(" vinculo(s).");
}

// ------------------- Participantes -------------------

int participantCreate(const char* name, const char* externalCode, int teamId) {
  if (!name || name[0] == '\0') return -1;
  for (int i = 0; i < g_participantCount; i++) {
    if (g_participants[i].teamId == teamId && strcasecmp(g_participants[i].name, name) == 0) return -2;
  }
  if (g_participantCount >= MAX_PARTICIPANTS) return -1;

  Participant &p = g_participants[g_participantCount++];
  p.id = g_nextParticipantId++;
  strlcpy(p.name, name, sizeof(p.name));
  strlcpy(p.externalCode, externalCode ? externalCode : "", sizeof(p.externalCode));
  p.teamId = teamId;

  persistNextIds(); // antes de mais nada, pra nunca reusar este id mesmo se o resto falhar
  saveParticipantsFile();
  dataVersionBump();
  return p.id;
}

int participantUpdate(int id, const char* name, const char* externalCode, int teamId) {
  if (!nameIsValid(name)) return -4;
  if (!teamById(teamId)) return -3;
  Participant* target = nullptr;
  for (int i = 0; i < g_participantCount; i++) {
    if (g_participants[i].id == id) target = &g_participants[i];
    else if (g_participants[i].teamId == teamId && strcasecmp(g_participants[i].name, name) == 0) return -2;
  }
  if (!target) return -1;
  strlcpy(target->name, name, sizeof(target->name));
  strlcpy(target->externalCode, externalCode ? externalCode : "", sizeof(target->externalCode));
  target->teamId = teamId;
  saveParticipantsFile();
  dataVersionBump();
  return 0;
}

int participantCount() { return g_participantCount; }

const Participant* participantAt(int index) {
  if (index < 0 || index >= g_participantCount) return nullptr;
  return &g_participants[index];
}

const Participant* participantById(int id) {
  for (int i = 0; i < g_participantCount; i++) {
    if (g_participants[i].id == id) return &g_participants[i];
  }
  return nullptr;
}

bool participantDelete(int id) {
  int idx = -1;
  for (int i = 0; i < g_participantCount; i++) {
    if (g_participants[i].id == id) { idx = i; break; }
  }
  if (idx < 0) return false;

  for (int i = idx; i < g_participantCount - 1; i++) {
    g_participants[i] = g_participants[i + 1];
  }
  g_participantCount--;
  saveParticipantsFile();

  // Cascata: remove os vinculos desse participante (compacta o array).
  int w = 0;
  for (int i = 0; i < g_linkCount; i++) {
    if (g_links[i].participantId != id) {
      g_links[w++] = g_links[i];
    }
  }
  if (w != g_linkCount) {
    g_linkCount = w;
    saveLinksFile();
  }
  dataVersionBump();
  return true;
}

// ------------------- Competições -------------------

int competitionCreate(const char* name, int numDays, int attemptsPerDay) {
  if (g_competitionCount >= MAX_COMPETITIONS || !name || name[0] == '\0') return -1;
  if (numDays < 1) numDays = 1;
  if (attemptsPerDay < 1) attemptsPerDay = 1;

  Competition &c = g_competitions[g_competitionCount++];
  c.id = g_nextCompetitionId++;
  strlcpy(c.name, name, sizeof(c.name));

  char baseSlug[24];
  slugify(name, baseSlug, sizeof(baseSlug));
  snprintf(c.slug, sizeof(c.slug), "%s_%d", baseSlug, c.id); // garante slug unico mesmo com nomes repetidos

  c.numDays = numDays;
  c.attemptsPerDay = attemptsPerDay;
  strlcpy(c.status, "draft", sizeof(c.status));
  c.currentDay = 1;

  persistNextIds(); // antes de mais nada, pra nunca reusar este id mesmo se o resto falhar
  saveCompetitionsFile();
  dataVersionBump();
  return c.id;
}

int competitionCount() { return g_competitionCount; }

const Competition* competitionAt(int index) {
  if (index < 0 || index >= g_competitionCount) return nullptr;
  return &g_competitions[index];
}

const Competition* competitionById(int id) {
  for (int i = 0; i < g_competitionCount; i++) {
    if (g_competitions[i].id == id) return &g_competitions[i];
  }
  return nullptr;
}

int competitionUpdate(int id, int numDays, int attemptsPerDay) {
  for (int i = 0; i < g_competitionCount; i++) {
    if (g_competitions[i].id != id) continue;
    Competition &c = g_competitions[i];

    StoredRun* dayRuns = storageDayRunsScratch(); // buffer compartilhado (ver storage.h)
    for (int day = 1; day <= c.numDays; day++) {
      if (storageReadDayRuns(c.slug, day, dayRuns, MAX_RUNS_PER_DAY) > 0) return -2;
    }

    if (numDays < 1) numDays = 1;
    if (attemptsPerDay < 1) attemptsPerDay = 1;
    c.numDays = numDays;
    c.attemptsPerDay = attemptsPerDay;
    saveCompetitionsFile();
    dataVersionBump();
    return 0;
  }
  return -1;
}

int competitionRename(int id, const char* name) {
  if (!nameIsValid(name)) return -4;
  for (int i = 0; i < g_competitionCount; i++) {
    if (g_competitions[i].id != id) continue;
    strlcpy(g_competitions[i].name, name, sizeof(g_competitions[i].name));
    saveCompetitionsFile();
    dataVersionBump();
    return 0;
  }
  return -1;
}

bool competitionAdvanceDay(int id, bool &outFinished, int &outCurrentDay) {
  for (int i = 0; i < g_competitionCount; i++) {
    if (g_competitions[i].id != id) continue;

    Competition &c = g_competitions[i];
    if (c.currentDay >= c.numDays) {
      strlcpy(c.status, "finished", sizeof(c.status));
      outFinished = true;
    } else {
      c.currentDay++;
      outFinished = false;
    }
    outCurrentDay = c.currentDay;
    saveCompetitionsFile();
    dataVersionBump();

    // Rede de seguranca: nunca deixa a corrida
    // armada "vazar" para a proxima competicao/dia.
    if (raceControlIsArmed() && raceControlCompetitionId() == id) {
      raceControlDisarm();
    }
    return true;
  }
  return false;
}

// ------------------- Vínculo participante <-> competição -------------------

bool linkExists(int competitionId, int participantId) {
  for (int i = 0; i < g_linkCount; i++) {
    if (g_links[i].competitionId == competitionId && g_links[i].participantId == participantId) {
      return true;
    }
  }
  return false;
}

bool linkCreate(int competitionId, int participantId) {
  if (!competitionById(competitionId) || !participantById(participantId)) return false;
  if (linkExists(competitionId, participantId)) return true; // idempotente
  if (g_linkCount >= MAX_LINKS) return false;

  g_links[g_linkCount].competitionId = competitionId;
  g_links[g_linkCount].participantId = participantId;
  g_linkCount++;
  saveLinksFile();
  dataVersionBump();
  return true;
}

// ------------------- Importação de lista de competidores -------------------

static Team* findTeamByName(const char* name) {
  for (int i = 0; i < g_teamCount; i++) {
    if (strcasecmp(g_teams[i].name, name) == 0) return &g_teams[i];
  }
  return nullptr;
}

static Participant* findParticipantByName(const char* name, int teamId) {
  for (int i = 0; i < g_participantCount; i++) {
    if (g_participants[i].teamId == teamId && strcasecmp(g_participants[i].name, name) == 0) {
      return &g_participants[i];
    }
  }
  return nullptr;
}

void rosterImport(const RosterImportRow* rows, int n, int8_t* outStatus, RosterImportSummary &outSummary) {
  outSummary = RosterImportSummary{0, 0, 0, 0};
  bool teamsChanged = false, participantsChanged = false;

  for (int r = 0; r < n; r++) {
    const char* teamName = rows[r].team ? rows[r].team : "";
    const char* robotName = rows[r].name ? rows[r].name : "";
    if (!nameIsValid(teamName) || !nameIsValid(robotName)) {
      outStatus[r] = ROSTER_INVALID;
      outSummary.failed++;
      continue;
    }

    // Mesmas regras de teamCreate/participantCreate, só que sem gravar
    // arquivo a cada linha (ver comentário em data_store.h).
    Team* team = findTeamByName(teamName);
    if (!team) {
      if (g_teamCount >= MAX_TEAMS) { outStatus[r] = ROSTER_FULL; outSummary.failed++; continue; }
      team = &g_teams[g_teamCount++];
      team->id = g_nextTeamId++;
      strlcpy(team->name, teamName, sizeof(team->name));
      teamsChanged = true;
      outSummary.teamsCreated++;
    }

    if (findParticipantByName(robotName, team->id)) {
      outStatus[r] = ROSTER_EXISTING;
      outSummary.participantsExisting++;
      continue;
    }
    if (g_participantCount >= MAX_PARTICIPANTS) { outStatus[r] = ROSTER_FULL; outSummary.failed++; continue; }
    Participant* p = &g_participants[g_participantCount++];
    p->id = g_nextParticipantId++;
    strlcpy(p->name, robotName, sizeof(p->name));
    strlcpy(p->externalCode, rows[r].code ? rows[r].code : "", sizeof(p->externalCode));
    p->teamId = team->id;
    participantsChanged = true;
    outStatus[r] = ROSTER_CREATED;
    outSummary.participantsCreated++;
  }

  if (participantsChanged) persistNextIds(); // antes de gravar os CSVs
  if (teamsChanged) saveTeamsFile();
  if (participantsChanged) saveParticipantsFile();
  if (teamsChanged || participantsChanged) dataVersionBump();
}

int linkCreateMany(int competitionId, const int* participantIds, int n, bool &outFull) {
  outFull = false;
  if (!competitionById(competitionId)) return -1;
  int added = 0;
  for (int i = 0; i < n; i++) {
    int pid = participantIds[i];
    if (!participantById(pid) || linkExists(competitionId, pid)) continue;
    if (g_linkCount >= MAX_LINKS) { outFull = true; break; }
    g_links[g_linkCount].competitionId = competitionId;
    g_links[g_linkCount].participantId = pid;
    g_linkCount++;
    added++;
  }
  if (added > 0) {
    saveLinksFile();
    dataVersionBump();
  }
  return added;
}

int participantRunCount(int participantId, int &outCompetitions) {
  outCompetitions = 0;
  int total = 0;
  for (int c = 0; c < g_competitionCount; c++) {
    const Competition &comp = g_competitions[c];
    int inComp = 0;
    for (int day = 1; day <= comp.numDays; day++) {
      inComp += storagePendingCountFor(comp.id, participantId, day);
      StoredRun* rows = storageDayRunsScratch();
      int n = storageReadDayRuns(comp.slug, day, rows, MAX_RUNS_PER_DAY);
      for (int i = 0; i < n; i++) {
        if (rows[i].participantId == participantId) inComp++;
      }
    }
    if (inComp > 0) {
      outCompetitions++;
      total += inComp;
    }
  }
  return total;
}

// Regrava o resultados.csv legível de um dia a partir das linhas que
// sobraram (mesma montagem de handlePatchRunValidation em web_server.cpp).
static void rewriteDayCsvFromRuns(const char* slug, int day, const StoredRun* rows, int n) {
  DayResultRow* csv = new (std::nothrow) DayResultRow[n > 0 ? n : 1];
  if (!csv) return; // o .jsonl (fonte de verdade) já foi regravado; o CSV é só cópia legível
  for (int i = 0; i < n; i++) {
    const Participant* p = participantById(rows[i].participantId);
    strlcpy(csv[i].participantName, p ? p->name : "?", sizeof(csv[i].participantName));
    csv[i].elapsedUs = rows[i].elapsedUs;
    strlcpy(csv[i].status, rows[i].status, sizeof(csv[i].status));
  }
  storageRewriteDayCsv(slug, day, csv, n);
  delete[] csv;
}

int participantPurge(int id, int &outRunsDeleted, RunDeletedFn onDeleted) {
  outRunsDeleted = 0;
  if (!participantById(id)) return -1;
  // Nunca mexe nos arquivos de resultado com uma corrida em andamento
  // (ela pode estar sendo gravada agora mesmo).
  if (currentState == SystemState::RUNNING || currentState == SystemState::FINISHED ||
      currentState == SystemState::SAVING) return -3;
  if (raceControlIsArmed() && raceControlParticipantId() == id) return -2;

  for (int c = 0; c < g_competitionCount; c++) {
    const Competition &comp = g_competitions[c];
    for (int day = 1; day <= comp.numDays; day++) {
      StoredRun* rows = storageDayRunsScratch();
      int n = storageReadDayRuns(comp.slug, day, rows, MAX_RUNS_PER_DAY);
      int kept = 0;
      for (int i = 0; i < n; i++) {
        if (rows[i].participantId != id) rows[kept++] = rows[i];
        else if (onDeleted) onDeleted(comp.slug, rows[i].eventId);
      }
      if (kept == n) continue;
      outRunsDeleted += n - kept;
      storageRewriteDayJsonl(comp.slug, day, rows, kept);
      rewriteDayCsvFromRuns(comp.slug, day, rows, kept);
    }
  }
  outRunsDeleted += storagePendingRemoveFor(id, onDeleted);

  participantDelete(id); // cadastro + inscrições (+ dataVersionBump)
  dataVersionBump();      // ranking/histórico em cache recalculam sem as corridas apagadas
  Serial.printf("[DATA] Robo %d excluido com %d corrida(s).\n", id, outRunsDeleted);
  return 0;
}

bool participantHasRunsInCompetition(int competitionId, int participantId) {
  const Competition* comp = competitionById(competitionId);
  if (!comp) return false;

  for (int day = 1; day <= comp->numDays; day++) {
    if (storagePendingCountFor(competitionId, participantId, day) > 0) return true;

    StoredRun* dayRuns = storageDayRunsScratch();
    int n = storageReadDayRuns(comp->slug, day, dayRuns, MAX_RUNS_PER_DAY);
    for (int i = 0; i < n; i++) {
      if (dayRuns[i].participantId == participantId) return true;
    }
  }
  return false;
}

bool linkDelete(int competitionId, int participantId) {
  int idx = -1;
  for (int i = 0; i < g_linkCount; i++) {
    if (g_links[i].competitionId == competitionId && g_links[i].participantId == participantId) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return false;

  for (int i = idx; i < g_linkCount - 1; i++) {
    g_links[i] = g_links[i + 1];
  }
  g_linkCount--;
  saveLinksFile();
  dataVersionBump();
  return true;
}

int linkedParticipantIds(int competitionId, int* outIds, int maxOut) {
  int count = 0;
  for (int i = 0; i < g_linkCount && count < maxOut; i++) {
    if (g_links[i].competitionId == competitionId) {
      outIds[count++] = g_links[i].participantId;
    }
  }
  return count;
}

// ------------------- Status do dia / avanço automático de tomada -------------------

// Corrida que o juiz mandou repetir ("retried") não gasta a tomada - a
// mesma tomada é rearmada e corrida de novo (se contasse, na última tomada do
// último robô o dia seria dado como concluído com o robô ainda armado).
static bool runUsesAttempt(const char* status) {
  return strcmp(status, "retried") != 0 && strcmp(status, "retry") != 0;
}

int competitionDayStatus(int competitionId, DayStatusParticipant* outRows, int maxRows,
                          bool &dayComplete, bool &isLastDay) {
  dayComplete = false;
  isLastDay = false;

  const Competition* comp = competitionById(competitionId);
  if (!comp) return 0;

  isLastDay = comp->currentDay >= comp->numDays;

  StoredRun* dayRuns = storageDayRunsScratch(); // buffer compartilhado (ver storage.h)
  int runCount = storageReadDayRuns(comp->slug, comp->currentDay, dayRuns, MAX_RUNS_PER_DAY);

  int linkedIds[MAX_PARTICIPANTS];
  int linkedCount = linkedParticipantIds(competitionId, linkedIds, MAX_PARTICIPANTS);

  int outCount = 0;
  bool allDone = true;
  for (int i = 0; i < linkedCount && outCount < maxRows; i++) {
    const Participant* p = participantById(linkedIds[i]);
    if (!p) continue;

    int attemptsDone = 0;
    for (int j = 0; j < runCount; j++) {
      if (dayRuns[j].participantId == p->id && runUsesAttempt(dayRuns[j].status)) attemptsDone++;
    }
    // Soma também tomadas presas na fila de pendências -
    // sem isso, uma corrida que falhou ao persistir não conta como
    // "usada" e a tela ofereceria a MESMA tomada de novo enquanto a
    // primeira ainda está pendente (duplicata quando a fila gravar depois).
    attemptsDone += storagePendingCountFor(competitionId, p->id, comp->currentDay);
    bool done = attemptsDone >= comp->attemptsPerDay;
    if (!done) allDone = false;

    outRows[outCount].participantId = p->id;
    strlcpy(outRows[outCount].name, p->name, sizeof(outRows[outCount].name));
    outRows[outCount].attemptsDone = attemptsDone;
    outRows[outCount].done = done;
    outCount++;
  }

  dayComplete = allDone && outCount > 0;
  return outCount;
}

void maybeAdvanceAttempt(int competitionId, int participantId, int day) {
  const Competition* comp = competitionById(competitionId);
  if (!comp) return;

  StoredRun* rows = storageDayRunsScratch(); // buffer compartilhado (ver storage.h)
  int n = storageReadDayRuns(comp->slug, day, rows, MAX_RUNS_PER_DAY);

  int count = 0;
  for (int i = 0; i < n; i++) {
    if (rows[i].participantId == participantId && runUsesAttempt(rows[i].status)) count++;
  }
  count += storagePendingCountFor(competitionId, participantId, day);
  if (count < comp->attemptsPerDay) {
    raceControlArm(competitionId, participantId, day, count + 1);
  }
}
