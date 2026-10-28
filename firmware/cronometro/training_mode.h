#pragma once

extern bool trainingModeActive;

void trainingModeInit(); // só pinMode, chamar uma vez no setup()

bool trainingSwitchOn(); // posição física da chave (true = treino)

// Lê a chave e troca de modo se for um momento seguro (sem corrida
// armada/em andamento). Chamar TODA iteração do loop(), antes do
// switch principal.
void trainingModePoll();

void trainingLoop(); // chamado quando trainingModeActive == true