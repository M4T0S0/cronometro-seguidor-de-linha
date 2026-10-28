#include "race_control.h"

static bool g_armed = false;
static bool g_released = false;
static bool g_started = false;
static bool g_abortRequested = false;
static bool g_retryRequested = false;
static int g_competitionId = -1;
static int g_participantId = -1;
static int g_day = -1;
static int g_attempt = -1;

void raceControlInit() {
  g_armed = false;
  g_released = false;
  g_started = false;
  g_abortRequested = false;
}

bool raceControlIsArmed() { return g_armed; }
bool raceControlIsReleased() { return g_armed && g_released; }
bool raceControlIsStarted() { return g_armed && g_started; }

void raceControlArm(int competitionId, int participantId, int day, int attempt) {
  g_competitionId = competitionId;
  g_participantId = participantId;
  g_day = day;
  g_attempt = attempt;
  g_armed = true;
  g_released = false;
  g_started = false;
  g_abortRequested = false;
}

void raceControlRelease() {
  if (!g_armed) return;
  g_released = true;
}

void raceControlMarkStarted() {
  g_started = true;
}

void raceControlDisarm() {
  g_armed = false;
  g_released = false;
  g_started = false;
  g_abortRequested = false;
  g_retryRequested = false;
  g_competitionId = -1;
  g_participantId = -1;
  g_day = -1;
  g_attempt = -1;
}

bool raceControlRequestAbort() {
  if (!g_armed || !g_started) return false; // corrida ainda nao comecou - nada para abortar
  g_abortRequested = true;
  return true;
}

bool raceControlConsumeAbort() {
  if (!g_abortRequested) return false;
  g_abortRequested = false;
  return true;
}


bool raceControlRequestRetry() {
  if (!g_armed || !g_started) return false;
  g_retryRequested = true;
  return true;
}

bool raceControlConsumeRetry() {
  if (!g_retryRequested) return false;
  g_retryRequested = false;
  return true;
}

int raceControlCompetitionId() { return g_competitionId; }
int raceControlParticipantId() { return g_participantId; }
int raceControlDay() { return g_day; }
int raceControlAttempt() { return g_attempt; }
