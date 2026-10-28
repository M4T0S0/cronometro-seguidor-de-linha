#pragma once
#include <Arduino.h>

// =====================================================================
// JSON_UTILS.H — Extratores manuais de JSON + slugify
//
// Antes viviam em api.cpp (o ESP32 falava com um servidor externo) e em
// server/utils.py (slugify, em Python). Sem servidor externo, essas duas
// pontas viraram a mesma responsabilidade local: extractJsonString/Int
// seguem uteis para ler o corpo de requisicoes recebidas pelo WebServer
// (ex.: campos simples de POST /api/v1/arm), e slugify() agora roda no
// proprio ESP32 (antes o Python normalizava acentos/maiusculas ao criar
// a competicao) para nomear a pasta na flash.
// =====================================================================

// Extrai o valor de uma chave string simples: "chave":"valor". Não é um
// parser JSON completo (não trata aninhamento nem escapes complexos) —
// suficiente porque o corpo é sempre gerado pela própria página local.
void extractJsonString(const String &body, const char* key, char* out, size_t outSize);

// Extrai o valor de uma chave numérica simples: "chave":123.
int extractJsonInt(const String &body, const char* key, int defaultValue);

// Igual acima, mas em 64 bits — necessário para os timestamps em
// microssegundos (esp_timer_get_time()), que passam de 2^31 depois de
// ~35 minutos de uptime e estourariam um int comum.
int64_t extractJsonInt64(const String &body, const char* key, int64_t defaultValue);

// Converte um nome livre (ex.: "Desafio de Verão 2026") num slug seguro
// para nome de pasta na flash (ex.: "Desafio_de_Verao_2026") — minúsculas
// não são forçadas (mantém o nome legível), só remove acentos/espaços/
// caracteres especiais. 'out' deve ter pelo menos 'outSize' bytes;
// resultado sempre cabe em 31 caracteres (ver RunContext::competitionSlug).
void slugify(const char* text, char* out, size_t outSize);
