// LittleFS "de mentira": os arquivos ficam num mapa na memória do
// computador. Os testes podem olhar e mexer neles (fakeFiles).
#pragma once
#include <map>
#include <set>
#include <string>
#include "Arduino.h"

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

std::map<std::string, std::string>& fakeFiles();
std::set<std::string>& fakeDirs();

class File {
 public:
  File() {}
  File(const std::string& path, bool writable) : path_(path), ok_(true), writable_(writable) {}
  explicit operator bool() const { return ok_; }

  int available() { return ok_ ? (int)(data().size() - pos_) : 0; }
  String readStringUntil(char end) {
    std::string& d = data();
    size_t e = d.find(end, pos_);
    std::string out = d.substr(pos_, e == std::string::npos ? std::string::npos : e - pos_);
    pos_ = (e == std::string::npos) ? d.size() : e + 1;
    return String(out);
  }
  size_t size() { return ok_ ? data().size() : 0; }

  template <class T> size_t print(const T& v) { return put(text(v)); }
  size_t println() { return put("\r\n"); }
  template <class T> size_t println(const T& v) { return put(text(v) + "\r\n"); }
  size_t printf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return put(buf);
  }
  void flush() {}
  void close() { ok_ = false; }

 private:
  std::string& data() { return fakeFiles()[path_]; }
  size_t put(const std::string& s) {
    if (!ok_ || !writable_) return 0;
    data() += s;
    return s.size();
  }
  static std::string text(const char* v) { return v ? v : ""; }
  static std::string text(char v) { return std::string(1, v); }
  static std::string text(const String& v) { return v.c_str(); }
  static std::string text(int v) { return std::to_string(v); }
  static std::string text(unsigned v) { return std::to_string(v); }
  static std::string text(long v) { return std::to_string(v); }
  static std::string text(unsigned long v) { return std::to_string(v); }
  static std::string text(long long v) { return std::to_string(v); }
  static std::string text(unsigned long long v) { return std::to_string(v); }

  std::string path_;
  size_t pos_ = 0;
  bool ok_ = false;
  bool writable_ = false;
};

struct FakeLittleFS {
  bool begin(bool = false) { return true; }
  bool exists(const char* p) { return fakeFiles().count(p) || fakeDirs().count(p); }
  bool exists(const String& p) { return exists(p.c_str()); }
  bool mkdir(const char* p) { fakeDirs().insert(p); return true; }
  bool mkdir(const String& p) { return mkdir(p.c_str()); }
  bool remove(const char* p) { return fakeFiles().erase(p) > 0; }
  bool remove(const String& p) { return remove(p.c_str()); }
  bool rename(const char* a, const char* b) {
    auto it = fakeFiles().find(a);
    if (it == fakeFiles().end()) return false;
    fakeFiles()[b] = it->second;
    fakeFiles().erase(a);
    return true;
  }
  bool rename(const String& a, const String& b) { return rename(a.c_str(), b.c_str()); }
  File open(const char* p, const char* mode = FILE_READ) {
    std::string m = mode;
    if (m == "r") return fakeFiles().count(p) ? File(p, false) : File();
    if (m == "w") fakeFiles()[p] = "";
    else fakeFiles()[p];  // "a": cria se não existir, mantém o conteúdo
    return File(p, true);
  }
  File open(const String& p, const char* mode = FILE_READ) { return open(p.c_str(), mode); }
};
extern FakeLittleFS LittleFS;
