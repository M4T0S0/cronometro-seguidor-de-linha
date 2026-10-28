#include "state_machine.h"
#include <Arduino.h>

volatile SystemState currentState = SystemState::BOOT;

const char* stateToString(SystemState state) {
  switch (state) {
    case SystemState::BOOT:            return "BOOT";
    case SystemState::INIT_HARDWARE:   return "INIT_HARDWARE";
    case SystemState::INIT_NETWORK:    return "INIT_NETWORK";
    case SystemState::IDLE:            return "IDLE";
    case SystemState::ARMED:           return "ARMED";
    case SystemState::WAITING_START:   return "WAITING_START";
    case SystemState::RUNNING:         return "RUNNING";
    case SystemState::FINISHED:        return "FINISHED";
    case SystemState::SAVING:          return "SAVING";
    case SystemState::ERROR:           return "ERROR";
    default:                           return "UNKNOWN";
  }
}

void transitionTo(SystemState newState) {
  if (newState == currentState) return; // evita log/transição redundante

  Serial.print("[STATE] ");
  Serial.print(stateToString(currentState));
  Serial.print(" -> ");
  Serial.println(stateToString(newState));

  currentState = newState;
}
