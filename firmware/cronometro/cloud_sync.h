#pragma once
#include <stdint.h>
#include <stddef.h>
#include "storage.h"

// =====================================================================
// CLOUD_SYNC.H — Espelhamento opcional dos resultados para o Módulo
// Nuvem
//
// A flash continua sendo a ÚNICA fonte de verdade (ver storage.h) — isto
// aqui só tenta mandar uma CÓPIA de cada resultado já salvo para a
// nuvem, quando o modo "Online" está habilitado e configurado. Qualquer
// falha (Wi-Fi fora, nuvem fora do ar, timeout) só é logada — o
// resultado já está seguro na flash de qualquer forma (falha segura).
//
// Sem task/núcleo dedicado: como o Objetivo 2 (segundo núcleo) está
// pausado, a tentativa de
// sincronização roda dentro do próprio loop() principal, no ÚNICO
// instante em que isso é seguro — uma única vez, logo após salvar o
// resultado na flash, durante a janela de exibição do resultado final
// (RESULT_DISPLAY_HOLD_US, ver o .ino), quando nenhuma corrida nova
// pode começar (raceReleased só volta a true depois que o juiz arma e
// libera a próxima). O timeout HTTP é propositalmente curto para nunca
// segurar essa janela por muito mais tempo do que ela já dura sozinha.
//
// Sem fila de reenvio nesta primeira versão: se a tentativa falhar
// (ex.: internet caiu bem naquele instante), aquele resultado
// específico só fica na flash e não é reenviado automaticamente depois —
//, pendência formal para uma v2.
// =====================================================================

struct CloudSyncConfig {
  // Sem checkbox "habilitar" separado — o modo Online
  // fica ativo sempre que baseUrl E deviceToken estiverem preenchidos;
  // Offline é simplesmente deixar os dois em branco. Ver configIsUsable()
  // em cloud_sync.cpp.
  char baseUrl[80];        // ex.: "http://192.168.15.23:8001" (sem barra no final)
  // Token único do dispositivo — o mesmo ADMIN_TOKEN da nuvem,
  // reaproveitado como "senha" que autoriza este ESP32 a sincronizar.
  // Substitui o par Competition ID + Access Token por competição da
  // Agora QUALQUER competição local sincroniza automaticamente
  // (a nuvem cria/encontra a competição correspondente pelo slug, ver
  // cloudSyncTryNow) — sem precisar configurar nada por competição.
  char deviceToken[64];
};

// Carrega a configuração salva na NVS (Preferences) — chamar uma vez no
// setup(), depois de storageInit()/dataStoreInit() (não depende deles,
// mas segue a mesma convenção de inicializar módulos de dados no boot).
void cloudSyncInit();

CloudSyncConfig cloudSyncGetConfig();

// Salva a nova configuração (NVS) — chamado pelo handler HTTP local
// POST /api/v1/cloud-config (tela de Configurações da página do juiz).
void cloudSyncSetConfig(const CloudSyncConfig &cfg);

// Confirma que o token do dispositivo é aceito pela nuvem, chamando
// GET /api/v1/device/ping (timeout curto). Só é chamado sob demanda
// (botão "Testar conexão" da página local) — nunca automaticamente.
// Preenche outMsg com o motivo em caso de falha.
bool cloudSyncTestAuth(char* outMsg, size_t outMsgSize);

// Tenta sincronizar UM resultado agora (bloqueante, timeout curto) — só
// deve ser chamado no instante seguro descrito acima. No-op silencioso
// se o modo Online não estiver habilitado/configurado, o Wi-Fi (STA)
// não estiver conectado, ou a corrida não tiver contexto resolvido.
void cloudSyncTryNow(const RunRecord &record, const RunContext &ctx);

