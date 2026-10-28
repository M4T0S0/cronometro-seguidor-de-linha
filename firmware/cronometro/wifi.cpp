#include "wifi.h"
#include "config.h"
#include "display.h"
#include "state_machine.h"
#include "training_mode.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <new>
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_timer.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"

// Namespace NVS da configuração de rede (ver wifiSetStaConfig).
static const char* WIFI_PREFS_NAMESPACE = "wifi_cfg";

static WifiStaConfig g_staCfg = { WIFI_STA_NONE, WIFI_EAP_PEAP, "", "", "" };
static int g_apChannelSetting = 0; // 0 = automático

static WifiStaState g_state = WIFI_STATE_OFF;
static uint32_t g_attemptStartMs = 0;
static uint32_t g_retryAtMs = 0;
static int g_failCount = 0;
static int64_t g_lastCheckUs = 0;
static char g_lastError[96] = "";

// Motivo de queda vindo do evento do core (roda em outra task) - só um
// byte, lido e zerado pelo loop() em wifiUpdate().
static volatile uint8_t g_pendingReason = 0;

// Resultado da busca de redes: alocado no heap só quando alguém busca
// (a RAM estática do firmware já está no limite).
static const int MAX_SCAN_ENTRIES = 30;
static WifiScanEntry* g_scan = nullptr;
static int g_scanCount = -2;     // -2 = nunca varreu / falhou
static bool g_scanRunning = false;

// ---------------------------------------------------------------------
// Persistência (NVS)
// ---------------------------------------------------------------------

// Senha do Wi-Fi do cronômetro: sorteada uma vez e guardada. Só letras
// minúsculas e números fáceis de ler no display (sem 0/o, 1/l/i).
static char g_apPassword[9] = "";

static void generateApPassword() {
  static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 8; i++) g_apPassword[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
  g_apPassword[8] = '\0';
  Preferences prefs;
  prefs.begin(WIFI_PREFS_NAMESPACE, false);
  prefs.putString("appass", g_apPassword);
  prefs.end();
}

const char* wifiApPassword() { return g_apPassword; }

// Senha da rede externa (RA da eduroam etc.) guardada cifrada:
// AES-256-CTR com chave derivada do MAC de fábrica do chip, então não
// aparece em texto puro numa cópia da flash. Proteção contra leitura
// casual da memória, não contra quem tem o aparelho E conhece este
// código - para isso só a criptografia de flash do ESP32 (eFuse).
static void passwordKey(uint8_t key[32]) {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  static const char SALT[] = "cronometro-wifi-v1";
  mbedtls_sha256_update(&sha, (const uint8_t*)SALT, sizeof(SALT) - 1);
  mbedtls_sha256_update(&sha, mac, sizeof(mac));
  mbedtls_sha256_finish(&sha, key);
  mbedtls_sha256_free(&sha);
}

// CTR: cifrar e decifrar são a mesma operação. buf tem 16 bytes de IV + dados.
static void passwordCrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
  uint8_t key[32], nonce[16], block[16];
  size_t off = 0;
  passwordKey(key);
  memcpy(nonce, iv, 16);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 256);
  mbedtls_aes_crypt_ctr(&aes, len, &off, nonce, block, in, out);
  mbedtls_aes_free(&aes);
  memset(key, 0, sizeof(key));
}

static void savePassword(Preferences &prefs, const char* password) {
  size_t len = strlen(password);
  uint8_t buf[16 + sizeof(WifiStaConfig::password)];
  esp_fill_random(buf, 16);
  passwordCrypt(buf, (const uint8_t*)password, buf + 16, len);
  prefs.putBytes("passx", buf, 16 + len);
  prefs.remove("pass"); // apaga a versão antiga em texto puro, se houver
}

static void loadPassword(Preferences &prefs, char* out, size_t outSize) {
  out[0] = '\0';
  uint8_t buf[16 + sizeof(WifiStaConfig::password)];
  size_t n = prefs.getBytesLength("passx");
  if (n >= 16 && n <= sizeof(buf) && prefs.getBytes("passx", buf, n) == n) {
    size_t len = n - 16;
    if (len >= outSize) len = outSize - 1;
    passwordCrypt(buf, buf + 16, (uint8_t*)out, len);
    out[len] = '\0';
    return;
  }
  prefs.getString("pass", out, outSize); // versões antigas: texto puro (vira cifrada no próximo save)
}

static int g_apLastAutoChannel = 6; // último canal automático (usado quando o boot pula a busca)

