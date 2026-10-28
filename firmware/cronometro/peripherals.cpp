#include "peripherals.h"
#include "config.h"
#include <Arduino.h>
#include "esp_timer.h"

static bool buzzerActive = false;
static int64_t buzzerOffAtUs = 0;

void peripheralsInit() {
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_BUZZER, LOW);
  Serial.println("[PERIFERICOS] LEDs e buzzer inicializados.");
}

void ledSetGreen(bool on) { digitalWrite(PIN_LED_GREEN, on ? HIGH : LOW); }
void ledSetRed(bool on)   { digitalWrite(PIN_LED_RED, on ? HIGH : LOW); }

void buzzerBeep() {
  tone(PIN_BUZZER, BUZZER_FREQUENCY_HZ);
  buzzerActive = true;
  buzzerOffAtUs = esp_timer_get_time() + (int64_t)BUZZER_BEEP_DURATION_US;
}

void peripheralsUpdate() {
  if (buzzerActive && esp_timer_get_time() >= buzzerOffAtUs) {
    noTone(PIN_BUZZER);
    buzzerActive = false;
  }
}