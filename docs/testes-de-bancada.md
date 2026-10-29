# Testes de bancada

Este é o roteiro que eu sigo antes de levar uma versão nova para uma prova. A ordem importa: as partes 1 e 2 são as que podem atrapalhar uma corrida de verdade, então se faltar tempo, elas vêm primeiro.

Se algo falhar, anote o número do item, o que esperava, o que apareceu (celular e display) e copie as últimas linhas do Serial Monitor (115200).

**Material:** o cronômetro montado, dois celulares (de preferência um Android e um iPhone), um notebook, um roteador ou hotspot de celular e uma planilha CSV de teste com uns 5 robôs.

## 0. Gravar

- [ ] 0.1 Rodar `node --test` e `python ferramentas/gerar_pagina.py --check` na pasta do projeto. Os dois precisam passar.
- [ ] 0.2 Abrir `firmware/cronometro/cronometro.ino` no Arduino IDE (esp32 2.0.14, placa ESP32 Dev Module) e gravar. Em **Ferramentas**, conferir que "Erase All Flash Before Sketch Upload" está **desligado**, senão as competições salvas somem.
- [ ] 0.3 No Serial: sem erros, aparecem `[WIFI] Access Point do sistema ativo` e `[WEB] Servidor HTTP local iniciado`.

## 1. A corrida (o mais importante)

Crie uma competição de teste com 1 dia, 3 tomadas e 3 robôs.

- [ ] 1.1 **Sensores:** o de largada inicia e o de chegada para. Se estiver trocado, os pinos ficam em `config.h` (`PIN_SENSOR1` é a largada).
- [ ] 1.2 Escolher um robô: a tela mostra **Pronto para liberar** com o nome grande e "Dia 1, tomada 1"; o display mostra o robô.
- [ ] 1.3 **Trocar robô** volta para a lista; o display acompanha.
- [ ] 1.4 **Liberar corrida**: a tela fica amarela, **Aguardando largada**. Passar pelo sensor de largada antes de liberar não pode iniciar nada.
- [ ] 1.5 Largar: a tela fica verde e o **tempo corre no celular**, batendo com o display (diferença de décimos é normal). LED verde aceso, buzzer apita.
- [ ] 1.6 Chegada: o tempo para, emoldurado, com **Válida** (verde) e **Inválida** (vermelho) lado a lado. LED vermelho aceso.
- [ ] 1.7 Tocar **Válida**: aparece a faixa "nome, tempo, registrada como Válida" com **Corrigir**, e some sozinha em alguns segundos. LED apaga, display volta ao normal, a próxima tomada do robô já fica selecionada.
- [ ] 1.8 Tocar **Corrigir** na faixa e trocar para Inválida: o ranking muda e **nenhum robô é selecionado sozinho**.
- [ ] 1.9 **Repetir tomada** (na validação e durante a corrida) pede confirmação; a tomada é repetida e não conta.
- [ ] 1.10 **Abortar** durante a corrida pede confirmação e registra "Não completou".
- [ ] 1.11 **Não compareceu** com o robô selecionado: pede confirmação, registra "Não completou" sem tempo e já seleciona a próxima tomada dele.
- [ ] 1.12 **Não compareceu** com a corrida liberada: depois disso, passar pelo sensor de largada **não** inicia o tempo.
- [ ] 1.13 Tocar **Não compareceu** em todas as tomadas de um robô: ele vai para "Já terminaram o dia" e o dia consegue terminar.
- [ ] 1.14 No último robô, na última tomada, tocar **Repetir tomada**: a competição **não** pode encerrar sozinha; a tomada é repetida.
- [ ] 1.15 Quando todos terminam: aparece o botão do próximo dia (ou **Encerrar competição** no último). Encerrada, ela vai para o Histórico.

### Com dois celulares abertos na mesma competição

- [ ] 1.16 Os dois tocam **Válida** na mesma corrida, quase juntos: o segundo recebe "Esta corrida já foi decidida" e nada mais acontece.
- [ ] 1.17 Um celular seleciona um robô; o outro, com a tela antiga, tenta selecionar outro: recebe "Já tem um robô selecionado".
- [ ] 1.18 O celular que não tocou em nada atualiza sozinho em poucos segundos (até uns 5 s fora de corrida, 1,5 s durante).

## 2. Conexão

- [ ] 2.1 Ao ligar, o display mostra o **QR do Wi-Fi** com nome e senha. Ler com a câmera conecta sozinho.
- [ ] 2.2 Assim que o celular entra, o display troca **na hora** para o QR da página, com o endereço ao lado. Ler abre a página do juiz.
- [ ] 2.3 Desligar e ligar: a senha do Wi-Fi é a mesma.
- [ ] 2.4 No meio da competição, tirar o celular do Wi-Fi do cronômetro: em uns 10 s aparece a faixa vermelha **Sem conexão com o cronômetro**, e a tela **continua** mostrando a competição. Voltar para a rede: a faixa some sozinha.
- [ ] 2.5 Com o cronômetro conectado na internet, abrir a página pelo outro endereço (o da rede externa): pede a senha do juiz uma vez nesse endereço. É esperado.

