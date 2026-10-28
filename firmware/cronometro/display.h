#pragma once
#include <stdint.h>

// =====================================================================
// DISPLAY.H — Display OLED (SSD1306, I2C, 128x64)
//
// Só DESENHA telas; quem decide QUAL tela mostrar fora de uma corrida é
// idle_screen.h. Todas as funções são no-op seguro se o display
// não tiver sido encontrado no barramento I2C.
//
// Textos podem vir em UTF-8 (nomes digitados na página, com acento): são
// convertidos para ASCII antes de desenhar ("Falcão" -> "Falcao") - a
// fonte do display não tem acentos - e cortados com "." se não couberem.
// =====================================================================

void displayInit();

// Conectando à rede externa (boot ou troca de rede na tela ⚙️). ssid
// nullptr = boot sem rede externa configurada ("Preparando a rede").
void displayShowConnecting(const char* ssid);

// QR code grande à esquerda (64 px de altura) + informação à direita: até
// dois blocos "RÓTULO" + valor; o valor quebra em linhas do que couber ao lado (11-12
// caracteres), preferindo quebrar depois de "." ou espaço. Usado para
// entrar no Wi-Fi do cronômetro (QR "WIFI:" + rede/senha) e para abrir o
// painel (QR com o endereço + o endereço escrito, para quem usa computador).
// label2/value2 podem ser nullptr.
void displayShowQrInfo(const char* payload, const char* label1, const char* value1,
                       const char* label2, const char* value2);

// Robô armado: released=false -> "aguardando liberação" do juiz;
// released=true -> liberado, esperando o robô passar pelo Sensor 1.
void displayShowArmed(const char* robotName, const char* teamName, int day, int attempt, bool released);

// Corrida terminada esperando o veredito do juiz na página.
void displayShowValidation(const char* robotName, const char* teamName, int64_t elapsedUs);

// Tela genérica de contexto: barra de título + linha principal (grande
// se couber) + linha secundária + rodapé. Qualquer linha pode ser "".
void displayShowContext(const char* title, const char* mainLine, const char* subLine, const char* footer);

// Alerta (sensor bloqueado, erro de memória...): título em destaque.
void displayShowWarning(const char* title, const char* line1, const char* line2);

// Tela do Modo Treino ociosa (sem corrida em andamento).
void displayShowTrainingIdle();

// Chamada repetidamente durante RUNNING (throttled internamente). Em
// Modo Treino, chamar com robotName == nullptr: mostra só o tempo,
// bem grande, sem nome/equipe. footer opcional (ex.: "Dia 1 - Tomada 2").
void displayShowRunning(int64_t elapsedUs, const char* robotName, const char* teamName, const char* footer = nullptr);

// Tela final, chamada uma única vez ao concluir a corrida. Mesma regra
// de robotName == nullptr para o Modo Treino. label: "FIM", "DNF"...
void displayShowFinished(int64_t elapsedUs, const char* robotName, const char* teamName, const char* label = "FIM");

void displayShowError(const char* message);
