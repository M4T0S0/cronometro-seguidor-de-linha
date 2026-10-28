#pragma once
#include <stdint.h>
#include <stddef.h>

// =====================================================================
// WIFI.H — Conectividade Wi-Fi: AP próprio + STA (rede externa)
//
// Como funciona:
//  - AP PRÓPRIO do sistema sobe logo no boot, SEMPRE, no canal menos
//    congestionado entre 1/6/11 (varredura no boot) ou num canal fixo
//    escolhido na tela ⚙️. Aceita até WIFI_AP_MAX_CLIENTS aparelhos.
//  - Rede EXTERNA (STA) configurada pela tela ⚙️ da página do juiz
//    (não existe mais o portal "ConfigCronometro"/WiFiManager): WPA2
//    pessoal (só senha) ou WPA2-Enterprise (usuário/RA + senha - eduroam
//    e redes institucionais, PEAP ou TTLS).
//  - Reconexão própria, com espera crescente entre tentativas
//    (WIFI_STA_BACKOFF_MIN_MS..MAX_MS), pra sempre - não depende do
//    auto-reconnect do core, que desiste em vários motivos de queda.
//    Cada tentativa varre os canais e atrapalha o AP por ~2 s, por isso
//    a espera cresce e nenhuma tentativa começa com corrida em andamento.
//  - Guarda o motivo da última queda (senha errada, rede não
//    encontrada...) pra tela ⚙️ mostrar.
// =====================================================================

enum WifiStaSecurity : uint8_t {
  WIFI_STA_NONE = 0,        // sem rede externa - só o AP próprio
  WIFI_STA_PERSONAL = 1,    // WPA2 pessoal (ou rede aberta, se senha vazia)
  WIFI_STA_ENTERPRISE = 2,  // WPA2-Enterprise (usuário + senha)
};

enum WifiEapMethod : uint8_t {
  WIFI_EAP_PEAP = 0,  // padrão da eduroam e da maioria das universidades
  WIFI_EAP_TTLS = 1,
};

struct WifiStaConfig {
  uint8_t security;   // WifiStaSecurity
  uint8_t eapMethod;  // WifiEapMethod (só Enterprise)
  char ssid[33];
  char username[64];  // só Enterprise (RA, e-mail institucional...)
  char password[64];
};

enum WifiStaState : uint8_t {
  WIFI_STATE_OFF = 0,        // nenhuma rede externa configurada
  WIFI_STATE_CONNECTING = 1, // tentativa em andamento
  WIFI_STATE_CONNECTED = 2,
  WIFI_STATE_WAITING = 3,    // falhou/caiu - esperando pra tentar de novo
};

void wifiInit();

// Chamar a cada iteração do loop() - conduz a (re)conexão da rede externa.
void wifiUpdate();

// true se o botão de reset estava pressionado no boot (também apaga a
// senha do painel, ver webServerInit).
bool wifiBootResetWasHeld();

// Senha do Wi-Fi próprio do cronômetro (8 caracteres, sorteada no
// primeiro boot e guardada; aparece no display).
const char* wifiApPassword();

bool wifiIsStaConnected();

// Preenche 'buffer' com o IP mais relevante no momento: o da rede externa
// se conectado, senão o do AP próprio. Buffer deve ter pelo menos 24 bytes.
void wifiGetStatusString(char* buffer, size_t bufferSize);

// ------------------- Configuração (tela ⚙️) -------------------
WifiStaConfig wifiGetStaConfig();
// Salva na NVS e já começa a conectar na rede nova (derruba a atual).
void wifiSetStaConfig(const WifiStaConfig &cfg);

// 0 = automático (canal mais livre no boot); 1..13 = fixo. Só vale a
// partir do próximo boot - trocar o canal do AP agora derrubaria quem
// está conectado nele (inclusive quem está mexendo na tela ⚙️).
int wifiGetApChannelSetting();
void wifiSetApChannelSetting(int channel);

// ------------------- Status (tela ⚙️) -------------------
WifiStaState wifiStaState();
// true durante a 1a tentativa de conexão na rede externa (boot ou logo
// depois de trocar a rede na ⚙️) - o display mostra "Conectando". As
// tentativas seguintes (reconexão) não ocupam o display.
bool wifiIsInitialAttempt();
const char* wifiStaSsid();       // rede externa configurada ("" se nenhuma)
void wifiStaIp(char* buffer, size_t bufferSize);
int wifiStaRssi();               // dBm; 0 se desconectado
const char* wifiLastError();     // motivo legível da última falha/queda ("" se nenhuma)
int wifiStaSecondsToRetry();     // só em WIFI_STATE_WAITING
int wifiCurrentChannel();        // canal em uso pelo rádio agora (AP e STA dividem o mesmo)
int wifiApClientCount();
void wifiApIp(char* buffer, size_t bufferSize);

// ------------------- Busca de redes (tela ⚙️) -------------------
// Assíncrona: wifiScanStart() dispara; wifiScanResultCount() devolve -1
// enquanto ainda está varrendo, -2 se falhou, ou a quantidade de redes.
struct WifiScanEntry {
  char ssid[33];
  int rssi;
  int channel;
  bool enterprise;
  bool open;
};
void wifiScanStart();
int wifiScanResultCount();
bool wifiScanResultAt(int index, WifiScanEntry &out);