## 3. Modo treino

- [ ] 3.1 Ligar com a chave **já em treino**: a tela de treino aparece na hora, sem passar por "Conectando".
- [ ] 3.2 Ligar em competição e virar a chave: troca para treino na hora.
- [ ] 3.3 Uma volta: o tempo fica no display, com o LED vermelho, até o robô passar de novo pela largada, que já começa a volta seguinte.
- [ ] 3.4 Voltar a chave para competição: volta para a tela de acesso normal.

## 4. Painel (TV)

- [ ] 4.1 **Painel (TV)** abre em outra aba, sem a barra do juiz e com letras grandes; **Tela cheia** funciona no notebook.
- [ ] 4.2 Com um robô na pista, o Painel mostra quem é e o tempo correndo; sem ninguém, mostra a última corrida.
- [ ] 4.3 A tabela tem posição, robô e melhor tempo. Robô sem tempo válido aparece como "sem tempo".
- [ ] 4.4 Com mais robôs do que cabem na tela, o ranking passa de página sozinho.
- [ ] 4.5 Painel aberto não muda o que aparece no display.
- [ ] 4.6 Com o Painel e dois celulares abertos durante várias corridas, os tempos continuam coerentes e nada trava.

## 5. Senha do juiz

Use um celular como "juiz" e outro como "público".

- [ ] 5.1 Sem senha, qualquer um mexe em tudo.
- [ ] 5.2 **Configurações → Senha do juiz**: menos de 6 caracteres é recusado. Quem cria a senha continua dentro.
- [ ] 5.3 O público vê a página, o ranking e o Painel sem senha. Ao tentar mexer, sobe **Entrar como juiz** dentro da página. **Só ver** fecha.
- [ ] 5.4 Senha certa: entra, e não pede de novo nem depois de reiniciar o cronômetro. Repetir no iPhone.
- [ ] 5.5 Errar 5 vezes: "Muitas senhas erradas. Espere 30 segundos". O juiz já logado continua usando normalmente.
- [ ] 5.6 **Trocar senha** num celular: o outro passa a pedir a nova. **Sair deste aparelho** pede a senha de novo.
- [ ] 5.7 Botão de reset (se montado): segurar 3 s ao ligar apaga a rede externa e a senha do juiz; o resto continua.

## 6. Internet (opcional)

- [ ] 6.1 **Configurações → Internet** já abre buscando; a lista mostra cada rede com o que ela pede e o sinal; a rede atual aparece como "conectado agora".
- [ ] 6.2 Tocar numa rede com senha: só pede a senha. Na eduroam: usuário e senha. Rede aberta: nada.
- [ ] 6.3 Errar a senha: aparece "Senha incorreta" e ele continua tentando.
- [ ] 6.4 Desligar e ligar o cronômetro: reconecta sozinho. No Serial a senha não aparece.
- [ ] 6.5 **Desconectar da internet** apaga a rede salva.

## 7. Cadastro

- [ ] 7.1 **Equipes e robôs**: criar equipe, cadastrar robôs em sequência, renomear, mudar robô de equipe.
- [ ] 7.2 Nome com aspas é recusado com mensagem vermelha dentro do formulário.
- [ ] 7.3 **Importar**: baixar o modelo, preencher no Excel, salvar como CSV e importar. Importar de novo não duplica.
- [ ] 7.4 Busca: `ve` acha "Ventania" mas não "Overlord"; `d` não acha "Seguidor de Linha".
- [ ] 7.5 Excluir um robô que já correu avisa quantas corridas somem. Robô selecionado para correr não pode ser excluído nem removido da competição.
- [ ] 7.6 Editar a competição: o nome muda mesmo depois de ter corridas; dias e tomadas ficam travados.

## 8. Aparelhos e telas

- [ ] 8.1 **Notebook:** a competição aparece em duas colunas (controle à esquerda, ranking à direita); os formulários abrem como janela no meio da tela.
- [ ] 8.2 **Tablet** (em pé e deitado) e **celular pequeno**: nada fica cortado nem rola para os lados.
- [ ] 8.3 **Modo sol** (ícone no topo): a tela fica clara e legível sob luz forte; o aparelho lembra a escolha.
- [ ] 8.4 **Android:** o celular vibra quando sai um resultado para validar.
- [ ] 8.5 **Baixar planilha do ranking** abre no Excel com acentos certos.
- [ ] 8.6 **Configurações → Backup** baixa o arquivo, também com senha ligada.

## Se algo der errado

Anote o número do item, o que esperava, o que apareceu e as últimas linhas do Serial. Se for algo da parte 1 ou 2, grave de volta a versão anterior (`git switch main`) antes da próxima prova.
