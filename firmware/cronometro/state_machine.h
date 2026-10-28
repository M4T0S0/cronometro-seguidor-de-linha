#pragma once

// =====================================================================
// STATE_MACHINE.H
// Conjunto completo de estados do sistema. SYNCING existiu
// enquanto o resultado precisava de uma tentativa de envio HTTP separada
// da gravação (versão antiga, com servidor externo) — hoje a gravação
// na flash já É definitiva, então esse estado foi removido.
// =====================================================================

enum class SystemState {
  BOOT,
  INIT_HARDWARE,
  INIT_NETWORK,
  IDLE,
  ARMED,
  WAITING_START,
  RUNNING,
  FINISHED,
  SAVING,
  ERROR
};

// Estado atual do sistema. 'volatile' porque pode ser lido/observado
// em contextos que futuramente incluirão ISRs ou tasks concorrentes.
extern volatile SystemState currentState;

// Converte o estado para string legível (debug / OLED / logs).
const char* stateToString(SystemState state);

// Realiza a transição de estado, com log da transição.
// Centralizar aqui evita mudanças de estado "soltas" espalhadas pelo código.
void transitionTo(SystemState newState);
