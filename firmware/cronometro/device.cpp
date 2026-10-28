#include "device.h"
#include <Arduino.h>

static uint64_t chipIdCache = 0;
static bool cached = false;

void deviceGetShortId(char* buffer, size_t bufferSize) {
  if (!cached) {
    chipIdCache = ESP.getEfuseMac();
    cached = true;
  }
  snprintf(buffer, bufferSize, "%06X", (unsigned int)(chipIdCache & 0xFFFFFFULL));
}