// Avisa a nuvem que o status de UMA competição local mudou (draft/
// finished), sem nenhuma corrida envolvida — usado em "avançar dia"/
// "encerrar competição" (ver handlePostAdvanceDay em web_server.cpp).
// Sem isto, uma competição que termina sem mais nenhuma corrida depois
// nunca teria outra chance de avisar a nuvem (cloudSyncTryNow só roda
// atrelado a uma corrida) — o dashboard ficaria "Em andamento" pra
// sempre mesmo já encerrada localmente. Mesmas regras de no-op
// silencioso de cloudSyncTryNow (offline, sem Wi-Fi etc.).
void cloudSyncCompetitionStatus(int competitionId);

// Fila de reenvio — corridas que não chegaram na nuvem (sem
// Wi-Fi, nuvem fora do ar) ficam anotadas num arquivo na flash de DADOS
// (/nuvem_reenvio.txt: competição, dia e event_id - ~30 bytes cada),
// sobrevivendo a reboot. Na hora de enviar, a corrida é lida de novo do
// resultados.jsonl do dia: vai sempre o status mais recente (validada ou
// não), e uma corrida apagada nesse meio tempo simplesmente sai da fila.
// Com o modo Online desligado nada é anotado.
//
// Envio: cloudSyncPoll() (no loop, ocioso) manda uma por vez em segundo
// plano assim que houver internet; cloudSyncFlushPending() (janela depois
// de uma corrida) manda algumas de uma vez.
void cloudSyncFlushPending();

// Chamar a cada iteração do loop() quando o cronômetro estiver ocioso
// (sem robô liberado nem corrida em andamento): a cada poucos segundos
// manda UMA corrida pendente (timeout curto) e, de vez em quando, as
// renomeações/exclusões pendentes - não segura o loop por muito tempo.
void cloudSyncPoll();

// ------------------- Robô renomeado -------------------
// A nuvem guarda o NOME do robô em cada corrida (não tem cadastro de
// robôs - ver cloud/api/database.py). Ao renomear no ESP32, as corridas
// já sincronizadas precisam ser corrigidas lá, apontadas pelo event_id
// (POST /api/v1/sync/participant-name, uma chamada por competição).
// Se não der agora (offline, sem Wi-Fi, nuvem fora), o robô fica numa
// lista de pendências gravada na NVS - sobrevive a reboot - e é tentado
// de novo em cloudSyncFlushRenames().
enum CloudRenameResult {
  CLOUD_RENAME_OFF = 0,     // modo Online não configurado - nada a fazer
  CLOUD_RENAME_SYNCED = 1,  // nuvem já atualizada
  CLOUD_RENAME_PENDING = 2, // guardado para tentar depois
};
#define MAX_CLOUD_RENAME_PENDING 16
CloudRenameResult cloudSyncParticipantRenamed(int participantId);
// Tenta mandar as renomeações pendentes (bloqueante, timeout curto por
// competição). Chamada junto de cloudSyncFlushPending() (janela pós-corrida),
// ao salvar/testar a config da nuvem e ao avançar o dia.
void cloudSyncFlushRenames();

// ------------------- Robô excluído -------------------
// Excluir um robô no ESP32 apaga as corridas dele (participantPurge).
// Para apagar também na nuvem, cada corrida apagada passa por
// cloudSyncQueueRunDeletion() - que anota competição + event_id num
// arquivo na flash (/nuvem_exclusoes.txt, sobrevive a reboot) - e
// cloudSyncFlushDeletes() manda essas exclusões (POST
// /api/v1/sync/runs-delete, uma chamada por competição). No-op com o modo
// Online desligado.
void cloudSyncQueueRunDeletion(const char* competitionSlug, const char* eventId);
// Tenta mandar as exclusões pendentes. Retorna CLOUD_RENAME_SYNCED se não
// sobrou nada pendente, CLOUD_RENAME_PENDING se sobrou, CLOUD_RENAME_OFF
// com o modo Online desligado (mesmos valores do aviso de renomeação).
CloudRenameResult cloudSyncFlushDeletes();