static void loadConfig() {
  Preferences prefs;
  prefs.begin(WIFI_PREFS_NAMESPACE, false); // escrita: migra a senha antiga pra cifrada
  g_staCfg.security = prefs.getUChar("sec", WIFI_STA_NONE);
  g_staCfg.eapMethod = prefs.getUChar("eap", WIFI_EAP_PEAP);
  prefs.getString("ssid", g_staCfg.ssid, sizeof(g_staCfg.ssid));
  prefs.getString("user", g_staCfg.username, sizeof(g_staCfg.username));
  loadPassword(prefs, g_staCfg.password, sizeof(g_staCfg.password));
  if (prefs.isKey("pass")) savePassword(prefs, g_staCfg.password);
  g_apChannelSetting = prefs.getInt("apch", 0);
  g_apLastAutoChannel = prefs.getUChar("apauto", 6);
  prefs.getString("appass", g_apPassword, sizeof(g_apPassword));
  bool hasConfig = prefs.isKey("sec");
  prefs.end();

  if (strlen(g_apPassword) < 8) generateApPassword();

  // Migração: versões antigas salvavam a rede STA pelo
  // WiFiManager na config interna do driver Wi-Fi, não aqui. Se ainda
  // não existe config nossa, aproveita a rede que estava salva lá.
  if (!hasConfig) {
    wifi_config_t legacy;
    if (esp_wifi_get_config(WIFI_IF_STA, &legacy) == ESP_OK && legacy.sta.ssid[0] != '\0') {
      g_staCfg.security = WIFI_STA_PERSONAL;
      strlcpy(g_staCfg.ssid, (const char*)legacy.sta.ssid, sizeof(g_staCfg.ssid));
      strlcpy(g_staCfg.password, (const char*)legacy.sta.password, sizeof(g_staCfg.password));
      Serial.print("[WIFI] Rede salva pela versao anterior importada: ");
      Serial.println(g_staCfg.ssid);
    }
  }
}

static void saveConfig() {
  Preferences prefs;
  prefs.begin(WIFI_PREFS_NAMESPACE, false);
  prefs.putUChar("sec", g_staCfg.security);
  prefs.putUChar("eap", g_staCfg.eapMethod);
  prefs.putString("ssid", g_staCfg.ssid);
  prefs.putString("user", g_staCfg.username);
  savePassword(prefs, g_staCfg.password);
  prefs.putInt("apch", g_apChannelSetting);
  prefs.end();
}

static bool g_bootResetHeld = false;
bool wifiBootResetWasHeld() { return g_bootResetHeld; }

static bool bootResetButtonHeld() {
  pinMode(WIFI_RESET_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(WIFI_RESET_BUTTON_PIN) != LOW) return false;

  uint32_t startMs = millis();
  while (digitalRead(WIFI_RESET_BUTTON_PIN) == LOW) {
    if (millis() - startMs >= WIFI_RESET_HOLD_MS) return true;
  }
  return false;
}

// ---------------------------------------------------------------------
// Motivos de queda -> texto pra tela ⚙️
// ---------------------------------------------------------------------

static void describeReason(uint8_t reason, char* out, size_t outSize) {
  bool enterprise = (g_staCfg.security == WIFI_STA_ENTERPRISE);
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      strlcpy(out, "Rede não encontrada. Confira o nome e se ela está por perto", outSize);
      break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
      strlcpy(out, enterprise ? "Usuário ou senha incorretos" : "Senha incorreta", outSize);
      break;
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_NOT_ASSOCED:
      strlcpy(out, "Sinal fraco. Aproxime o cronômetro do roteador", outSize);
      break;
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
      strlcpy(out, "A rede recusou a conexão", outSize);
      break;
    default:
      (void)reason;
      strlcpy(out, "A conexão caiu", outSize);
      break;
  }
}

static void onStaDisconnected(arduino_event_id_t event, arduino_event_info_t info) {
  (void)event;
  uint8_t reason = info.wifi_sta_disconnected.reason;
  // ASSOC_LEAVE = nós mesmos desconectamos (nova tentativa / troca de rede).
  if (reason != WIFI_REASON_ASSOC_LEAVE) g_pendingReason = reason;
}

// ---------------------------------------------------------------------
// Canal do AP
// ---------------------------------------------------------------------

