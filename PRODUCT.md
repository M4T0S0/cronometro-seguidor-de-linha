# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

- **Juiz / organizador de prova** (usuário principal): opera a página do juiz no celular, na beira da pista, durante uma competição de robô seguidor de linha. Escolhe o robô, libera a corrida, valida/invalida/repete a tomada, corrige erros, avança dias. O projeto é aberto (MIT) e feito para que **outras competições e equipes montem e operem o próprio cronômetro sem o autor presente** — o juiz pode nunca ter visto o sistema antes do dia da prova.
- **Público e equipes**: veem o ranking no **Painel** (TV/projetor/notebook) e, com senha do juiz ativa, podem abrir a página no próprio celular só para ver ranking e corridas.
- **Equipes em treino**: usam o modo treino (chave física) só pelo display, sem a página.

## Product Purpose

RaceTrack UTFPR cronometra provas de seguidor de linha com precisão de milissegundos e organiza a competição inteira (cadastro, dias, tomadas, validação, ranking pelo melhor tempo válido) dentro de um ESP32. Existe porque a versão anterior dependia de notebook, servidor e Wi-Fi do local, e quebrava justamente na hora da prova. Sucesso = uma prova inteira roda do ligar ao ranking final sem computador, sem internet, sem perder resultado, conduzida por alguém sem treino.

## Positioning

Tudo vive no próprio cronômetro: ele cria a rede, serve a página do juiz, guarda as competições na flash e sobrevive a queda de energia no meio da prova. Entrar é ler dois QR codes no display — sem app, sem digitar endereço.

## Operating Context

- Local de prova (ginásio, sala de aula, evento), muitas vezes sem internet e com rede lotada; celulares podem sair sozinhos da rede sem internet (a página avisa com faixa vermelha).
- Fluxo por corrida: escolher robô → Liberar → robô passa pelo sensor de largada → tempo corre no celular e no display → chegada → Válida / Inválida / Repetir tomada. Abortar registra DNF. Corridas recentes permitem corrigir o veredito.
- Competição organizada em dias × tomadas por dia; ranking pelo melhor tempo válido; fim do dia avança, último dia encerra e vai para Histórico.
- Cadastro de equipes e robôs (manual ou importando CSV/JSON a partir de modelo); exportação do ranking em planilha; backup em Configurações.
- Painel aberto em notebook ligado a TV/projetor, em tela cheia, com ranking e última corrida em destaque.
- O display OLED do cronômetro complementa a página (QR de acesso, robô armado, tempo, validação pendente) e reflete em que tela o juiz está.

## Capabilities and Constraints

- **Zero recurso externo**: a página é um único `firmware/cronometro/pagina/index.html` servido pelo ESP32 sem internet — nada de fontes web, CDN, imagens ou scripts remotos.
- **Tamanho na flash**: o HTML vai gzipado para o firmware (`ferramentas/gerar_pagina.py` → `web_page_gz.h`); precisa continuar pequeno. Toda mudança na página exige regenerar o `.h`.
- **Servidor modesto**: o ESP32 atende uma requisição por vez no mesmo loop que cronometra; a página faz polling (sem SSE/WebSocket). Nada de pedidos pesados ou em rajada.
- Senha do juiz opcional: com ela, qualquer um vê, só o juiz mexe.
- Wi-Fi externo opcional (inclui eduroam); sincronização com nuvem existe mas ainda não foi testada em prova.
- Limites de capacidade em `firmware/cronometro/config.h` (ex.: 80 robôs, 30 equipes, 20 competições).
- Terminologia fixa: **equipe**, **robô**, **competição**, **dia**, **tomada**, **Liberar**, **Válida / Inválida / Repetir tomada**, **Abortar** (= não completou / DNF), **Painel**, **Inscritos**, **Histórico**.
- Interface inteira em português do Brasil.
- **Sem emojis** em nenhum texto (página, display, documentação).
- **Sem texto técnico** na página do juiz (dB, canal, IP, token, HTTP...): só o que um juiz entende.
- **Todos os aparelhos**: o juiz pode usar celular, tablet ou notebook. Toda tela precisa funcionar bem em 360, 768, 1024, 1366 e 1920 px, sem nada vazando para os lados.

## Brand Commitments

- Nome: **RaceTrack UTFPR** (título da página). O dispositivo e a rede usam "Cronômetro" / `CronometroCompeticao`.
- Voz da documentação: primeira pessoa, direta, prática, sem jargão ("Este é o caminho que eu sigo num dia de prova").
- Crédito ao autor (Matheus Leineker Stanula, UTFPR) mantido conforme a licença MIT.

## Evidence on Hand

- Já usado em competição real; vários comportamentos nasceram de problemas vistos na pista (README).
- `docs/como-usar.md` (passo a passo do juiz), `hardware/montagem.md` e `hardware/ligacoes.svg`.
- Não há depoimentos, números de público, fotos de prova ou lista de eventos no repositório — não inventar.

## Product Principles

1. **Funciona sem o autor.** Um juiz que nunca viu o sistema conduz uma prova só com o que a tela mostra.
2. **Nenhum resultado se perde, nenhum toque é definitivo.** Erros de toque sob pressão sempre têm volta (corrigir veredito, trocar robô, repetir tomada).
3. **O cronômetro é autossuficiente.** Nada pode depender de internet, computador ou recurso externo.
4. **O momento da corrida manda.** Durante uma tomada, o estado atual (armado, liberado, correndo, validar) é inconfundível e a ação certa é a mais fácil de tocar.
5. **Leve por necessidade.** Cada byte e cada requisição disputam a mesma memória e o mesmo laço que mede o tempo.

## Accessibility & Inclusion

- Uso com uma mão, em pé, com pressa, sob luz forte de ginásio ou sol: alvos de toque grandes, contraste alto, estados distinguíveis sem depender só de cor.
- Painel legível a vários metros numa TV ou projetor.
