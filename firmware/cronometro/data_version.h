#pragma once
#include <stdint.h>

// =====================================================================
// DATA_VERSION.H — Contador de versão dos dados (participantes,
// competições, vínculos, corridas).
//
// Por quê: ranking/day-status/lista de corridas são caros de calcular
// (varrem arquivo(s) na flash — ver ranking.cpp, data_store.cpp,
// web_server.cpp), mas a página do juiz pergunta de novo a cada ~1.5s
// mesmo quando nada mudou (não há mais servidor com SSE para avisar só
// quando algo muda de verdade). Isso estava deixando o loop() principal
// perceptivelmente lento (o mesmo núcleo também cuida dos sensores e do
// display — ver CronometroCompeticao.ino).
//
// Este módulo não sabe NADA sobre o que mudou, só QUE algo mudou —
// storage.cpp e data_store.cpp chamam dataVersionBump() sempre que
// gravam algo que a página consulta; web_server.cpp guarda, junto de
// cada resposta JSON já pronta, a versão em que foi calculada, e só
// recalcula quando a versão atual for diferente (ver JsonCache em
// web_server.cpp). Um módulo isolado evita dependência circular entre
// storage.cpp (mais "baixo nível") e data_store.cpp (mais "alto nível").
// =====================================================================

void dataVersionBump();
uint32_t dataVersionCurrent();
