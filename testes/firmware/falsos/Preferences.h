// Preferences (NVS) "de mentira": um mapa na memória, por namespace.
#pragma once
#include <map>
#include <string>

std::map<std::string, int>& fakePrefsInts();

class Preferences {
 public:
  bool begin(const char* ns, bool = false) { ns_ = ns; return true; }
  void end() {}
  int getInt(const char* key, int def = 0) {
    auto it = fakePrefsInts().find(ns_ + "/" + key);
    return it == fakePrefsInts().end() ? def : it->second;
  }
  size_t putInt(const char* key, int v) { fakePrefsInts()[ns_ + "/" + key] = v; return sizeof(int); }

 private:
  std::string ns_;
};
