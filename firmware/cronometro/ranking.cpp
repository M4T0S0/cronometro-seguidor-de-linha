#include "ranking.h"
#include "config.h"
#include "data_store.h"
#include "storage.h"
#include <string.h>

static char statusToCode(const char* status) {
  if (strcmp(status, "valid") == 0) return 'V';
  if (strcmp(status, "invalid") == 0) return 'I';
  if (strcmp(status, "dnf") == 0) return 'D';
  if (strcmp(status, "pending_validation") == 0) return 'P';
  return '?';
}

int computeRanking(int competitionId, RankingRow* outRows, int maxRows) {
  const Competition* comp = competitionById(competitionId);
  if (!comp) return 0;

  int totalSlots = comp->numDays * comp->attemptsPerDay;
  if (totalSlots > MAX_ATTEMPTS_PER_ROW) totalSlots = MAX_ATTEMPTS_PER_ROW;

  int rowCount = 0;

  StoredRun* dayRuns = storageDayRunsScratch();
  for (int day = 1; day <= comp->numDays; day++) {
    int n = storageReadDayRuns(comp->slug, day, dayRuns, MAX_RUNS_PER_DAY);
    for (int i = 0; i < n; i++) {
      int pid = dayRuns[i].participantId;

      int idx = -1;
      for (int j = 0; j < rowCount; j++) {
        if (outRows[j].participantId == pid) { idx = j; break; }
      }
      if (idx < 0) {
        if (rowCount >= maxRows) continue;
        idx = rowCount++;
        const Participant* p = participantById(pid);
        RankingRow &row = outRows[idx];
        row.position = 0;
        row.participantId = pid;
        strlcpy(row.participantName, p ? p->name : "?", sizeof(row.participantName));
        strlcpy(row.externalCode, p ? p->externalCode : "", sizeof(row.externalCode));
        row.bestElapsedUs = -1;
        row.attemptCount = totalSlots;
        for (int s = 0; s < totalSlots; s++) row.attempts[s].present = false;
      }

      RankingRow &row = outRows[idx];
      int slot = (dayRuns[i].day - 1) * comp->attemptsPerDay + (dayRuns[i].attempt - 1);
      if (slot >= 0 && slot < totalSlots) {
        AttemptCell &cell = row.attempts[slot];
        char code = statusToCode(dayRuns[i].status);
        if (code == '?') {
          // "retried" (ou outro status desconhecido) - trata como se a
          // tomada ainda não tivesse acontecido, até o redo chegar.
          cell.present = false;
        } else {
          cell.elapsedUs = (int32_t)dayRuns[i].elapsedUs;
          cell.day = (uint8_t)dayRuns[i].day;
          cell.attempt = (uint8_t)dayRuns[i].attempt;
          cell.statusCode = code;
          cell.isBest = false;
          cell.present = true;
        }
      }

      if (strcmp(dayRuns[i].status, "valid") == 0) {
        if (row.bestElapsedUs < 0 || (int32_t)dayRuns[i].elapsedUs < row.bestElapsedUs) {
          row.bestElapsedUs = (int32_t)dayRuns[i].elapsedUs;
        }
      }
    }
  }

  // Melhor tomada de CADA participante (não só o recorde da competição).
  for (int i = 0; i < rowCount; i++) {
    if (outRows[i].bestElapsedUs < 0) continue;
    for (int j = 0; j < outRows[i].attemptCount; j++) {
      AttemptCell &cell = outRows[i].attempts[j];
      if (cell.present && cell.statusCode == 'V' && cell.elapsedUs == outRows[i].bestElapsedUs) {
        cell.isBest = true;
        break;
      }
    }
  }

  for (int i = 0; i < rowCount - 1; i++) {
    int bestIdx = i;
    for (int j = i + 1; j < rowCount; j++) {
      bool jHas = outRows[j].bestElapsedUs >= 0;
      bool bHas = outRows[bestIdx].bestElapsedUs >= 0;
      if (jHas && (!bHas || outRows[j].bestElapsedUs < outRows[bestIdx].bestElapsedUs)) {
        bestIdx = j;
      }
    }
    if (bestIdx != i) {
      RankingRow tmp = outRows[i];
      outRows[i] = outRows[bestIdx];
      outRows[bestIdx] = tmp;
    }
  }

  for (int i = 0; i < rowCount; i++) outRows[i].position = i + 1;
  return rowCount;
}