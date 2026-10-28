#include "idle_screen.h"
#include "config.h"
#include "display.h"
#include "wifi.h"
#include "sensors.h"
#include "storage.h"
#include "race_control.h"
#include "data_store.h"
#include <Arduino.h>
#include <string.h>
#include "esp_timer.h"

// Contexto da página do juiz (ver idleScreenSetJudgeContext).
static char g_ctxView[16] = "";
static char g_ctxPhase[16] = "";
static int g_ctxCompetitionId = -1;
static int64_t g_ctxAtUs = 0;
static bool g_ctxEverReceived = false;

// Corrida esperando validação.
static bool g_valPending = false;
static char g_valRobot[40] = "";
static char g_valTeam[40] = "";
static int64_t g_valElapsedUs = 0;

static bool g_force = true;
static uint32_t g_lastSignature = 0;
static int64_t g_lastEvalUs = 0;

static const int64_t EVAL_INTERVAL_US = 150000; // 150 ms
// Variantes da tela de acesso (ScreenDesc.n2).
enum { ACCESS_WIFI = 0, ACCESS_PANEL_AP = 1, ACCESS_PANEL_STA = 2 };

// Uma tela "descrita": tipo + textos. A assinatura (hash) decide se
// precisa redesenhar.
enum class ScreenKind : uint8_t { CONNECTING, WARNING, ARMED, VALIDATION, CONTEXT, ACCESS };
struct ScreenDesc {
  ScreenKind kind;
  char a[48];
  char b[48];
  char c[48];
  char d[48];
  int32_t n1;
  int32_t n2;
  int64_t n3;
};

