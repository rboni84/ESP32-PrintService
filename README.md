# PrintService

Servidor de monitoramento e impressão para impressoras de rede, em **ESP32-C6** (placa ESP32-C6-DevKitM-1), construído com PlatformIO e Arduino.

O dispositivo fica na rede local, descobre e monitora impressoras por SNMP, e atua como ponte de impressão para um aplicativo externo: o aplicativo envia o documento por WebSocket e o PrintService o entrega à impressora em raw (porta 9100).

## Funcionalidades

- **Configuração sem software**: AP de configuração com captive portal (`http://192.168.4.1/`) e menu no terminal serial (`menu` + Enter). Nome do dispositivo (`PrintService-XXXX`) e senha padrão do AP derivados do MAC; a senha aparece no serial.
- **Rede resiliente**: modo AP + STA. O AP desliga quando o WiFi estabiliza e volta se a conexão cair por 60 s.
- **Descoberta de impressoras**: mDNS (`_ipp`, `_printer`, `_pdl-datastream`) e cadastro manual por IP, persistido.
- **Monitoramento SNMP** (v2c com fallback para v1, cliente próprio, sem dependências): estado do dispositivo e da impressora, erros detectados (papel, toner, tampa, atolamento…), contador de páginas e níveis de suprimentos (toner, cilindro, resíduo).
- **Serviço de impressão**: cliente WebSocket para o aplicativo externo, trabalhos em blocos com confirmação, entrega em raw (porta 9100), IPP (porta 631, para impressoras domésticas e AirPrint) ou LPD (porta 515, modelos antigos), página de teste em PDF, PCL, texto ou PostScript.
- **Portal web** com abas Status, Impressoras, Rede WiFi, Dispositivo e Sistema, e API REST local.
- **LED RGB de status** e **botão BOOT** segurado por 5 s para reset de fábrica.

## Hardware

| Item | Detalhe |
|---|---|
| Placa | ESP32-C6-DevKitM-1 (4 MB flash) |
| LED de status | WS2812 integrado, GPIO8 |
| Botão de reset | BOOT integrado, GPIO9 |
| Serial | USB, 115200 bps |

Os pinos podem ser alterados em `platformio.ini` (`PIN_STATUS_LED`, `PIN_RESET_BTN`).

## Documentação

| Documento | Conteúdo |
|---|---|
| [docs/status-led.md](docs/status-led.md) | Cores do LED, botão BOOT, identidade do dispositivo (nome e senha derivados do MAC) e menu serial |
| [docs/api-contract.md](docs/api-contract.md) | Contrato para o aplicativo externo: provisionamento, protocolo WebSocket (status, impressoras, trabalhos de impressão, página de teste) e API REST local |

## Primeiro uso

1. Grave o firmware e abra o monitor serial. O boot mostra o SSID e a senha do AP.
2. Conecte-se ao AP `PrintService-XXXX`. O captive portal abre; se não abrir, acesse `http://192.168.4.1/`.
3. Na aba **Rede WiFi**, escolha a rede e salve. O dispositivo reinicia e conecta.
4. Acesse `http://PrintService-XXXX.local/` (ou o IP mostrado no serial). As impressoras que anunciam mDNS aparecem na aba **Impressoras**; as demais podem ser adicionadas por IP.
5. Para integrar ao aplicativo externo, informe URL WebSocket e token na aba **Dispositivo**.

Tudo isso também pode ser feito pelo menu serial.

## API REST local

Base `http://<ip>/`, autenticação HTTP Basic (usuário `admin`) quando houver senha de portal.

| Rota | Função |
|---|---|
| `GET /api/status` | Estado geral, rede, contadores, estado do servidor externo |
| `GET /api/printers` | Lista de impressoras com status e suprimentos |
| `POST /api/printers` · `POST /api/printers/remove` · `POST /api/printers/refresh` | Cadastro manual e atualização |
| `GET|POST /api/cloud` | Estado e configuração do servidor externo |
| `GET /api/print/status` · `POST /api/print/test` · `POST /api/print/cancel` · `POST /api/print` | Impressão e página de teste |

Detalhes em [docs/api-contract.md](docs/api-contract.md).

## Compilação

Requisitos: [PlatformIO Core](https://platformio.org/) 6.2 ou superior (ou a extensão do VS Code). A plataforma usada é a [pioarduino](https://github.com/pioarduino/platform-espressif32) 55.03.311 (Arduino core 3.3 / ESP-IDF 5.5), fixada em `platformio.ini`.

```
pio run -e esp32-c6-devkitm-1            # compilar
pio run -e esp32-c6-devkitm-1 -t upload  # gravar
pio device monitor                       # serial 115200
```

Observações para Windows, detalhadas nos comentários de `platformio.ini`:

- Execute o PlatformIO pelo **PowerShell ou CMD**, nunca pelo Git Bash: o instalador de toolchain da pioarduino aborta em ambiente MSYS.
- Não edite `platformio.ini` com um build em andamento: a extensão do VS Code dispara um `pio project init` concorrente.
- Os pacotes são instalados em `C:\pio` para evitar o limite de 260 caracteres de caminho.

Dependências (resolvidas automaticamente): ESPAsyncWebServer, AsyncTCP, WebSockets (links2004), ArduinoJson.

## Estrutura

```
src/
  main.cpp         inicializacao e loop
  config.*         configuracao persistente (NVS), nome e senha derivados do MAC
  portal.*         WiFi AP/STA, captive portal, servidor web e API REST
  web_pages.h      pagina HTML/JS do portal (embutida)
  console.*        menu no terminal serial
  printers.*       descoberta mDNS e sondagem SNMP das impressoras
  snmp.*           cliente SNMP v1/v2c (GET/GETNEXT) nao bloqueante
  printjob.*       entrega raw (9100), IPP (631) ou LPD (515) e pagina de teste (PDF/PCL/texto/PS)
  cloud.*          cliente WebSocket para o aplicativo externo
  status_led.*     LED WS2812 de status
  reset_button.*   botao BOOT: reset de fabrica
docs/              documentacao tecnica
```

## Licença

A definir.
