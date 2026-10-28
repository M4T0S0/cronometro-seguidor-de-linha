#include "display.h"
#include "config.h"
#include "timer.h"
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_timer.h"
#include "qrcode.h" // gerador de QR do próprio ESP-IDF (vem no core, sem biblioteca extra)

static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
static bool displayOk = false;
static int64_t lastRunningUpdateUs = 0;

// Fonte padrão do Adafruit GFX: 6x8 px por caractere no tamanho 1.
static const int CHARS_SIZE1 = OLED_WIDTH / 6;  // 21
static const int CHARS_SIZE2 = OLED_WIDTH / 12; // 10
static const int HEADER_H = 12;

void displayInit() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(400000);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println("[DISPLAY] ERRO: SSD1306 nao encontrado no barramento I2C.");
    displayOk = false;
    return;
  }

  displayOk = true;
  oled.cp437(true);
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
  Serial.println("[DISPLAY] OLED inicializado.");
}

// ---------------------------------------------------------------------
// Texto: UTF-8 -> ASCII e corte
// ---------------------------------------------------------------------

// Letras latinas acentuadas (UTF-8 0xC3 0x80..0xBF) -> letra sem acento.
static const char LATIN1_TO_ASCII[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

// Converte para ASCII imprimível e corta em maxChars (último vira ".").
static void toDisplayText(const char* in, char* out, size_t outSize, int maxChars) {
  size_t w = 0;
  const unsigned char* p = (const unsigned char*)(in ? in : "");
  while (*p && w + 1 < outSize) {
    char c;
    if (*p < 0x80) {
      c = (*p >= 0x20) ? (char)*p : ' ';
      p++;
    } else if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
      c = LATIN1_TO_ASCII[p[1] - 0x80];
      p += 2;
    } else if (*p == 0xC2 && p[1]) {
      c = (p[1] == 0xBA) ? 'o' : (p[1] == 0xAA) ? 'a' : (p[1] == 0xB7) ? '.' : ' '; // º ª ·
      p += 2;
    } else {
      // outro caractere multibyte (emoji etc.): pula a sequência inteira
      p++;
      while ((*p & 0xC0) == 0x80) p++;
      c = '?';
    }
    out[w++] = c;
  }
  out[w] = '\0';
  if ((int)w > maxChars && maxChars > 1) {
    out[maxChars - 1] = '.';
    out[maxChars] = '\0';
  }
}

static void drawCenteredRaw(const char* text, int16_t y, uint8_t textSize) {
  oled.setTextSize(textSize);
  int16_t x1, y1;
  uint16_t w, h;
  oled.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (OLED_WIDTH - (int16_t)w) / 2;
  if (x < 0) x = 0;
  oled.setCursor(x, y);
  oled.print(text);
}

// Linha centralizada no tamanho 1 (convertida/cortada).
static void drawLine(const char* text, int16_t y) {
  char buf[48];
  toDisplayText(text, buf, sizeof(buf), CHARS_SIZE1);
  if (buf[0]) drawCenteredRaw(buf, y, 1);
}

// Linha principal: tamanho 2 se couber (até 10 caracteres), senão tamanho
// 1 centralizada na mesma faixa de 16 px. Retorna a altura usada.
static int16_t drawMainLine(const char* text, int16_t y) {
  char buf[48];
  toDisplayText(text, buf, sizeof(buf), 40);
  if (!buf[0]) return 16;
  if ((int)strlen(buf) <= CHARS_SIZE2) {
    drawCenteredRaw(buf, y, 2);
  } else {
    toDisplayText(text, buf, sizeof(buf), CHARS_SIZE1);
    drawCenteredRaw(buf, y + 4, 1);
  }
  return 16;
}

