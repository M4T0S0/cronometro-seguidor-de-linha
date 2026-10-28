#pragma once

// =====================================================================
// CONFIG.H — Configurações centralizadas do firmware
// Nenhum valor de pino, timing ou rede deve ser hardcoded fora daqui.
// =====================================================================

// ------------------- PINOUT -------------------
// GPIO 34/35 são input-only no ESP32 (sem pull-up/pull-down interno).
// Se o E18-D80NK usado tiver saída NPN/open-collector, é OBRIGATÓRIO
// resistor de pull-up externo (ex.: 10k para 3.3V) em cada pino.
// NOTA: 26/25 conforme esquematico fisico do projeto (sensores ligados
// atraves de divisor resistivo 100k/220k, que ja define o nivel logico
// externamente - por isso NAO usamos pull-up interno aqui, ver sensors.cpp).
#define PIN_SENSOR1 25   // START
#define PIN_SENSOR2 26   // STOP

// Nível lógico do sensor quando ACIONADO (detectou o objeto).
// E18-D80NK: por padrão a saída vai para LOW quando detecta.
// Ajuste aqui caso o comportamento do seu módulo específico seja invertido.
#define SENSOR_ACTIVE_LEVEL LOW

// ------------------- TIMING / PRECISÃO -------------------
// Debounce: tempo mínimo entre duas bordas válidas do MESMO sensor.
// Abaixo disso, a segunda borda é considerada ruído/bounce.
#define SENSOR_DEBOUNCE_US        50000UL     // 50 ms

// Lockout: tempo mínimo entre um START válido e um STOP válido.
// Protege contra STOP falso disparado por ruído logo após o START
// (nenhuma corrida real dura menos que isso).
#define SENSOR_LOCKOUT_US         300000UL    // 300 ms

// Se um sensor permanecer no nível "acionado" continuamente por mais
// tempo que isso, consideramos ele travado/obstruído e sinalizamos.
#define SENSOR_STUCK_TIMEOUT_US   10000000ULL // 10 s

// ------------------- SERIAL (debug / bancada) -------------------
#define SERIAL_BAUD 115200

// ------------------- OLED -------------------
#define OLED_SDA_PIN 21
#define OLED_SCL_PIN 22
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_RESET_PIN -1     // sem pino de reset dedicado (compartilha reset do ESP32)
#define OLED_I2C_ADDRESS 0x3C // confirme no seu módulo específico; alguns usam 0x3D

// Limita a taxa de redesenho do display durante RUNNING. Mesmo sem
// conseguir ler os dígitos com nitidez nessa velocidade, ver todos eles
// se mexendo passa mais confiança de que é um cronômetro de verdade em
// funcionamento (não uma tela travada) — balanceado para não saturar o
// barramento I2C (ver Wire.setClock em display.cpp).
//
// IMPORTANTE: o valor NÃO deve ser múltiplo de 10 — se fosse (ex.: 40ms,
// 100ms), o instante de amostragem sempre cairia num múltiplo de 10ms,
// fazendo o último dígito do milissegundo parecer sempre travado em
// zero (não é limitação do olho humano, é artefato da amostragem).
#define DISPLAY_UPDATE_INTERVAL_US 43000UL // 43 ms (~23 fps, não-múltiplo de 10)

// Tempo que a tela de resultado final (FINISHED) permanece visível antes
// de o sistema rearmar automaticamente para a próxima corrida. Sem isso,
// a tela "FINALIZADO" seria sobrescrita na mesma iteração do loop() em
// que aparece, tornando-a invisível para o competidor.
#define RESULT_DISPLAY_HOLD_US 3000000UL // 3 s

// Fallback: se por algum motivo uma corrida terminar sem contexto válido
// (nada armado — não deveria acontecer, já que o sensor só é liberado
// depois de armar localmente, mas fica como defesa: falha
// de contexto nunca perde o resultado, só cai num arquivo plano em vez da
// pasta estruturada da competição).
#define SD_QUEUE_FILENAME "/avulsas.jsonl"

// ------------------- WI-FI -------------------
// Access Point PRÓPRIO do sistema (hub) — sempre ativo, independente de
// a rede externa (STA) ter conectado ou não. Garante funcionamento
// mesmo sem infraestrutura externa. A senha não
// fica no código: cada cronômetro sorteia a sua no primeiro
// boot e mostra no display (ver wifiApPassword()).
#define WIFI_AP_SSID     "CronometroCompeticao"
#define WIFI_AP_MAX_CLIENTS 8 // padrão do core é 4 - pouco quando juiz + equipe + TV usam a rede

