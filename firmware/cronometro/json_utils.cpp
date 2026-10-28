#include "json_utils.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

// Extrai o valor de uma string JSON simples: "chave":"valor". Portado de
// api.cpp — a razão de existir continua a
// mesma: o corpo é sempre gerado por código nosso (agora local, não mais
// pelo servidor Python), então buscar a substring exata é seguro.
void extractJsonString(const String &body, const char* key, char* out, size_t outSize) {
  out[0] = '\0';
  String pattern = String("\"") + key + "\":\"";
  int idx = body.indexOf(pattern);
  if (idx < 0) return;

  int start = idx + pattern.length();
  int end = body.indexOf('"', start);
  if (end < 0) return;

  int len = end - start;
  if ((size_t)len >= outSize) len = outSize - 1;
  body.substring(start, start + len).toCharArray(out, len + 1);
}

int extractJsonInt(const String &body, const char* key, int defaultValue) {
  String pattern = String("\"") + key + "\":";
  int idx = body.indexOf(pattern);
  if (idx < 0) return defaultValue;

  int start = idx + pattern.length();
  int end = start;
  while (end < (int)body.length() && (isDigit(body[end]) || body[end] == '-')) end++;
  if (end == start) return defaultValue;

  return body.substring(start, end).toInt();
}

int64_t extractJsonInt64(const String &body, const char* key, int64_t defaultValue) {
  String pattern = String("\"") + key + "\":";
  int idx = body.indexOf(pattern);
  if (idx < 0) return defaultValue;

  int start = idx + pattern.length();
  int end = start;
  while (end < (int)body.length() && (isDigit(body[end]) || body[end] == '-')) end++;
  if (end == start) return defaultValue;

  char buf[24];
  int len = end - start;
  if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
  body.substring(start, start + len).toCharArray(buf, len + 1);
  return strtoll(buf, nullptr, 10);
}

// Mapeia o segundo byte de uma sequência UTF-8 de 2 bytes iniciada em
// 0xC3 (cobre U+00C0-U+00FF, os acentos latinos usados em PT-BR) para a
// letra ASCII mais próxima. Cobre os casos reais de nomes de competição/
// participante — não é uma normalização Unicode completa (isso viveria
// em unicodedata no Python original, inviável aqui).
static char latin1SupplementToAscii(uint8_t b2) {
  switch (b2) {
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: return 'A';
    case 0x87: return 'C';
    case 0x88: case 0x89: case 0x8A: case 0x8B: return 'E';
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: return 'I';
    case 0x91: return 'N';
    case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x98: return 'O';
    case 0x99: case 0x9A: case 0x9B: case 0x9C: return 'U';
    case 0x9D: return 'Y';
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: return 'a';
    case 0xA7: return 'c';
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: return 'e';
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: return 'i';
    case 0xB1: return 'n';
    case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB8: return 'o';
    case 0xB9: case 0xBA: case 0xBB: case 0xBC: return 'u';
    case 0xBD: case 0xBF: return 'y';
    default: return '_';
  }
}

void slugify(const char* text, char* out, size_t outSize) {
  if (!text || text[0] == '\0' || outSize == 0) {
    strlcpy(out, "competicao", outSize);
    return;
  }

  size_t outIdx = 0;
  size_t i = 0;
  bool lastWasUnderscore = true; // evita underscore inicial

  while (text[i] != '\0' && outIdx + 1 < outSize) {
    uint8_t b = (uint8_t)text[i];
    char emit;
    size_t advance = 1;

    if (b < 0x80) {
      emit = isalnum((int)b) ? (char)b : '_';
    } else if (b == 0xC3 && (uint8_t)text[i + 1] >= 0x80 && (uint8_t)text[i + 1] <= 0xBF) {
      emit = latin1SupplementToAscii((uint8_t)text[i + 1]);
      advance = 2;
    } else {
      // Outra sequência UTF-8 multibyte (ou byte de continuação órfão) —
      // vira separador; ainda assim precisamos pular os bytes certos
      // para não quebrar a sequência no meio.
      if ((b & 0xE0) == 0xC0) advance = 2;
      else if ((b & 0xF0) == 0xE0) advance = 3;
      else if ((b & 0xF8) == 0xF0) advance = 4;
      emit = '_';
    }

    if (emit == '_') {
      if (!lastWasUnderscore) {
        out[outIdx++] = '_';
        lastWasUnderscore = true;
      }
    } else {
      out[outIdx++] = emit;
      lastWasUnderscore = false;
    }
    i += advance;
  }

  while (outIdx > 0 && out[outIdx - 1] == '_') outIdx--;
  out[outIdx] = '\0';

  if (outIdx == 0) {
    strlcpy(out, "competicao", outSize);
  }
}