// Varre as redes ao redor e escolhe, entre os canais que não se
// sobrepõem (1, 6, 11), o de menor "ocupação": cada rede pesa pela força
// do sinal e pela proximidade do canal dela (canais a menos de 5 de
// distância se sobrepõem em 2,4 GHz).
static int pickQuietChannel() {
  // 80 ms por canal (padrão do core é 300 ms = ~4 s com o boot travado);
  // a busca ativa já recebe a resposta dos roteadores bem antes disso.
  int n = WiFi.scanNetworks(false, true, false, 80);
  if (n <= 0) {
    WiFi.scanDelete();
    Serial.println("[WIFI] Nenhuma rede ao redor - AP no canal 1.");
    return 1;
  }
  const int candidates[3] = { 1, 6, 11 };
  long score[3] = { 0, 0, 0 };
  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    int weight = WiFi.RSSI(i) + 100; // -30 dBm (forte) -> 70; -90 dBm (fraco) -> 10
    if (weight < 1) weight = 1;
    for (int k = 0; k < 3; k++) {
      int dist = abs(ch - candidates[k]);
      if (dist < 5) score[k] += (long)weight * (5 - dist);
    }
  }
  WiFi.scanDelete();

  int best = 0;
  for (int k = 1; k < 3; k++) if (score[k] < score[best]) best = k;
  Serial.printf("[WIFI] %d rede(s) ao redor. Ocupacao canal 1/6/11: %ld/%ld/%ld -> AP no canal %d\n",
                n, score[0], score[1], score[2], candidates[best]);
  return candidates[best];
}

// ---------------------------------------------------------------------
// Conexão STA
// ---------------------------------------------------------------------

static void startAttempt() {
  g_pendingReason = 0;
  WiFi.disconnect(false, false);

  if (g_staCfg.security == WIFI_STA_ENTERPRISE) {
    // Sem certificado da instituição (ca_pem = NULL): o servidor não é
    // verificado - mesmo que "Não validar" no Android.
    wpa2_auth_method_t method = (g_staCfg.eapMethod == WIFI_EAP_TTLS) ? WPA2_AUTH_TTLS : WPA2_AUTH_PEAP;
    WiFi.begin(g_staCfg.ssid, method, g_staCfg.username, g_staCfg.username, g_staCfg.password);
  } else {
    esp_wifi_sta_wpa2_ent_disable(); // caso a rede anterior fosse Enterprise
    WiFi.begin(g_staCfg.ssid, g_staCfg.password[0] ? g_staCfg.password : nullptr);
  }

  g_state = WIFI_STATE_CONNECTING;
  g_attemptStartMs = millis();
  Serial.printf("[WIFI] Tentando conectar em \"%s\" (%s)...\n", g_staCfg.ssid,
                g_staCfg.security == WIFI_STA_ENTERPRISE ? "Enterprise" : "WPA2 pessoal");
}

static void scheduleRetry() {
  g_failCount++;
  uint32_t waitMs = WIFI_STA_BACKOFF_MIN_MS;
  for (int i = 1; i < g_failCount && waitMs < WIFI_STA_BACKOFF_MAX_MS; i++) waitMs *= 2;
  if (waitMs > WIFI_STA_BACKOFF_MAX_MS) waitMs = WIFI_STA_BACKOFF_MAX_MS;

  WiFi.disconnect(false, false); // para de varrer canais enquanto espera - não atrapalha o AP
  g_state = WIFI_STATE_WAITING;
  g_retryAtMs = millis() + waitMs;
  Serial.printf("[WIFI] %s. Nova tentativa em %lu s.\n", g_lastError, (unsigned long)(waitMs / 1000));
}

