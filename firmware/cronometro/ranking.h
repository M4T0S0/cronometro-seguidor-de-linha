#pragma once
#include <stdint.h>

#define MAX_RANKING_ROWS 40
#define MAX_ATTEMPTS_PER_ROW 10

struct AttemptCell {
  int32_t elapsedUs;
  uint8_t day;
  uint8_t attempt;
  char statusCode;  // 'V' valid | 'I' invalid | 'D' dnf | 'P' pending_validation
  bool isBest;       // melhor tomada VÁLIDA deste participante
  bool present;      // false = esta tomada ainda não aconteceu (traço na tela)
};

struct RankingRow {
  int position;
  int participantId;
  char participantName[40];
  char externalCode[24];
  int32_t bestElapsedUs; // -1 = nenhuma tomada valida ainda
  int attemptCount;       // total de slots da competicao (dias x tomadas/dia)
  AttemptCell attempts[MAX_ATTEMPTS_PER_ROW];
};

int computeRanking(int competitionId, RankingRow* outRows, int maxRows);