#pragma once
#include <stdint.h>
#include <stddef.h>

// =====================================================================
// TIMER.H — Cronômetro monotônico de alta precisão
//
// Usa esp_timer_get_time(), que retorna microssegundos desde o boot,
// baseado em um timer de hardware do ESP32. Vantagens sobre millis():
//  - Resolução de microssegundos (millis() só dá 1ms)
//  - Não sofre com jitter de chamadas dentro do loop()
//  - Monotônico: nunca "anda para trás", imune a ajuste de NTP/RTC
//    (que só afetam relógio de parede, nunca este timer)
//
// Este timer mede APENAS duração de corrida (start->stop).
// Data/hora real do evento é responsabilidade de outro módulo.
// =====================================================================

void timerStart();
void timerStop();

// MUDANÇA: antes só era válido após timerStop(). Agora também
// funciona enquanto o cronômetro está rodando (retorna o tempo decorrido
// ATÉ AGORA) — necessário para o OLED atualizar o tempo ao vivo durante
// a corrida (RUNNING), sem precisar de uma segunda função/API paralela.
int64_t timerElapsedUs();

bool timerIsRunning();

// Relógio monotônico bruto (microssegundos desde o boot), para uso em
// lógica de temporização fora do cronômetro em si (ex.: hold de tela no
// display). Mantém o acesso ao esp_timer centralizado neste módulo.
int64_t nowUs();

// Formata um valor em microssegundos como "mm:ss.mmm".
// 'buffer' deve ter pelo menos 13 bytes. Compartilhado entre o log
// Serial e o display OLED, para não duplicar a lógica de formatação.
void formatElapsedTime(int64_t elapsedUs, char* buffer, size_t bufferSize);
