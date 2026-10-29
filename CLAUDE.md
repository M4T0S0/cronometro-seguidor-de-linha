# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Cronômetro para corrida de robô seguidor de linha, rodando inteiro num ESP32: sensores de largada/chegada, display OLED e uma página web do juiz servida pelo próprio ESP32 (AP Wi-Fi próprio, sem servidor externo nem internet). Código, comentários, mensagens de erro e UI são em português — mantenha assim.

## Build e ferramentas

- Não há build por linha de comando. Há testes automáticos da página do juiz (`testes/`, Node puro, sem dependências): rode `node --test` na raiz depois de mexer no `index.html`. O firmware C++ ainda não tem testes. A cada push, `.github/workflows/verificar.yml` roda os testes, confere o `web_page_gz.h` e compila o firmware no Linux com versões fixas (esp32 2.0.14, Adafruit SSD1306 2.5.17, GFX 1.12.6, BusIO 1.17.4, ArduinoJson 7.4.3) e falha se a memória do programa passar de 95%. Ao trocar versão de biblioteca, atualize lá também. O firmware é compilado e gravado pelo **Arduino IDE**: abrir `firmware/cronometro/cronometro.ino`, placa **ESP32 Dev Module**, partição padrão.
- Core **esp32 da Espressif 2.0.14 exatamente** — o WPA2-Enterprise (eduroam) depende de APIs que mudaram no 3.x. Bibliotecas: Adafruit SSD1306, Adafruit GFX, ArduinoJson **v7**.
- A página do juiz é editada em `firmware/cronometro/pagina/index.html` e embutida no firmware compactada em `web_page_gz.h` (gerado, nunca editar à mão). Depois de qualquer mudança no HTML:
  ```bash
  python ferramentas/gerar_pagina.py          # regenera web_page_gz.h
  python ferramentas/gerar_pagina.py --check  # só confere se está em dia (sai 1 se não)
  ```
  A saída é determinística (gzip com `mtime=0`), então commite o `.h` junto com o `.html`.
- Debug é pelo Serial a 115200 baud (logs com prefixos `[RESULT]`, `[ARM]`, `[SENSORS]`, `[WEB]`…).

## Arquitetura

**Uma única linha de execução.** Tudo roda no `loop()` do `.ino` (Core 1): `webServerHandle()` atende no máximo uma requisição por iteração, no mesmo loop da máquina de estados. Por isso `race_control` e os demais módulos não usam mutex — os handlers HTTP e a máquina de estados chamam as mesmas funções `raceControlXxx()` diretamente. Nada pode bloquear: sem `delay()` (o buzzer e o display são não-bloqueantes/throttled). Trabalho lento (sync com a nuvem) é adiado para depois de pelo menos uma volta do `webServerHandle()` — ver o caso `FINISHED` no `.ino`.

**Máquina de estados** (`state_machine.h`, transições só por `transitionTo()`): `WAITING_START → RUNNING → FINISHED (→ SAVING) → WAITING_START`. O Sensor 1 só é aceito quando o juiz **armou** (competição/robô/dia/tomada) e **liberou** a corrida; fora disso os eventos são descartados com `sensorXFlush()`. Sensores usam ISR mínima (só timestamp + flag) consumida no loop; debounce, lockout START→STOP e detecção de sensor travado ficam em `sensors.cpp`.

**Fluxo de uma corrida:** página `POST /api/v1/arm` → `raceControlArm` → liberar → Sensor 1 captura o `RunContext` (uma vez, estável durante a corrida) → Sensor 2 ou abortar (= `dnf`) / retentar (rearma a mesma tomada sem gravar) → resultado salvo como `pending_validation` → juiz valida/invalida via `PATCH .../validation` em `web_server.cpp`, que é quem avança a tomada (`maybeAdvanceAttempt`). Resultados já resolvidos (DNF) avançam direto no `.ino`.

**Persistência (LittleFS, flash interna) — a flash é a única fonte de verdade:**
- `data_store`: equipes, robôs, competições e vínculos vivem inteiros em arrays fixos na RAM (tetos `MAX_*` em `config.h`) e são persistidos em CSVs com cabeçalho em português (`/participantes.csv`, `/competicoes.csv`, `/vinculos.csv`), recarregados no boot — é assim que uma competição sobrevive a queda de energia.
- `storage`: corridas em JSON Lines append-only em `/<slug>/Dia_N/resultados.jsonl` (linha corrompida não invalida as outras). Corrida sem contexto cai em `/avulsas.jsonl`. Falha de flash nunca para a cronometragem: há fila de pendências em RAM.
- `data_version`: contador global incrementado a cada escrita; `web_server.cpp` cacheia respostas JSON caras (ranking, day-status, runs) por versão, porque a página faz polling a cada ~1,5 s. Toda nova escrita que a página consulta precisa chamar `dataVersionBump()`.
- Configurações (Wi-Fi STA, nuvem, senha do juiz, senha do AP) ficam na NVS via `Preferences`.

**Servidor web** (`web_server.cpp`): `/` serve o HTML gzipado; todo o resto passa por `onNotFound(handleApi)`, que despacha por `resource` em `/api/v1/<resource>/...`. Com senha do juiz ativa, leituras são públicas (`isPublicRequest`) e escritas exigem o token no cabeçalho `X-Sessao`. O botão físico (GPIO 33) segurado no boot apaga a rede externa e a senha do juiz.

**Display** (`display` só desenha; `idle_screen` decide o que mostrar fora de uma corrida, por prioridade: conectando → alertas → robô armado → validação pendente → contexto que a página envia em `POST /display-context` → QR de acesso). O OLED não tem acentos: textos UTF-8 são convertidos para ASCII.

**Outros módulos:** `training_mode` (chave no GPIO 32 lida a cada volta do `loop()`, troca na hora quando não há corrida armada; em treino assume o `loop()` inteiro e o resultado fica no display até a próxima passagem pelo sensor 1), `cloud_sync` (cópia opcional para servidor externo, uma tentativa logo após salvar, ainda não testado em prova), `wifi` (AP sempre ativo + STA opcional com backoff; AP e STA dividem o canal).

## Convenções

- Pinos, tempos, limites e nomes de rede ficam **só** em `config.h`.
- `json_utils` tem extratores manuais de JSON para corpos simples gerados pela própria página; ArduinoJson v7 é usado onde há estrutura.
- Timestamps em microssegundos são `int64_t` (`esp_timer`) — `int` estoura após ~35 min de uptime.

## Preferências do autor

- Sem emojis em nenhum texto (página, display, README).
- Nada de texto técnico na página do juiz (dB, canal, IP, token, HTTP...): só o que um juiz entende.
- Busca de robôs/equipes casa só com o começo do nome ("d" acha "Dragão", não "Seguidor de Linha"), sem diferenciar acento nem maiúscula.
- Formulários da página abrem no painel que sobe de baixo (`openSheet`/`formSheet`), não no meio da lista.
- O display muda por evento (alguém conectou, robô armado), nunca por tempo.
- Toda tela precisa ficar boa em celular, tablet e computador (o juiz pode usar qualquer um). O CSS parte do celular e só acrescenta em `@media (min-width: 768px)` (formulários viram janela no meio) e `(min-width: 900px)` (largura por tela via `body[data-page]`, competição em duas colunas); conferir em 360, 768, 1024, 1366 e 1920 px sem nada vazando para os lados. Na pista o uso mais comum é no celular, em pé; precisa funcionar sem internet (nada de fontes, scripts ou imagens externas) e caber na flash (hoje ~42 KB compactada, flash em 88%).