// Canal do AP próprio: escolhido no boot (o menos congestionado entre
// 1/6/11, ver wifi.cpp) a menos que um canal fixo tenha sido salvo na
// tela ⚙️. Quando a rede externa (STA) conecta, o rádio passa a usar o
// canal DELA - o ESP32 só tem um rádio, AP e STA dividem o canal.

// Rede EXTERNA (STA): credenciais NÃO ficam hardcoded - são configuradas
// na tela ⚙️ da página do juiz (WPA2 pessoal ou WPA2-Enterprise com
// usuário/RA + senha) e salvas na NVS do ESP32.
#define WIFI_STA_ATTEMPT_TIMEOUT_MS 20000UL // desiste de UMA tentativa depois disso (Enterprise demora mais)
#define WIFI_STA_BACKOFF_MIN_MS      5000UL // espera depois da 1a falha...
#define WIFI_STA_BACKOFF_MAX_MS     60000UL // ...dobrando a cada falha, até este teto
// Display: a página do juiz avisa a cada ~10 s em que tela está;
// sem aviso por este tempo (celular bloqueado, página fechada), o display
// volta a mostrar como acessar o painel (rede, senha e endereço).
#define JUDGE_CONTEXT_TIMEOUT_US (45ULL * 1000000ULL)

// Chave de seleção de modo (Treino x Competição), lida UMA VEZ no boot.
// GND = Modo Treino; aberta (pull-up interno) = Modo Competição (padrão seguro).
#define PIN_MODE_SELECT 32
#define PIN_LED_GREEN 27
#define PIN_LED_RED   14
#define PIN_BUZZER    13
#define BUZZER_BEEP_DURATION_US 80000UL // 80 ms
#define BUZZER_FREQUENCY_HZ 2500 // só usado se o buzzer for passivo

// Botão físico (GPIO 33): se mantido pressionado durante o BOOT, apaga a
// rede externa (STA) salva - o cronômetro sobe só com o AP próprio. O
// "abortar corrida" do juiz não usa botão físico nenhum — é a página web
// (POST /api/v1/arm/abort, ver web_server.cpp), então não há conflito de
// uso deste pino em outro momento.
#define WIFI_RESET_BUTTON_PIN 33
#define WIFI_RESET_HOLD_MS    3000UL

// Intervalo mínimo entre checagens de status Wi-Fi no loop() — evita
// overhead de CPU a cada iteração.
#define WIFI_STATUS_CHECK_INTERVAL_US 1000000UL // 1 s

// ------------------- SERVIDOR WEB LOCAL (versão standalone) -------------------
// Sem servidor externo: o próprio ESP32 hospeda a API + a página do juiz
// (ver web_server.h/cpp). Nenhuma URL/timeout de rede a configurar aqui —
// tudo é local (RAM + flash), não há chamada HTTP de saída no sistema.
#define HTTP_SERVER_PORT 80

// ------------------- ARMAZENAMENTO LOCAL (substitui o banco SQLite) -------------------
// Tetos de RAM para os "dados cadastrais" (participantes/competições/
// vínculos), carregados da flash para arrays fixos no boot (ver
// data_store.h). Corridas em si NÃO entram nesses tetos — continuam
// vivendo em /<slug>/Dia_N/resultados.jsonl na flash (ver storage.h);
// MAX_RUNS_PER_DAY só limita quantas linhas de UM dia cabem de uma vez
// na RAM ao montar ranking/day-status/lista de corridas.
#define MAX_PARTICIPANTS   80
#define MAX_COMPETITIONS   20
#define MAX_LINKS          400
#define MAX_RUNS_PER_DAY   150
#define MAX_TEAMS 30 

// Teto de segurança para GET .../runs (que soma as linhas de TODOS os
// dias de uma competição num único JSON): evita que uma competição com
// muitos dias/tentativas gere uma resposta grande o bastante para
// esgotar a RAM livre do ESP32 (WiFi + WebServer + ArduinoJson já usam
// uma fatia considerável). Bem acima do uso real esperado (dezenas de
// corridas), só como rede de segurança.
#define MAX_RUNS_LIST_TOTAL 300
