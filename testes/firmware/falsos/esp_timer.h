// Relógio do ESP32 "de mentira": os testes controlam o tempo (fakeTimeUs).
#pragma once
#include <cstdint>

int64_t& fakeTimeUs();
inline int64_t esp_timer_get_time() { return fakeTimeUs(); }