// Barra de título invertida (texto preto sobre faixa branca).
static void drawHeader(const char* title) {
  oled.fillRect(0, 0, OLED_WIDTH, HEADER_H, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  drawLine(title, 2);
  oled.setTextColor(SSD1306_WHITE);
}

static void formatDayAttempt(char* buf, size_t size, int day, int attempt) {
  if (day > 0 && attempt > 0) snprintf(buf, size, "Dia %d - Tomada %d", day, attempt);
  else buf[0] = '\0';
}

// ---------------------------------------------------------------------
// Telas
// ---------------------------------------------------------------------

void displayShowConnecting(const char* ssid) {
  if (!displayOk) return;
  oled.clearDisplay();
  drawHeader("WI-FI");
  if (ssid && ssid[0]) {
    drawLine("Conectando a rede", 18);
    drawMainLine(ssid, 30);
  } else {
    drawLine("Preparando a rede", 26);
  }
  drawLine("aguarde...", 54);
  oled.display();
}

// O ESP-IDF entrega o QR pronto num callback - a posição/escala de desenho
// passam por aqui (só uma tela desenha QR por vez, sem concorrência).
// Largura da área clara do QR desenhado por último: o texto começa depois dela.
static int g_qrAreaWidth = 64;

static void drawQrModules(esp_qrcode_handle_t qr) {
  int size = esp_qrcode_get_size(qr);
  // Maior escala inteira que cabe em 64x64 deixando borda clara de pelo
  // menos 2 px (a "zona de silêncio" que a câmera usa para achar o QR):
  // versão 3 (Wi-Fi, 29 módulos) -> 2 px/módulo = 58 px; versão 2
  // (endereço, 25 módulos) -> 2 px/módulo = 50 px com borda maior.
  int scale = (64 - 4) / size;
  if (scale < 1) scale = 1;
  int px = size * scale;
  // Na vertical a borda é o que sobra dos 64 px; na horizontal fica só
  // 2 px - o resto da largura vai para o texto ao lado.
  int x0 = 2;
  int y0 = (64 - px) / 2;
  g_qrAreaWidth = px + 4;
  // Fundo claro (pixels acesos) com módulos escuros = QR "normal", o que
  // qualquer câmera lê; o QR invertido (claro sobre escuro) falha em muitas.
  oled.fillRect(0, 0, g_qrAreaWidth, 64, SSD1306_WHITE);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qr, x, y)) oled.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, SSD1306_BLACK);
    }
  }
}

// Quebra 'text' (já em ASCII) em linhas de até 'width' caracteres,
// preferindo quebrar logo depois de ".", espaço, "-", "_" ou entre uma
// minúscula e uma maiúscula. Máximo
// maxLines linhas (a última ganha "." se cortar). Retorna quantas.
static int wrapText(const char* text, char lines[][22], int maxLines, int width) {
  int n = 0;
  const char* p = text;
  while (*p && n < maxLines) {
    int len = strlen(p);
    int take = len;
    if (len > width) {
      take = width;
      for (int i = width; i > width / 2; i--) {
        char c = p[i - 1];
        // depois de pontuação/espaço, ou numa troca minúscula->maiúscula
        // ("Cronometro|Competicao")
        bool camel = (c >= 'a' && c <= 'z') && (p[i] >= 'A' && p[i] <= 'Z');
        if (c == '.' || c == ' ' || c == '-' || c == '_' || camel) { take = i; break; }
      }
    }
    memcpy(lines[n], p, take);
    lines[n][take] = '\0';
    p += take;
    while (*p == ' ') p++;
    n++;
  }
  if (*p && n > 0) lines[n - 1][strlen(lines[n - 1]) - 1] = '.';
  return n;
}

void displayShowQrInfo(const char* payload, const char* label1, const char* value1,
                       const char* label2, const char* value2) {
  if (!displayOk) return;
  oled.clearDisplay();
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = drawQrModules;
  cfg.max_qrcode_version = 5;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  g_qrAreaWidth = 64;
  if (esp_qrcode_generate(&cfg, payload) != ESP_OK) {
    drawLine("QR indisponivel", 28);
  }

  // Coluna da direita: o que sobra depois da área clara do QR, em
  // caracteres de 6 px - 11 ao lado do QR do Wi-Fi, 12 ao lado do QR do
  // endereço; 6 linhas de 10 px.
  const int colX = g_qrAreaWidth;
  const int colW = OLED_WIDTH - colX;
  const int width = colW / 6;
  char lines[6][22];
  int n = 0;
  char buf[48];
  const char* labels[2] = { label1, label2 };
  const char* values[2] = { value1, value2 };
  for (int b = 0; b < 2 && n < 6; b++) {
    if (!labels[b] && !values[b]) continue;
    if (labels[b] && labels[b][0]) {
      toDisplayText(labels[b], buf, sizeof(buf), width);
      strlcpy(lines[n++], buf, sizeof(lines[0]));
    }
    if (values[b] && values[b][0] && n < 6) {
      toDisplayText(values[b], buf, sizeof(buf), 40);
      // o 1o bloco não pode tomar as linhas que o 2o bloco precisa
      int reserve = (b == 0 && label2) ? 2 : 0;
      n += wrapText(buf, &lines[n], 6 - n - reserve, width);
    }
  }
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  int16_t y = (64 - n * 10) / 2 + 1;
  for (int i = 0; i < n; i++, y += 10) {
    int16_t w = strlen(lines[i]) * 6;
    oled.setCursor(colX + (colW - w) / 2, y);
    oled.print(lines[i]);
  }
  oled.display();
}

