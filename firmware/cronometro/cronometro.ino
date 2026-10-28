// =====================================================================
// CronometroCompeticao.ino — versão standalone (sem servidor externo)
//
// O ESP32 hospeda a própria API + página do juiz (ver web_server.h) —
// não há mais um servidor Python/SQLite remoto. "Armar/liberar/abortar"
// deixou de ser um polling HTTP (ver race_control.h): webServerHandle()
// roda dentro deste mesmo loop() (Core 1, mesmo núcleo da gravação), então não
// existe mais concorrência entre núcleos — as funções raceControlXxx()
// são chamadas tanto pelos handlers HTTP quanto pela máquina de estados
// abaixo, sem mutex (só existe uma linha de execução tocando esse estado).
//
// Participantes/competições/vínculos são o "banco de dados" local (ver
// data_store.h, CSV na flash interna) — carregados
// de volta no boot, o que é exatamente como uma competição em andamento
// sobrevive a um reboot.
//
// Falha segura preservada: qualquer corrida que termine sem contexto
// armado válido (não deveria acontecer, mas ver defesa em storage.cpp)
// ainda é salva, só cai no arquivo plano de fallback.
// =====================================================================

#include "config.h"
#include "state_machine.h"
#include "timer.h"
#include "sensors.h"
#include "display.h"
#include "storage.h"
#include "wifi.h"
#include "data_store.h"
#include "race_control.h"
#include "web_server.h"
#include "cloud_sync.h"
#include "training_mode.h"
#include "peripherals.h"
#include "idle_screen.h"
#include <string.h>

static const char* PARTICIPANT_PLACEHOLDER = "----";

static int64_t lastStartTimestampUs = 0;
static int64_t lastStopTimestampUs = 0;
static const char* currentRunStatus = "pending_validation";

// Contexto legível da corrida ATUAL — capturado uma única vez no
// instante do START (agora resolvido localmente via data_store, não
// mais por uma chamada de rede) e mantido estável durante toda a
// corrida, mesmo que o participante seja editado no meio do caminho.
static RunContext raceCtx = { "", "", -1, -1, -1, -1, false };
static char raceCtxTeamName[40] = "";

// Fora de uma corrida, o display é decidido por idle_screen:
// conectando, alertas, robô armado, validação pendente, o que o juiz está
// fazendo na página, ou como acessar o painel (sem limite de tempo).

// "Dia 1 - Tomada 2" da corrida atual, para o rodapé do display.
static char raceFooter[24] = "";

static void printElapsed(int64_t elapsedUs) {
  char timeStr[16];
  formatElapsedTime(elapsedUs, timeStr, sizeof(timeStr));

  Serial.print("[RESULT] Tempo: ");
  Serial.print(timeStr);
  Serial.print("  (");
  Serial.print((long)elapsedUs);
  Serial.println(" us)");
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);

  Serial.println();
  Serial.println("=== Sistema de Cronometragem — versao standalone ===");

  trainingModeInit();

  transitionTo(SystemState::BOOT);
  sensorsInit();
  displayInit();
  peripheralsInit();
  storageInit();
  dataStoreInit();
  raceControlInit();

  transitionTo(SystemState::INIT_NETWORK);
  wifiInit();
  webServerInit();
  cloudSyncInit();

  transitionTo(SystemState::IDLE);
  transitionTo(SystemState::WAITING_START);
}

