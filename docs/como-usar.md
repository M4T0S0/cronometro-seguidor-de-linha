# Como usar numa competição

Este é o caminho que eu sigo num dia de prova, do ligar ao ranking final.

## Antes da prova

### Entrar no cronômetro

1. Ligue o cronômetro com a chave no modo competição. O display mostra um QR code com o nome da rede (`CronometroCompeticao`) e a senha ao lado. Cada cronômetro sorteia a própria senha no primeiro uso, e ela não muda depois.
2. Leia o QR com a câmera do celular para entrar na rede. Num notebook, digite o nome e a senha que aparecem no display.
3. Assim que alguém entra, o display troca para um segundo QR code, com o endereço da página (normalmente `192.168.4.1`). Leia e a página do juiz abre.

Alguns celulares saem sozinhos de redes sem internet. Se a página parar de responder, confira se o celular continua na rede do cronômetro.

### Cadastrar equipes e robôs

Em **Equipes e robôs** dá para criar equipes e robôs um por um ou importar uma planilha. Para importar, toque em **Importar arquivo**, baixe o modelo, preencha no Excel (colunas `equipe`, `robo` e, se quiser, `codigo`) e salve como CSV.

O cadastro vale para todas as competições. Se escrever um nome errado, dá para corrigir depois, e as corridas já feitas passam a mostrar o nome certo.

### Criar a competição

Em **Nova competição**, escolha o nome, quantos dias e quantas tomadas cada robô tem por dia. Depois, em **Inscritos**, adicione os robôs que vão correr. Dias e tomadas só podem mudar antes da primeira corrida; o nome pode mudar a qualquer hora.

### Senha do juiz (recomendado)

Em **Configurações** (a engrenagem no topo) → **Senha do juiz**, crie uma senha. Com ela ligada, qualquer pessoa na rede do cronômetro vê o ranking e as corridas, mas só quem souber a senha consegue mexer. O celular que criou a senha continua logado.

## Durante a prova

Na tela da competição:

1. **Escolher robô:** a lista mostra cada robô com a tomada que ele vai fazer agora. Quem já fez todas as tomadas do dia fica separado embaixo.
2. **Pronto para liberar:** confira se é o robô certo. Se não for, toque em **Trocar robô**. Quando o robô estiver posicionado, toque em **Liberar corrida**.
3. **Aguardando largada:** o tempo começa sozinho quando o robô passa pelo sensor de largada.
4. **Corrida em andamento:** o tempo aparece correndo no celular e no display. Se precisar parar, use **Repetir tomada** (a tomada não conta) ou **Abortar** (fica registrada como não completou).
5. **Validar resultado:** quando o robô passa pelo sensor de chegada, o tempo aparece grande. Toque em **Válida**, **Inválida** ou **Repetir tomada**.

Errou o toque? Em **Corridas recentes**, toque na corrida e escolha o resultado certo.

### Painel para o público

O botão **Painel** abre o ranking numa tela própria, com letras grandes e a última corrida em destaque. Abra num notebook ligado a uma TV ou projetor, conectado à rede do cronômetro.

## No fim do dia

Quando todos os robôs fizerem suas tomadas, aparece o botão para começar o próximo dia. No último dia, o botão encerra a competição. O ranking final fica em **Histórico**.

Para guardar uma cópia de tudo no computador, use **Configurações** → **Backup**.

## Modo treino

Com a chave no modo treino, o cronômetro só mede: o robô passa pelo sensor de largada, o tempo corre no display e para no sensor de chegada. O tempo fica no display até o robô passar de novo pela largada, que já começa a próxima volta. Nada é gravado.

## Se algo der errado

- **Esqueci a senha do juiz:** desligue o cronômetro, segure o botão de reset e ligue, mantendo por 3 segundos. A senha some, junto com a rede de internet salva. Nada da competição é apagado.
- **O display avisa que um sensor está travado:** tem algo na frente do sensor ou ele desalinhou.
- **Quero ligar na internet:** em **Configurações** → **Internet**, escolha a rede na lista. Redes de faculdade (eduroam) pedem usuário e senha. O cronômetro só enxerga redes de 2,4 GHz. Sem internet ele funciona normalmente.
