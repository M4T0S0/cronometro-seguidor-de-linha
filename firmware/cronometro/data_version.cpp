#include "data_version.h"

static uint32_t g_version = 0;

void dataVersionBump() {
  g_version++;
}

uint32_t dataVersionCurrent() {
  return g_version;
}