void loop() {
  trainingModePoll();
  if (trainingModeActive) {
    trainingLoop();
    return;
  }

  sensorsUpdate(); // precisa rodar sempre, detecta sensor travado
  peripheralsUpdate(); // desliga o buzzer sozinho quando o bipe expira
  wifiUpdate();     // throttled internamente — (re)conecta a rede externa
  webServerHandle(); // processa no máximo uma requisição HTTP pendente

  int64_t ts;

  switch (currentState) {

    case SystemState::WAITING_START: {
      idleScreenUpdate();

      if (!raceControlIsReleased()) {
        // Corrida travada: nenhum START deve ser aceito. Descarta
        // continuamente qualquer passagem pelo Sensor 1 nesta janela.
        sensor1Flush();
        // Ocioso: aproveita pra mandar à nuvem o que ficou pendente, uma
        // corrida por vez (ver cloud_sync.h). Nunca com robô liberado.
        cloudSyncPoll();
        break;
      }

      if (sensor1Consume(ts)) {
        lastStartTimestampUs = ts;
        timerStart();
        buzzerBeep();
        ledSetGreen(true);
        ledSetRed(false);
        currentRunStatus = "pending_validation";
        sensor2Flush(); // descarta qualquer STOP obsoleto anterior ao START

        // Captura o contexto UMA VEZ para esta corrida, resolvendo os
        // ids armados (race_control) para nome/slug (data_store) — tudo
        // local, sem round-trip de rede.
        raceCtx.competitionId = raceControlCompetitionId();
        raceCtx.participantId = raceControlParticipantId();
        raceCtx.day = raceControlDay();
        raceCtx.attempt = raceControlAttempt();
        const Competition* comp = competitionById(raceCtx.competitionId);
        const Participant* part = participantById(raceCtx.participantId);
        strlcpy(raceCtx.competitionSlug, comp ? comp->slug : "", sizeof(raceCtx.competitionSlug));
        strlcpy(raceCtx.participantName, part ? part->name : "", sizeof(raceCtx.participantName));
        raceCtx.hasContext = (comp != nullptr && part != nullptr);

        const Team* team = part ? teamById(part->teamId) : nullptr;
        strlcpy(raceCtxTeamName, team ? team->name : "", sizeof(raceCtxTeamName));
        if (raceCtx.day > 0 && raceCtx.attempt > 0) snprintf(raceFooter, sizeof(raceFooter), "Dia %d - Tomada %d", raceCtx.day, raceCtx.attempt);
        else raceFooter[0] = '\0';

        raceControlMarkStarted(); // diferencia "liberado, aguardando" de "corrida em andamento" na pagina
        transitionTo(SystemState::RUNNING);
      }
      break;
    }

    case SystemState::RUNNING: {
      const char* robotName = raceCtx.hasContext ? raceCtx.participantName : PARTICIPANT_PLACEHOLDER;
      const char* teamName = raceCtx.hasContext ? raceCtxTeamName : "";
      displayShowRunning(timerElapsedUs(), robotName, teamName, raceFooter); // throttled internamente

      if (raceControlConsumeRetry()) {
        Serial.println("[ARM] Retentativa solicitada pelo juiz - refazendo a mesma tomada, sem gravar resultado.");
        ledSetGreen(false); // corrida interrompida no meio do percurso
        int cid = raceCtx.competitionId, pid = raceCtx.participantId, day = raceCtx.day, attempt = raceCtx.attempt;
        raceControlDisarm();
        raceControlArm(cid, pid, day, attempt);
        transitionTo(SystemState::WAITING_START);
        break;
      }

      if (raceControlConsumeAbort()) {
        Serial.println("[ARM] Aborto solicitado pelo juiz - encerrando tomada como DNF.");
        ledSetGreen(false); // corrida interrompida no meio do percurso
        lastStopTimestampUs = nowUs();
        timerStop();
        currentRunStatus = "dnf"; // abortar = corredor nao completou o percurso = DNF, nao "invalid"
        transitionTo(SystemState::FINISHED);
        break;
      }

      if (sensor2Consume(ts)) {
        int64_t sinceStart = ts - lastStartTimestampUs;

        if (sinceStart < (int64_t)SENSOR_LOCKOUT_US) {
          Serial.print("[SENSORS] STOP ignorado (lockout): apenas ");
          Serial.print((long)sinceStart);
          Serial.println(" us apos o START.");
        } else {
          lastStopTimestampUs = ts; // timestamp bruto da ISR, não o re-sample do timerStop()
          timerStop();
          buzzerBeep();
          ledSetGreen(false);
          ledSetRed(true); // fica aceso até o juiz validar/invalidar/refazer (ver web_server.cpp)
          transitionTo(SystemState::FINISHED);
        }
      }
      break;
    }

    case SystemState::FINISHED: {
      static bool finishedActionsDone = false;
      static bool postSaveWorkDone = false;
      static int64_t finishedAtUs = 0;
      static RunRecord finishedRec; // guardado entre iterações - ver postSaveWorkDone abaixo

      if (!finishedActionsDone) {
        int64_t elapsedUs = lastStopTimestampUs - lastStartTimestampUs;
        const char* robotName = raceCtx.hasContext ? raceCtx.participantName : PARTICIPANT_PLACEHOLDER;
        const char* teamName = raceCtx.hasContext ? raceCtxTeamName : "";

        printElapsed(elapsedUs);
        bool isDnf = strcmp(currentRunStatus, "dnf") == 0;
        displayShowFinished(elapsedUs, robotName, teamName, isDnf ? "ABORTADA (DNF)" : "FIM");
        // Depois dos 3 s do resultado, o display fica em "aguardando
        // validação" até o juiz decidir na página (ver idle_screen.h).
        if (!isDnf && raceCtx.hasContext) idleScreenSetValidationPending(robotName, teamName, elapsedUs);

        RunRecord &rec = finishedRec;
        storageGenerateEventId(lastStartTimestampUs, rec.eventId, sizeof(rec.eventId));
        rec.competitionId = raceCtx.hasContext ? raceCtx.competitionId : -1;
        rec.participantId = raceCtx.hasContext ? raceCtx.participantId : -1;
        rec.day = raceCtx.hasContext ? raceCtx.day : -1;
        rec.attempt = raceCtx.hasContext ? raceCtx.attempt : -1;
        rec.startTimestampUs = lastStartTimestampUs;
        rec.endTimestampUs = lastStopTimestampUs;
        rec.elapsedUs = elapsedUs;
        strncpy(rec.status, currentRunStatus, sizeof(rec.status) - 1);
        rec.status[sizeof(rec.status) - 1] = '\0';

        transitionTo(SystemState::SAVING);
        storageAppendRun(rec, raceCtx); // grava na flash - rápido, a corrida já fica visível pra validação a partir daqui
        transitionTo(SystemState::FINISHED);

        // Espelhar pra nuvem ainda é seguros de fazer
        // aqui (nenhuma corrida nova pode começar nesse meio tempo, ver
        // cloud_sync.h), mas são chamadas de rede/E/S que podem demorar
        // (cloudSyncTryNow até ~1,5s) — fazer isso ANTES de devolver o
        // controle ao loop() atrasava o WebServer local de responder ao
        // juiz, mesmo com o resultado já salvo e pronto pra validar.
        // Adiado pra próxima iteração (postSaveWorkDone abaixo): assim
        // pelo menos um webServerHandle() roda no meio, e a página do
        // juiz vê a corrida pendente antes da parte lenta começar.
        postSaveWorkDone = false;

        // A tomada armada foi consumida — desarma sempre (equivalente
        // local ao antigo DELETE system_state). Se o resultado já saiu
        // RESOLVIDO desta corrida (abortada -> dnf), avança a tomada
        // automaticamente; se ficou pendente de validação, quem avança
        // é o handler PATCH .../validation, depois que o juiz decidir
        // (ver web_server.cpp) — nunca antes disso.
        int cid = rec.competitionId, pid = rec.participantId, day = rec.day;
        raceControlDisarm();
        if (strcmp(currentRunStatus, "pending_validation") != 0 && cid > 0 && pid > 0 && day > 0) {
          maybeAdvanceAttempt(cid, pid, day);
        }

        finishedAtUs = nowUs();
        finishedActionsDone = true;

        // Devolve o controle ao loop() JÁ - sem este break, o bloco de
        // trabalho lento abaixo (postSaveWorkDone) executaria na MESMA
        // iteração, já que os dois "if" ficam dentro do mesmo case e
        // nada os separa. Só assim webServerHandle() roda pelo menos
        // uma vez com o resultado já salvo antes da parte lenta começar
        // (era esse o ponto inteiro da separação em duas etapas).
        break;
      }

      if (!postSaveWorkDone) {
        cloudSyncTryNow(finishedRec, raceCtx);
        // Tenta persistir na flash qualquer resultado anterior que tenha
        // ficado pendente - custo desprezível quando a fila
        // está vazia.
        storageFlushPending();
        cloudSyncFlushPending();
        postSaveWorkDone = true;
      }

      sensor1Flush();

      if (nowUs() - finishedAtUs >= (int64_t)RESULT_DISPLAY_HOLD_US) {
        finishedActionsDone = false;
        raceCtx.hasContext = false;
        transitionTo(SystemState::WAITING_START);
        idleScreenInvalidate();
      }
      break;
    }

    default:
      break;
  }
}
