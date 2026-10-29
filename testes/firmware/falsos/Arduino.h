// Arduino "de mentira" para compilar a lógica do firmware no computador.
// Só tem o que o firmware usa: String, Serial (sem saída), millis e ESP.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define PROGMEM
#define F(x) (x)

class String {
 public:
  String() {}
  String(const char* c) : s_(c ? c : "") {}
  String(const std::string& v) : s_(v) {}
  String(char c) : s_(1, c) {}
  String(int v) : s_(std::to_string(v)) {}
  String(unsigned v) : s_(std::to_string(v)) {}
  String(long v) : s_(std::to_string(v)) {}
  String(unsigned long v) : s_(std::to_string(v)) {}
  String(long long v) : s_(std::to_string(v)) {}

  unsigned length() const { return (unsigned)s_.size(); }
  const char* c_str() const { return s_.c_str(); }
  char operator[](unsigned i) const { return i < s_.size() ? s_[i] : '\0'; }
  char charAt(unsigned i) const { return (*this)[i]; }

  int indexOf(char c, unsigned from = 0) const { return pos(s_.find(c, from)); }
  int indexOf(const String& t, unsigned from = 0) const { return pos(s_.find(t.s_, from)); }
  int indexOf(const char* t, unsigned from = 0) const { return pos(s_.find(t, from)); }
  String substring(unsigned b) const { return b >= s_.size() ? String() : String(s_.substr(b)); }
  String substring(unsigned b, unsigned e) const {
    if (b > e) std::swap(b, e);
    if (b >= s_.size()) return String();
    return String(s_.substr(b, e - b));
  }
  bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
  void trim() {
    size_t a = s_.find_first_not_of(" \t\r\n");
    size_t z = s_.find_last_not_of(" \t\r\n");
    s_ = (a == std::string::npos) ? "" : s_.substr(a, z - a + 1);
  }
  long toInt() const { return std::strtol(s_.c_str(), nullptr, 10); }
  void toCharArray(char* buf, unsigned size) const {
    if (!size) return;
    std::strncpy(buf, s_.c_str(), size - 1);
    buf[size - 1] = '\0';
  }

  String& operator+=(const String& o) { s_ += o.s_; return *this; }
  String& operator+=(const char* o) { s_ += (o ? o : ""); return *this; }
  String& operator+=(char c) { s_ += c; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }
  friend String operator+(const String& a, const char* b) { return String(a.s_ + (b ? b : "")); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a ? a : "") + b.s_); }
  friend String operator+(const String& a, char b) { return String(a.s_ + b); }
  bool operator==(const String& o) const { return s_ == o.s_; }
  bool operator==(const char* o) const { return s_ == (o ? o : ""); }
  bool operator!=(const String& o) const { return !(*this == o); }
  bool operator!=(const char* o) const { return !(*this == o); }

 private:
  static int pos(size_t p) { return p == std::string::npos ? -1 : (int)p; }
  std::string s_;
};

// Serial: aceita tudo e não imprime nada (os testes mostram só o resultado).
struct FakeSerial {
  template <class T> size_t print(const T&) { return 0; }
  template <class T> size_t print(const T&, int) { return 0; }
  size_t println() { return 0; }
  template <class T> size_t println(const T&) { return 0; }
  template <class T> size_t println(const T&, int) { return 0; }
  size_t printf(const char*, ...) { return 0; }
};
extern FakeSerial Serial;

struct FakeEsp {
  uint64_t getEfuseMac() { return 0xA1B2C3D4E5F6ULL; }
};
extern FakeEsp ESP;

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
unsigned long millis();
inline void delay(unsigned long) {}