void displayShowArmed(const char* robotName, const char* teamName, int day, int attempt, bool released) {
  if (!displayOk) return;
  char footer[32];
  formatDayAttempt(footer, sizeof(footer), day, attempt);
  oled.clearDisplay();
  drawHeader(released ? "LIBERADO - PODE IR" : "AGUARDANDO LIBERACAO");
  drawMainLine(robotName, 16);
  drawLine(teamName, 36);
  drawLine(footer, 54);
  oled.display();
}

void displayShowValidation(const char* robotName, const char* teamName, int64_t elapsedUs) {
  if (!displayOk) return;
  char timeStr[16];
  formatElapsedTime(elapsedUs, timeStr, sizeof(timeStr));
  oled.clearDisplay();
  drawHeader("AGUARDANDO VALIDACAO");
  drawLine(robotName, 16);
  drawLine(teamName, 26);
  drawCenteredRaw(timeStr, 42, 2);
  oled.display();
}

void displayShowContext(const char* title, const char* mainLine, const char* subLine, const char* footer) {
  if (!displayOk) return;
  oled.clearDisplay();
  drawHeader(title);
  drawMainLine(mainLine, 18);
  drawLine(subLine, 39);
  drawLine(footer, 54);
  oled.display();
}

void displayShowWarning(const char* title, const char* line1, const char* line2) {
  if (!displayOk) return;
  oled.clearDisplay();
  drawHeader("ATENCAO");
  drawMainLine(title, 18);
  drawLine(line1, 40);
  drawLine(line2, 52);
  oled.display();
}

void displayShowTrainingIdle() {
  if (!displayOk) return;
  oled.clearDisplay();
  drawHeader("MODO TREINO");
  drawCenteredRaw("PRONTO", 22, 2);
  drawLine("passe pelo Sensor 1", 52);
  oled.display();
}

void displayShowRunning(int64_t elapsedUs, const char* robotName, const char* teamName, const char* footer) {
  if (!displayOk) return;

  int64_t now = esp_timer_get_time();
  if (now - lastRunningUpdateUs < (int64_t)DISPLAY_UPDATE_INTERVAL_US) return;
  lastRunningUpdateUs = now;

  char timeStr[16];
  formatElapsedTime(elapsedUs, timeStr, sizeof(timeStr));

  oled.clearDisplay();

  if (robotName == nullptr) {
    // Modo Treino: só o tempo, bem grande, nada mais na tela.
    drawHeader("TREINO");
    drawCenteredRaw(timeStr, 28, 2);
  } else {
    drawHeader(robotName);
    drawLine(teamName ? teamName : "", 16);
    drawCenteredRaw(timeStr, 30, 2);
    drawLine(footer ? footer : "", 54);
  }

  oled.display();
}

void displayShowFinished(int64_t elapsedUs, const char* robotName, const char* teamName, const char* label) {
  if (!displayOk) return;

  char timeStr[16];
  formatElapsedTime(elapsedUs, timeStr, sizeof(timeStr));

  oled.clearDisplay();

  if (robotName == nullptr) {
    drawHeader(label ? label : "FIM");
    drawCenteredRaw(timeStr, 28, 2);
  } else {
    drawHeader(label ? label : "FIM");
    drawLine(robotName, 16);
    drawLine(teamName ? teamName : "", 26);
    drawCenteredRaw(timeStr, 42, 2);
  }

  oled.display();
}

void displayShowError(const char* message) {
  if (!displayOk) return;
  char buf[48];
  toDisplayText(message, buf, sizeof(buf), 42);
  oled.clearDisplay();
  drawHeader("ERRO");
  oled.setTextSize(1);
  oled.setCursor(0, 20);
  oled.println(buf);
  oled.display();
}