// Atualiza o estado; chamado no boot (espera inicial) e em wifiUpdate().
static void stepConnection() {
  // Busca de redes que terminou sem a página vir buscar o resultado
  // (tela fechada no meio) - fecha aqui, senão a reconexão (que espera
  // a busca acabar) ficaria travada pra sempre.
  if (g_scanRunning && WiFi.scanComplete() != WIFI_SCAN_RUNNING) wifiScanResultCount();

  if (g_staCfg.security == WIFI_STA_NONE) return;

  uint8_t reason = g_pendingReason;
  if (reason) {
    g_pendingReason = 0;
    describeReason(reason, g_lastError, sizeof(g_lastError));
  }
  bool connected = (WiFi.status() == WL_CONNECTED);

  switch (g_state) {
    case WIFI_STATE_CONNECTING:
      if (connected) {
        g_state = WIFI_STATE_CONNECTED;
        g_failCount = 0;
        g_lastError[0] = '\0';
        Serial.printf("[WIFI] Rede externa conectada. IP: %s  Canal: %d  Sinal: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
      } else if (reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_AUTH_FAIL ||
                 reason == WIFI_REASON_802_1X_AUTH_FAILED) {
        scheduleRetry(); // falha definitiva desta tentativa - não adianta esperar o timeout
      } else if (millis() - g_attemptStartMs >= WIFI_STA_ATTEMPT_TIMEOUT_MS) {
        if (!g_lastError[0]) strlcpy(g_lastError, "A rede não respondeu", sizeof(g_lastError));
        scheduleRetry();
      }
      break;

    case WIFI_STATE_CONNECTED:
      if (!connected) {
        if (!g_lastError[0]) strlcpy(g_lastError, "A conexão caiu", sizeof(g_lastError));
        g_failCount = 0; // acabou de cair: 1a nova tentativa rápida
        scheduleRetry();
      }
      break;

    case WIFI_STATE_WAITING:
      // Não começa tentativa com corrida em andamento nem durante uma
      // busca de redes pedida na tela ⚙️ (as duas usam o rádio).
      if ((int32_t)(millis() - g_retryAtMs) >= 0 && currentState != SystemState::RUNNING && !g_scanRunning) {
        startAttempt();
      }
      break;

    case WIFI_STATE_OFF:
      startAttempt();
      break;
  }
}

// ---------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------

void wifiInit() {
  WiFi.persistent(false);        // credenciais ficam na NOSSA NVS (wifi_cfg), não na do driver
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);          // economia de energia do rádio aumenta latência e quedas
  WiFi.setAutoReconnect(false);  // reconexão é nossa (stepConnection) - a do core desiste fácil
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);      // procura em todos os canais...
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);  // ...e entra no roteador de sinal mais forte
  WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  loadConfig();
  if (bootResetButtonHeld()) {
    g_bootResetHeld = true;
    Serial.println("[WIFI] Botao mantido pressionado no boot - apagando rede externa salva.");
    g_staCfg.security = WIFI_STA_NONE;
    g_staCfg.ssid[0] = g_staCfg.username[0] = g_staCfg.password[0] = '\0';
    saveConfig();
  }

  displayShowConnecting(g_staCfg.security != WIFI_STA_NONE ? g_staCfg.ssid : nullptr);

  // Chave já em treino no boot: nem procura canal (usa o último escolhido),
  // pra o treino aparecer na hora.
  int channel;
  if (g_apChannelSetting >= 1 && g_apChannelSetting <= 13) channel = g_apChannelSetting;
  else if (trainingSwitchOn()) channel = g_apLastAutoChannel;
  else {
    channel = pickQuietChannel();
    if (channel != g_apLastAutoChannel) {
      g_apLastAutoChannel = channel;
      Preferences prefs;
      prefs.begin(WIFI_PREFS_NAMESPACE, false);
      prefs.putUChar("apauto", (uint8_t)channel);
      prefs.end();
    }
  }
  if (WiFi.softAP(WIFI_AP_SSID, g_apPassword, channel, 0, WIFI_AP_MAX_CLIENTS)) {
    Serial.printf("[WIFI] Access Point do sistema ativo. SSID: %s  IP: %s  Canal: %d\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str(), channel);
  } else {
    Serial.println("[WIFI] ERRO: falha ao iniciar o Access Point do sistema.");
  }

  if (g_staCfg.security == WIFI_STA_NONE) {
    Serial.println("[WIFI] Nenhuma rede externa configurada - so o AP proprio (configure na tela de Configuracoes).");
    return;
  }

  // Sem esperar aqui: a conexão segue em segundo plano (wifiUpdate) e o
  // display troca sozinho quando conectar (idle_screen). Esperar travava
  // o boot - a chave de treino só era lida depois.
  startAttempt();
}

void wifiUpdate() {
  int64_t now = esp_timer_get_time();
  if (now - g_lastCheckUs < (int64_t)WIFI_STATUS_CHECK_INTERVAL_US) return;
  g_lastCheckUs = now;
  stepConnection();
}

bool wifiIsStaConnected() { return g_state == WIFI_STATE_CONNECTED; }

void wifiGetStatusString(char* buffer, size_t bufferSize) {
  if (wifiIsStaConnected()) {
    snprintf(buffer, bufferSize, "STA:%s", WiFi.localIP().toString().c_str());
  } else {
    snprintf(buffer, bufferSize, "AP:%s", WiFi.softAPIP().toString().c_str());
  }
}

WifiStaConfig wifiGetStaConfig() { return g_staCfg; }

