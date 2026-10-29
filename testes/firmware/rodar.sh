#!/bin/sh
# Compila a lógica do firmware com as peças falsas e roda os testes.
# Precisa só do g++ (Linux, WSL ou a conferência automática do GitHub).
# Uso, na raiz do projeto:  sh testes/firmware/rodar.sh
set -e
cd "$(dirname "$0")/../.."
F=firmware/cronometro
T=testes/firmware
mkdir -p build
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -I "$T/falsos" -I "$F" \
  "$T/teste_firmware.cpp" "$T/falsos/falsos.cpp" \
  "$F/data_store.cpp" "$F/storage.cpp" "$F/ranking.cpp" "$F/race_control.cpp" \
  "$F/data_version.cpp" "$F/json_utils.cpp" "$F/device.cpp" "$F/timer.cpp" "$F/state_machine.cpp" \
  -o build/teste_firmware
./build/teste_firmware
