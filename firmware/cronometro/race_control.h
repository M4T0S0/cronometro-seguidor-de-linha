#pragma once
#include <stdint.h>

// =====================================================================
// RACE_CONTROL.H — Estado da corrida "armada" (versão standalone)
//
// Substitui shared_state.h. O nome antigo descrevia
// especificamente a sincronização entre a networkTask (Core 0, falando
// com o servidor externo) e o loop() principal (Core 1, dono da flash) —
// duas linhas de execução diferentes cochichando por mutex/semáforo.
//
// Sem servidor externo, não existe mais essa segunda linha de execução:
// o WebServer roda dentro do próprio loop() (ver web_server.cpp), então
// só existe UM lugar tocando este estado. Por isso este módulo não usa
// nenhuma primitiva de concorrência — é só o domínio "armar/liberar/
// abortar", sem nenhuma sincronização entre núcleos.
// =====================================================================

void raceControlInit();

bool raceControlIsArmed();
bool raceControlIsReleased();
bool raceControlIsStarted();

// Arma o sistema para um participante/dia/tomada específicos — trava o
// sensor (released=false) até raceControlRelease() ser chamado.
void raceControlArm(int competitionId, int participantId, int day, int attempt);

// Libera o sensor para a corrida atualmente armada (não-op se nada
// estiver armado).
void raceControlRelease();

// Marca a corrida armada como "realmente em andamento" — chamado pelo
// loop() principal no instante em que o Sensor 1 dispara de verdade
// (equivalente local ao antigo apiConfirmStart(), agora instantâneo,
// sem round-trip de rede).
void raceControlMarkStarted();

// Limpa completamente o estado armado (equivalente a DELETE /arm no
// servidor antigo) — chamado ao concluir uma corrida.
void raceControlDisarm();

// Pedido de aborto (vindo do handler HTTP POST /arm/abort). Só tem
// efeito depois de raceControlMarkStarted() ter sido chamado (mesma
// regra do servidor antigo: não dá pra abortar o que ainda não começou).
bool raceControlRequestAbort();

// Consome (lê e reseta) o pedido de aborto — uso único por ocorrência.
bool raceControlConsumeAbort();

// Pedido de retentar (mesma tomada) — mesma regra do abort, só tem
// efeito com a corrida em andamento (g_started).
bool raceControlRequestRetry();
bool raceControlConsumeRetry();

int raceControlCompetitionId();
int raceControlParticipantId();
int raceControlDay();
int raceControlAttempt();