void wifiSetStaConfig(const WifiStaConfig &cfg) {
  g_staCfg = cfg;
  if (g_staCfg.ssid[0] == '\0') g_staCfg.security = WIFI_STA_NONE;
  saveConfig();

  g_failCount = 0;
  g_lastError[0] = '\0';
  if (g_staCfg.security == WIFI_STA_NONE) {
    WiFi.disconnect(false, false);
    esp_wifi_sta_wpa2_ent_disable();
    g_state = WIFI_STATE_OFF;
    Serial.println("[WIFI] Rede externa removida - so o AP proprio.");
  } else {
    startAttempt();
  }
}

int wifiGetApChannelSetting() { return g_apChannelSetting; }

void wifiSetApChannelSetting(int channel) {
  g_apChannelSetting = (channel >= 1 && channel <= 13) ? channel : 0;
  saveConfig();
}

bool wifiIsInitialAttempt() {
  return g_staCfg.security != WIFI_STA_NONE && g_state == WIFI_STATE_CONNECTING && g_failCount == 0;
}

WifiStaState wifiStaState() {
  return g_staCfg.security == WIFI_STA_NONE ? WIFI_STATE_OFF : g_state;
}

const char* wifiStaSsid() {
  return g_staCfg.security == WIFI_STA_NONE ? "" : g_staCfg.ssid;
}

void wifiStaIp(char* buffer, size_t bufferSize) {
  if (wifiIsStaConnected()) strlcpy(buffer, WiFi.localIP().toString().c_str(), bufferSize);
  else if (bufferSize) buffer[0] = '\0';
}

int wifiStaRssi() { return wifiIsStaConnected() ? WiFi.RSSI() : 0; }

const char* wifiLastError() { return g_lastError; }

int wifiStaSecondsToRetry() {
  if (g_state != WIFI_STATE_WAITING) return 0;
  int32_t left = (int32_t)(g_retryAtMs - millis());
  return left > 0 ? (left + 999) / 1000 : 0;
}

int wifiCurrentChannel() { return WiFi.channel(); }

int wifiApClientCount() { return WiFi.softAPgetStationNum(); }

void wifiApIp(char* buffer, size_t bufferSize) {
  strlcpy(buffer, WiFi.softAPIP().toString().c_str(), bufferSize);
}

void wifiScanStart() {
  if (g_scanRunning) return;
  // Uma tentativa de conexão em andamento também está usando o rádio
  // pra varrer - espera ela acabar (a página tenta de novo).
  if (g_state == WIFI_STATE_CONNECTING) {
    g_scanCount = -2;
    return;
  }
  WiFi.scanDelete();
  if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) {
    g_scanCount = -2;
    return;
  }
  g_scanRunning = true;
  g_scanCount = -1;
}

int wifiScanResultCount() {
  if (!g_scanRunning) return g_scanCount;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  g_scanRunning = false;
  if (!g_scan) g_scan = new (std::nothrow) WifiScanEntry[MAX_SCAN_ENTRIES];
  if (n < 0 || !g_scan) {
    WiFi.scanDelete();
    g_scanCount = -2;
    return g_scanCount;
  }

  // Uma linha por nome de rede (o mesmo nome em vários roteadores - comum
  // em faculdade - aparece uma vez só, com o sinal mais forte).
  g_scanCount = 0;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    int rssi = WiFi.RSSI(i);
    int existing = -1;
    for (int k = 0; k < g_scanCount; k++) {
      if (ssid == g_scan[k].ssid) { existing = k; break; }
    }
    if (existing >= 0) {
      if (rssi > g_scan[existing].rssi) {
        g_scan[existing].rssi = rssi;
        g_scan[existing].channel = WiFi.channel(i);
      }
      continue;
    }
    if (g_scanCount >= MAX_SCAN_ENTRIES) continue;
    WifiScanEntry &e = g_scan[g_scanCount++];
    strlcpy(e.ssid, ssid.c_str(), sizeof(e.ssid));
    e.rssi = rssi;
    e.channel = WiFi.channel(i);
    wifi_auth_mode_t auth = WiFi.encryptionType(i);
    e.enterprise = (auth == WIFI_AUTH_WPA2_ENTERPRISE);
    e.open = (auth == WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();

  // mais forte primeiro
  for (int i = 1; i < g_scanCount; i++) {
    WifiScanEntry tmp = g_scan[i];
    int j = i - 1;
    while (j >= 0 && g_scan[j].rssi < tmp.rssi) { g_scan[j + 1] = g_scan[j]; j--; }
    g_scan[j + 1] = tmp;
  }
  return g_scanCount;
}

bool wifiScanResultAt(int index, WifiScanEntry &out) {
  if (index < 0 || index >= g_scanCount) return false;
  out = g_scan[index];
  return true;
}
