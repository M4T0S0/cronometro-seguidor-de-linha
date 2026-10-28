#pragma once

// =====================================================================
// PERIPHERALS.H — LEDs de status + buzzer
//
// Buzzer não-bloqueante de propósito: nada de delay() aqui, que
// travaria sensores/WebServer pela duração do bipe. peripheralsUpdate()
// precisa ser chamada a cada iteração do loop() (mesmo padrão de
// sensorsUpdate()) pra desligar o buzzer sozinho quando o tempo passar.
// =====================================================================

void peripheralsInit();

// Chamar a cada iteração do loop() - desliga o buzzer quando o tempo
// de bipe (BUZZER_BEEP_DURATION_US) tiver passado.
void peripheralsUpdate();

void ledSetGreen(bool on);
void ledSetRed(bool on);

// Dispara um bipe curto (não bloqueia). Chamadas repetidas antes do
// bipe anterior terminar simplesmente reiniciam a contagem.
void buzzerBeep();