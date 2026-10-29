// Estado das peças "de mentira" (ver os .h desta pasta).
#include "Arduino.h"
#include "LittleFS.h"
#include "Preferences.h"
#include "esp_timer.h"

FakeSerial Serial;
FakeEsp ESP;
FakeLittleFS LittleFS;

std::map<std::string, std::string>& fakeFiles() { static std::map<std::string, std::string> f; return f; }
std::set<std::string>& fakeDirs() { static std::set<std::string> d; return d; }
std::map<std::string, int>& fakePrefsInts() { static std::map<std::string, int> p; return p; }
int64_t& fakeTimeUs() { static int64_t t = 1000000; return t; }
unsigned long millis() { return (unsigned long)(fakeTimeUs() / 1000); }
