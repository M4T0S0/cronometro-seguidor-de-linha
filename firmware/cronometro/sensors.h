#pragma once
#include <stdint.h>

// =====================================================================
// SENSORS.H
//
// Padrão usado: ISR minimalista (apenas timestamp + flag) + consumo
// no loop() principal. Isso é DELIBERADO: lógica de negócio dentro de
// uma ISR é a causa mais comum de crash/heap corruption em projetos
// ESP32 com sensores (já vivemos isso no projeto anterior).
// =====================================================================

void sensorsInit();

// Chamar a cada iteração do loop(). Verifica condição de sensor travado.
void sensorsUpdate();

// Retorna true se houve um evento NOVO e válido (pós-debounce) desde a
// última chamada, preenchendo o timestamp em microssegundos (esp_timer).
// Consome o evento (chamadas subsequentes retornam false até o próximo).
bool sensor1Consume(int64_t &timestampUs);
bool sensor2Consume(int64_t &timestampUs);

// Indica se algum sensor está sinalizado como travado/obstruído.
bool sensor1IsStuck();
bool sensor2IsStuck();

// Descarta qualquer evento pendente SEM processá-lo. Usado nas transições
// de estado para evitar que um evento "velho" (ex.: ruído do sensor 2
// enquanto o sistema ainda estava em WAITING_START) seja interpretado
// como se tivesse ocorrido depois da transição atual.
void sensor1Flush();
void sensor2Flush();
