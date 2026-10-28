#include "timer.h"
#include "esp_timer.h"
#include <stdio.h>

static int64_t startTimestampUs = 0;
static int64_t stopTimestampUs = 0;
static bool running = false;

void timerStart() {
  startTimestampUs = esp_timer_get_time();
  running = true;
}

void timerStop() {
  stopTimestampUs = esp_timer_get_time();
  running = false;
}

int64_t timerElapsedUs() {
  if (running) {
    return esp_timer_get_time() - startTimestampUs; // tempo ao vivo
  }
  return stopTimestampUs - startTimestampUs; // tempo final da última corrida
}

bool timerIsRunning() {
  return running;
}

int64_t nowUs() {
  return esp_timer_get_time();
}

void formatElapsedTime(int64_t elapsedUs, char* buffer, size_t bufferSize) {
  int64_t totalMs = elapsedUs / 1000;
  int ms = (int)(totalMs % 1000);
  int totalSec = (int)(totalMs / 1000);
  int s = totalSec % 60;
  int m = totalSec / 60;
  snprintf(buffer, bufferSize, "%02d:%02d.%03d", m, s, ms);
}
