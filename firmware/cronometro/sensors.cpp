#include "sensors.h"
#include "config.h"
#include <Arduino.h>
#include "esp_timer.h"

// ---------------------------------------------------------------------
// Estado interno por sensor. Tudo 'volatile' pois é escrito pela ISR
// e lido pelo loop principal (dois contextos de execução distintos).
// ---------------------------------------------------------------------
struct SensorState {
  uint8_t pin;
  volatile int64_t lastValidEdgeUs;   // para debounce
  volatile int64_t pendingEventUs;    // timestamp do evento a consumir
  volatile bool pendingFlag;          // há evento novo não consumido?
  volatile int64_t activeSinceUs;     // desde quando está no nível ativo (0 = não está ativo)
  volatile bool stuckFlag;
};

static SensorState s1 = {PIN_SENSOR1, 0, 0, false, 0, false};
static SensorState s2 = {PIN_SENSOR2, 0, 0, false, 0, false};

// ---------------------------------------------------------------------
// ISR genérica (chamada pelas duas ISRs específicas abaixo).
// Regra: SÓ timestamp + comparação de debounce. Nada de Serial, nada
// de malloc, nada de lógica de negócio aqui dentro.
// ---------------------------------------------------------------------
static void IRAM_ATTR handleEdge(SensorState *s) {
  int64_t now = esp_timer_get_time();

  if (now - s->lastValidEdgeUs < (int64_t)SENSOR_DEBOUNCE_US) {
    return; // borda descartada: ruído/bounce dentro da janela de debounce
  }

  s->lastValidEdgeUs = now;
  s->pendingEventUs = now;
  s->pendingFlag = true;
}

static void IRAM_ATTR isrSensor1() { handleEdge(&s1); }
static void IRAM_ATTR isrSensor2() { handleEdge(&s2); }

void sensorsInit() {
  // Sensores conectados via divisor resistivo externo (100k/220k) -
  // o nivel logico ja e definido por essa rede, entao usamos INPUT puro.
  // Pull-up interno (~45k) teria magnitude comparavel ao divisor e
  // distorceria a tensao lida no pino.
  pinMode(s1.pin, INPUT);
  pinMode(s2.pin, INPUT);

  // Interrompe na borda que corresponde ao nível "acionado" configurado.
  int mode = (SENSOR_ACTIVE_LEVEL == LOW) ? FALLING : RISING;

  attachInterrupt(digitalPinToInterrupt(s1.pin), isrSensor1, mode);
  attachInterrupt(digitalPinToInterrupt(s2.pin), isrSensor2, mode);

  Serial.println("[SENSORS] Inicializados (interrupcao + debounce ativo)");
}

// ---------------------------------------------------------------------
// Detecção de sensor travado: só pode ser feita por POLLING, pois um
// sensor preso no nível ativo não gera novas bordas (logo, não gera
// interrupção nova). Por isso sensorsUpdate() precisa ser chamada
// continuamente no loop().
// ---------------------------------------------------------------------
static void checkStuck(SensorState *s) {
  bool active = (digitalRead(s->pin) == SENSOR_ACTIVE_LEVEL);
  int64_t now = esp_timer_get_time();

  if (active) {
    if (s->activeSinceUs == 0) {
      s->activeSinceUs = now; // começou a ficar ativo agora
    } else if (!s->stuckFlag && (now - s->activeSinceUs > (int64_t)SENSOR_STUCK_TIMEOUT_US)) {
      s->stuckFlag = true;
      Serial.print("[SENSORS] ALERTA: sensor no pino ");
      Serial.print(s->pin);
      Serial.println(" parece travado/obstruido (acionado continuamente).");
    }
  } else {
    s->activeSinceUs = 0;
    s->stuckFlag = false;
  }
}

void sensorsUpdate() {
  checkStuck(&s1);
  checkStuck(&s2);
}

bool sensor1Consume(int64_t &timestampUs) {
  if (!s1.pendingFlag) return false;
  noInterrupts();
  timestampUs = s1.pendingEventUs;
  s1.pendingFlag = false;
  interrupts();
  return true;
}

bool sensor2Consume(int64_t &timestampUs) {
  if (!s2.pendingFlag) return false;
  noInterrupts();
  timestampUs = s2.pendingEventUs;
  s2.pendingFlag = false;
  interrupts();
  return true;
}

bool sensor1IsStuck() { return s1.stuckFlag; }
bool sensor2IsStuck() { return s2.stuckFlag; }

void sensor1Flush() {
  noInterrupts();
  s1.pendingFlag = false;
  interrupts();
}

void sensor2Flush() {
  noInterrupts();
  s2.pendingFlag = false;
  interrupts();
}
