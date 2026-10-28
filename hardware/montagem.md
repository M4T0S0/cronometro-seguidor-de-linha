# Montagem

![Ligações do cronômetro](ligacoes.svg)

Todos os pinos ficam em `firmware/cronometro/config.h`. Se precisar trocar algum, é só mudar lá e gravar de novo.

## Pinos

| O quê | Pino do ESP32 | Constante no `config.h` |
|---|---|---|
| Sensor de largada | GPIO 25 | `PIN_SENSOR1` |
| Sensor de chegada | GPIO 26 | `PIN_SENSOR2` |
| Display (SDA) | GPIO 21 | `OLED_SDA_PIN` |
| Display (SCL) | GPIO 22 | `OLED_SCL_PIN` |
| LED verde | GPIO 27 | `PIN_LED_GREEN` |
| LED vermelho | GPIO 14 | `PIN_LED_RED` |
| Buzzer | GPIO 13 | `PIN_BUZZER` |
| Chave do modo treino | GPIO 32 | `PIN_MODE_SELECT` |
| Botão de reset | GPIO 33 | `WIFI_RESET_BUTTON_PIN` |

## Sensores

Eu uso o E18-D80NK, que é alimentado com 5 V e entrega 5 V na saída. O ESP32 trabalha com 3,3 V, então **não ligue a saída do sensor direto no pino**. Em cada sensor eu coloco um divisor de tensão com um resistor de 100 kΩ (entre a saída do sensor e o pino) e um de 220 kΩ (entre o pino e o GND). Com isso o pino recebe por volta de 3,4 V.

Por causa do divisor, o código configura o pino como entrada simples, sem o resistor interno do ESP32 (que atrapalharia a divisão).

O E18-D80NK vai para nível baixo quando detecta o robô. Se o seu sensor funcionar ao contrário, troque `SENSOR_ACTIVE_LEVEL` para `HIGH` no `config.h`.

Pinos que eu evitaria para os sensores:

- **GPIO 34 a 39:** só funcionam como entrada e não têm resistor interno.
- **GPIO 0, 2, 12 e 15:** interferem na forma como o ESP32 liga.
- **GPIO 6 a 11:** são usados pela memória flash.

## Display

OLED SSD1306 de 128x64 com I2C, no endereço 0x3C. Alguns módulos usam 0x3D. Se o display ficar apagado, troque `OLED_I2C_ADDRESS` no `config.h`.

## Chave e botão

- **Chave do modo treino:** um lado no GPIO 32, o outro no GND. Fechada, o cronômetro entra em modo treino; aberta, fica no modo competição. Dá para trocar com ele ligado.
- **Botão de reset:** um lado no GPIO 33, o outro no GND. Segurando o botão por 3 segundos enquanto liga o cronômetro, ele esquece a rede de internet salva e tira a senha do juiz. Cadastros, competições e corridas não são apagados.

## LEDs e buzzer

Os LEDs vão em série com um resistor, como em qualquer LED. O verde acende enquanto a corrida está rodando; o vermelho fica aceso quando o tempo terminou e está esperando o juiz decidir.

O buzzer pode ser ativo ou passivo. Ele apita na largada e na chegada.
