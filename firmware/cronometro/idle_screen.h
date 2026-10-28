#pragma once
#include <stdint.h>

// =====================================================================
// IDLE_SCREEN.H — O que o display mostra FORA de uma corrida
//
// Fora de RUNNING/FINISHED (que o .ino desenha direto), o display mostra
// sempre a informação mais importante do momento, nesta prioridade:
//   1. Conectando ao Wi-Fi (1a tentativa: boot ou troca de rede na ⚙️)
//   2. Alerta: sensor bloqueado ou memória com erro
//   3. Robô armado: "aguardando liberação" / "liberado - pode ir"
//   4. Corrida terminada aguardando validação do juiz
//   5. O que o juiz está fazendo na página (contexto enviado por ela)
//   6. Como acessar o painel - sem limite de tempo: fica até alguém abrir
//      a página, e volta quando a página some por mais de
//      JUDGE_CONTEXT_TIMEOUT_US (celular bloqueado etc.). Muda por evento
//     : ninguém no Wi-Fi do cronômetro -> QR do Wi-Fi com rede e
//      senha ao lado; alguém entrou -> QR do painel com o endereço ao lado.
//
// Só redesenha quando o conteúdo muda (o I2C do OLED não é de graça).
// =====================================================================

// Chamar a cada iteração do loop() enquanto WAITING_START (throttled).
void idleScreenUpdate();

// Força redesenhar na próxima chamada (ex.: voltando do Modo Treino ou
// de uma corrida, quando o display mostrava outra coisa).
void idleScreenInvalidate();

// A página informa em que tela o juiz está (POST /api/v1/display-context).
// view: "home", "roster", "config", "competitions", "history", "new",
// "inscritos", "add", "competition"; competitionId <= 0 se não se aplica;
// phase (só em "competition"): "select", "day_complete", "finished"...
void idleScreenSetJudgeContext(const char* view, int competitionId, const char* phase);

// Corrida terminou com status pendente -> mostrar "aguardando validação"
// até o juiz dar o veredito (idleScreenValidationResolved).
void idleScreenSetValidationPending(const char* robotName, const char* teamName, int64_t elapsedUs);
void idleScreenValidationResolved();
