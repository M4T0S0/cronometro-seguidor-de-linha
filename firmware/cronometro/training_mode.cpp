#include "training_mode.h"
#include "config.h"
#include "state_machine.h"
#include "race_control.h"
#include "sensors.h"
#include "timer.h"
#include "display.h"
#include "peripherals.h"
#include "idle_screen.h"
#include <Arduino.h>

bool trainingModeActive = false;

// Sem tempo de exibição: o resultado fica no display até o robô passar
// de novo pelo sensor 1, que já começa a próxima volta.
enum class TrainingState { WAITING, RUNNING };
static TrainingState tState = TrainingState::WAITING;

void trainingModeInit() {
  pinMode(PIN_MODE_SELECT, INPUT_PULLUP);
}

static void enterTraining() {
  trainingModeActive = true;
  tState = TrainingState::WAITING;
  ledSetGreen(false);
  ledSetRed(false);
  displayShowTrainingIdle();
  Serial.println("[MODO] TREINO ativo.");
}

static void exitTraining() {
  trainingModeActive = false;
  ledSetRed(false); // o vermelho do último resultado de treino fica aceso até aqui
  transitionTo(SystemState::WAITING_START);
  idleScreenInvalidate(); // o loop() principal redesenha a tela certa (ver idle_screen.h)
  Serial.println("[MODO] COMPETICAO ativo.");
}

bool trainingSwitchOn() { return digitalRead(PIN_MODE_SELECT) == LOW; }

void trainingModePoll() {
  bool wantTraining = trainingSwitchOn();
  if (wantTraining == trainingModeActive) return; // chave não mudou

  if (wantTraining && currentState == SystemState::WAITING_START && !raceControlIsArmed()) {
    enterTraining();
  } else if (!wantTraining && tState == TrainingState::WAITING) {
    exitTraining();
  }
  // troca pedida no meio de uma corrida (competição ou treino) fica
  // pendente e é aplicada assim que o sistema voltar pro estado ocioso
}

void trainingLoop() {
  sensorsUpdate();
  peripheralsUpdate();
  int64_t ts;

  switch (tState) {
    case TrainingState::WAITING:
      sensor2Flush();
      if (sensor1Consume(ts)) {
        timerStart();
        buzzerBeep();
        ledSetGreen(true);
        ledSetRed(false);
        tState = TrainingState::RUNNING;
      }
      break;

    case TrainingState::RUNNING:
      displayShowRunning(timerElapsedUs(), nullptr, nullptr);
      if (sensor2Consume(ts)) {
        timerStop();
        buzzerBeep();
        ledSetGreen(false);
        ledSetRed(true);
        displayShowFinished(timerElapsedUs(), nullptr, nullptr);
        sensor1Flush(); // descarta passagem antiga; só conta a próxima
        tState = TrainingState::WAITING;
      }
      break;
  }
}