static uint32_t signatureOf(const ScreenDesc &s) {
  uint32_t h = 2166136261u; // FNV-1a
  const uint8_t* p = (const uint8_t*)&s;
  for (size_t i = 0; i < sizeof(s); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static void describeContext(ScreenDesc &s) {
  s.kind = ScreenKind::CONTEXT;
  const Competition* comp = g_ctxCompetitionId > 0 ? competitionById(g_ctxCompetitionId) : nullptr;
  const char* v = g_ctxView;

  if (strcmp(v, "competition") == 0 && comp) {
    strlcpy(s.a, "COMPETICAO", sizeof(s.a));
    strlcpy(s.b, comp->name, sizeof(s.b));
    if (strcmp(comp->status, "finished") == 0 || strcmp(g_ctxPhase, "finished") == 0) {
      strlcpy(s.c, "Encerrada", sizeof(s.c));
      strlcpy(s.d, "veja o ranking", sizeof(s.d));
    } else {
      snprintf(s.c, sizeof(s.c), "Dia %d de %d", comp->currentDay, comp->numDays);
      strlcpy(s.d, strcmp(g_ctxPhase, "day_complete") == 0 ? "Dia concluido" : "Escolhendo o robo", sizeof(s.d));
    }
  } else if ((strcmp(v, "inscritos") == 0 || strcmp(v, "add") == 0) && comp) {
    int ids[MAX_PARTICIPANTS];
    int n = linkedParticipantIds(comp->id, ids, MAX_PARTICIPANTS);
    strlcpy(s.a, "INSCRICOES", sizeof(s.a));
    strlcpy(s.b, comp->name, sizeof(s.b));
    snprintf(s.c, sizeof(s.c), "%d robo%s inscrito%s", n, n == 1 ? "" : "s", n == 1 ? "" : "s");
    strlcpy(s.d, strcmp(v, "add") == 0 ? "Juiz adicionando robos" : "", sizeof(s.d));
  } else if (strcmp(v, "roster") == 0) {
    strlcpy(s.a, "CADASTRO", sizeof(s.a));
    strlcpy(s.b, "Equipes e robos", sizeof(s.b));
    snprintf(s.c, sizeof(s.c), "%d equipe%s, %d robo%s", teamCount(), teamCount() == 1 ? "" : "s",
             participantCount(), participantCount() == 1 ? "" : "s");
  } else if (strcmp(v, "config") == 0) {
    strlcpy(s.a, "CONFIGURACOES", sizeof(s.a));
    strlcpy(s.b, "Rede e nuvem", sizeof(s.b));
    const char* ext = wifiStaSsid();
    if (wifiIsStaConnected()) snprintf(s.c, sizeof(s.c), "Ext: %s", ext);
    else if (ext[0]) snprintf(s.c, sizeof(s.c), "Ext: sem conexao");
    else strlcpy(s.c, "Sem rede externa", sizeof(s.c));
  } else if (strcmp(v, "new") == 0) {
    strlcpy(s.a, "NOVA COMPETICAO", sizeof(s.a));
    strlcpy(s.b, "Criando...", sizeof(s.b));
  } else if (strcmp(v, "competitions") == 0 || strcmp(v, "history") == 0) {
    strlcpy(s.a, strcmp(v, "history") == 0 ? "HISTORICO" : "COMPETICOES", sizeof(s.a));
    strlcpy(s.b, "Escolhendo", sizeof(s.b));
    strlcpy(s.c, "uma competicao", sizeof(s.c));
  } else {
    strlcpy(s.a, "PAINEL ABERTO", sizeof(s.a));
    strlcpy(s.b, "Pronto", sizeof(s.b));
    strlcpy(s.c, "Juiz no menu inicial", sizeof(s.c));
  }
}

static void describe(ScreenDesc &s) {
  memset(&s, 0, sizeof(s));
  int64_t now = esp_timer_get_time();

  // 1. Primeira tentativa de conexão na rede externa
  if (wifiIsInitialAttempt() && !raceControlIsArmed()) {
    s.kind = ScreenKind::CONNECTING;
    strlcpy(s.a, wifiStaSsid(), sizeof(s.a));
    return;
  }

  // 2. Alertas
  bool s1 = sensor1IsStuck(), s2 = sensor2IsStuck();
  if (s1 || s2) {
    s.kind = ScreenKind::WARNING;
    strlcpy(s.a, s1 && s2 ? "Sensores 1 e 2" : s1 ? "Sensor 1" : "Sensor 2", sizeof(s.a));
    strlcpy(s.b, "bloqueado/obstruido", sizeof(s.b));
    strlcpy(s.c, "libere a pista", sizeof(s.c));
    return;
  }
  if (!storageIsOk()) {
    s.kind = ScreenKind::WARNING;
    strlcpy(s.a, "Memoria", sizeof(s.a));
    strlcpy(s.b, "falhou ao abrir", sizeof(s.b));
    strlcpy(s.c, "resultados em risco", sizeof(s.c));
    return;
  }

  // 3. Robô armado
  if (raceControlIsArmed()) {
    s.kind = ScreenKind::ARMED;
    const Participant* p = participantById(raceControlParticipantId());
    const Team* t = p ? teamById(p->teamId) : nullptr;
    strlcpy(s.a, p ? p->name : "----", sizeof(s.a));
    strlcpy(s.b, t ? t->name : "", sizeof(s.b));
    s.n1 = raceControlDay();
    s.n2 = raceControlAttempt();
    s.n3 = raceControlIsReleased() ? 1 : 0;
    return;
  }

  // 4. Corrida esperando validação
  if (g_valPending) {
    s.kind = ScreenKind::VALIDATION;
    strlcpy(s.a, g_valRobot, sizeof(s.a));
    strlcpy(s.b, g_valTeam, sizeof(s.b));
    s.n3 = g_valElapsedUs;
    return;
  }

  // 5. O que o juiz está fazendo na página (se ela deu sinal de vida há pouco)
  if (g_ctxEverReceived && now - g_ctxAtUs < (int64_t)JUDGE_CONTEXT_TIMEOUT_US) {
    describeContext(s);
    return;
  }

  // 6. Como acessar o painel
  // Muda conforme o que o ESP32 percebe, nunca por tempo:
  //  - alguém entrou no Wi-Fi do cronômetro -> QR do painel (192.168.4.1)
  //  - ninguém nele, mas conectado numa rede externa -> QR do painel no
  //    endereço dessa rede (o juiz pode estar nela)
  //  - ninguém e sem rede externa -> QR para entrar no Wi-Fi do cronômetro
  s.kind = ScreenKind::ACCESS;
  wifiApIp(s.a, sizeof(s.a));
  wifiStaIp(s.b, sizeof(s.b));
  s.n1 = wifiApClientCount();
  if (s.n1 > 0) s.n2 = ACCESS_PANEL_AP;
  else if (s.b[0]) { s.n2 = ACCESS_PANEL_STA; strlcpy(s.c, wifiStaSsid(), sizeof(s.c)); }
  else s.n2 = ACCESS_WIFI;
}

// Texto do QR de Wi-Fi ("WIFI:T:WPA;S:<rede>;P:<senha>;;"), com os
// caracteres especiais do formato escapados com barra.
static void appendEscaped(char* out, size_t outSize, const char* text) {
  size_t w = strlen(out);
  for (const char* p = text; *p && w + 2 < outSize; p++) {
    if (strchr("\\;,:\"", *p)) out[w++] = '\\';
    out[w++] = *p;
  }
  out[w] = '\0';
}

static void buildWifiQrPayload(char* out, size_t outSize) {
  strlcpy(out, "WIFI:T:WPA;S:", outSize);
  appendEscaped(out, outSize, WIFI_AP_SSID);
  strlcat(out, ";P:", outSize);
  appendEscaped(out, outSize, wifiApPassword());
  strlcat(out, ";;", outSize);
}

static void draw(const ScreenDesc &s) {
  switch (s.kind) {
    case ScreenKind::CONNECTING:
      displayShowConnecting(s.a);
      break;
    case ScreenKind::WARNING:
      displayShowWarning(s.a, s.b, s.c);
      break;
    case ScreenKind::ARMED:
      displayShowArmed(s.a, s.b, s.n1, s.n2, s.n3 != 0);
      break;
    case ScreenKind::VALIDATION:
      displayShowValidation(s.a, s.b, s.n3);
      break;
    case ScreenKind::CONTEXT:
      displayShowContext(s.a, s.b, s.c, s.d);
      break;
    case ScreenKind::ACCESS: {
      if (s.n2 == ACCESS_WIFI) {
        char payload[128];
        buildWifiQrPayload(payload, sizeof(payload));
        displayShowQrInfo(payload, "WI-FI:", WIFI_AP_SSID, "SENHA:", wifiApPassword());
      } else {
        const char* ip = (s.n2 == ACCESS_PANEL_AP) ? s.a : s.b;
        char url[40];
        snprintf(url, sizeof(url), "http://%s", ip);
        if (s.n2 == ACCESS_PANEL_AP) displayShowQrInfo(url, "PAINEL:", ip, nullptr, nullptr);
        else displayShowQrInfo(url, "REDE:", s.c, "PAINEL:", ip);
      }
      break;
    }
  }
}

void idleScreenUpdate() {
  int64_t now = esp_timer_get_time();
  if (!g_force && now - g_lastEvalUs < EVAL_INTERVAL_US) return;
  g_lastEvalUs = now;

  static ScreenDesc s; // static: ~200 bytes fora da pilha do loop()
  describe(s);
  uint32_t sig = signatureOf(s);
  if (!g_force && sig == g_lastSignature) return;
  g_force = false;
  g_lastSignature = sig;
  draw(s);
}

void idleScreenInvalidate() { g_force = true; }

void idleScreenSetJudgeContext(const char* view, int competitionId, const char* phase) {
  strlcpy(g_ctxView, view ? view : "", sizeof(g_ctxView));
  strlcpy(g_ctxPhase, phase ? phase : "", sizeof(g_ctxPhase));
  g_ctxCompetitionId = competitionId;
  g_ctxAtUs = esp_timer_get_time();
  g_ctxEverReceived = true;
}

void idleScreenSetValidationPending(const char* robotName, const char* teamName, int64_t elapsedUs) {
  strlcpy(g_valRobot, robotName ? robotName : "", sizeof(g_valRobot));
  strlcpy(g_valTeam, teamName ? teamName : "", sizeof(g_valTeam));
  g_valElapsedUs = elapsedUs;
  g_valPending = true;
}

void idleScreenValidationResolved() { g_valPending = false; }
