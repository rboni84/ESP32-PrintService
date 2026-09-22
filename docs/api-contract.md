# Contrato de API: PrintService ⇄ aplicativo externo

Versão do protocolo: **1**. Firmware a partir de 0.3.0.

Este documento descreve como um servidor externo (o "aplicativo") integra-se ao dispositivo PrintService (ESP32-C6) para:

1. receber status e a lista de impressoras monitoradas;
2. enviar trabalhos de impressão, que o dispositivo entrega à impressora em raw (porta 9100);
3. disparar uma página de teste.

Há duas interfaces:

| Interface | Direção | Uso |
|---|---|---|
| **WebSocket** (seção 2) | dispositivo → aplicativo (cliente → servidor) | Canal permanente. É a interface principal para o aplicativo. |
| **REST local** (seção 3) | qualquer cliente na LAN → dispositivo | Configuração, diagnóstico e página de teste na rede local. |

---

## 1. Provisionamento

O aplicativo é quem provisiona. Para cada dispositivo ele gera:

| Item | Formato | Exemplo |
|---|---|---|
| URL WebSocket | `ws://` ou `wss://` + host + porta opcional + caminho | `wss://app.exemplo.com/ws/devices/PrintService-3A7F` |
| Token | string opaca, até 128 caracteres, sem espaços | `eyJhbGciOi...` |

Recomendação: incluir o identificador do dispositivo no caminho e emitir um token por dispositivo, revogável.

O operador insere URL e token no dispositivo por um destes meios:

- Portal web → aba **Dispositivo** → cartão **Servidor externo**.
- Menu serial (115200 bps, `menu` + Enter) → opção **s**.
- REST local: `POST /api/cloud` com `url` e `token` (form-urlencoded).

Ao salvar, o dispositivo grava em NVS e conecta imediatamente. A conexão só é tentada com o WiFi (modo STA) ativo. Em caso de queda, reconecta a cada 10 s.

### Identificação do dispositivo

O nome é fixo e derivado do MAC: `PrintService-XXXX` (últimos 2 bytes do MAC). Ele é o hostname mDNS (`PrintService-XXXX.local`) e o SSID do AP de configuração.

---

## 2. WebSocket

### 2.1 Handshake HTTP

O dispositivo abre a conexão com estes cabeçalhos:

```
GET <caminho da URL> HTTP/1.1
Host: <host>
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Protocol: arduino
Authorization: Bearer <token>
X-Device-Id: PrintService-3A7F
X-Device-Mac: AA:BB:CC:DD:3A:7F
X-Firmware: 0.3.0
```

O servidor **deve** validar `Authorization`. Se rejeitar, responda `401`; o dispositivo registra erro e tenta de novo em 10 s (não há backoff exponencial, então revogue tokens no servidor em vez de contar com o dispositivo).

O subprotocolo `arduino` é enviado pela biblioteca; o servidor pode ignorá-lo ou ecoá-lo em `Sec-WebSocket-Protocol`.

TLS: com `wss://` o dispositivo **não valida o certificado do servidor** (sem CA embutida). O tráfego fica cifrado contra escuta, mas não contra um servidor impostor. Se isso for inaceitável, use rede privada/VPN ou solicite a versão com CA embutida.

Keep-alive: o dispositivo envia ping WebSocket a cada 15 s e derruba a conexão após 3 pongs perdidos. O servidor deve responder pings no nível do protocolo WebSocket (praticamente todas as bibliotecas fazem isso).

### 2.2 Formato das mensagens

Todas as mensagens são **frames de texto** com um objeto JSON. Campo obrigatório: `type`. Mensagens do dispositivo trazem `ts` (millis desde o boot, apenas para ordenação).

Frames binários não são aceitos: o dispositivo responde `error` com `code: "binary_unsupported"`.

Tamanho máximo de um frame recebido pelo dispositivo: **15 KB**. Isso limita `job.chunk` a 8 KB de dados binários (≈ 10,9 KB em base64) mais o envelope.

### 2.3 Mensagens dispositivo → servidor

#### `hello` (ao conectar)

```json
{
  "type": "hello", "protocol": "1",
  "device": "PrintService-3A7F", "mac": "AA:BB:CC:DD:3A:7F",
  "fw": "0.3.0", "ip": "192.168.0.23", "hostname": "PrintService-3A7F.local",
  "chunk_max": 8192, "buffer": 32768,
  "capabilities": ["raw9100", "snmp", "mdns", "test_page"],
  "ts": 12345
}
```

#### `status` (a cada 30 s, ou a pedido)

```json
{
  "type": "status", "uptime": 3600, "heap": 180000, "rssi": -61, "ip": "192.168.0.23",
  "ap": false, "printers": 3, "printers_online": 2, "printers_alert": 1,
  "printing": false, "ts": 12345
}
```

`job_id` aparece quando `printing` é `true`.

#### `printers` (a cada 60 s, ao conectar, ou a pedido)

```json
{
  "type": "printers", "ts": 12345,
  "data": {
    "count": 1, "online": 1, "alerts": 0, "sta": true,
    "printers": [{
      "ip": "192.168.0.50", "host": "NPI3A7F2C", "name": "HP LaserJet Pro M404",
      "model": "HP LaserJet Pro M404dn", "location": "Recepcao",
      "manual": false, "mdns": true, "online": true, "alert": false,
      "dev_status": 2, "dev_status_text": "operando",
      "prn_status": 3, "prn_status_text": "ociosa",
      "errors": "", "pages": 48213, "last_ok": 12, "snmp_version": "v2c",
      "supplies": [
        {"desc": "Black Cartridge HP CF259A", "level": 62, "max": 100, "pct": 62, "class": 3, "type": 3}
      ]
    }]
  }
}
```

Campos de impressora:

| Campo | Significado |
|---|---|
| `dev_status` | hrDeviceStatus: 1 desconhecido, 2 operando, 3 alerta, 4 teste, 5 parada |
| `prn_status` | hrPrinterStatus: 1 outro, 2 desconhecido, 3 ociosa, 4 imprimindo, 5 aquecendo |
| `errors` | Texto com os bits de hrPrinterDetectedErrorState ativos, separados por vírgula (vazio = nenhum) |
| `pages` | prtMarkerLifeCount ou `null` |
| `last_ok` | segundos desde a última resposta SNMP, ou `null` |
| `supplies[].class` | 3 = consumido (toner, tinta, cilindro); 4 = receptáculo que enche (resíduo) |
| `supplies[].type` | Printer-MIB: 3 toner, 4 resíduo de toner, 5 tinta, 15 cilindro (OPC)… |
| `supplies[].pct` | percentual calculado, ou -1 se desconhecido |
| `supplies[].level` | -1 outro, -2 desconhecido, -3 "resta algum" |
| `alert` | true se offline conhecido, dev_status 3/5, algum erro, toner ≤ 10 %, resíduo ≥ 90 % |

#### `job.ready`, `job.ack`, `job.done`, `job.error`, `job.status`

Ver fluxo em 2.5.

#### `ack`, `error`, `pong`

`ack` confirma comandos simples (`refresh`, `printer.add`, `printer.remove`) e traz `reply_to`. `error` traz `code`, `message` opcional e `reply_to` quando aplicável.

### 2.4 Mensagens servidor → dispositivo

| `type` | Campos | Efeito |
|---|---|---|
| `welcome` | `status_interval` (s, 5–600), `printers_interval` (s, 10–3600), opcionais | Ajusta os intervalos periódicos. O dispositivo responde com `status` e `printers`. Recomendado enviar logo após o `hello`. |
| `ping` | — | Responde `pong`. (Ping de aplicação; o ping WebSocket também funciona.) |
| `get_status` | — | Responde `status`. |
| `get_printers` | — | Responde `printers`. |
| `get_job` | — | Responde `job.status` com o estado do último/atual trabalho. |
| `refresh` | — | Força descoberta mDNS e sondagem SNMP. Responde `ack`. |
| `printer.add` | `printer` (IP) | Cadastra impressora manual. Responde `ack` ou `error` (`code`: `IP invalido`, `ja cadastrada`, `limite de impressoras atingido`). |
| `printer.remove` | `printer` (IP) | Remove. Responde `ack` ou `error` (`not_found`). |
| `print_test` | `printer` (IP), `port` (padrão 9100), `format` (`pcl` padrão, `text`, `ps`), `job_id` opcional | Imprime página de teste. Responde `job.ready` (com `test: true`) e depois `job.done`/`job.error`. |
| `job.start` | `job_id`, `printer`, `port`, `size`, `name` | Abre TCP com a impressora. Responde `job.ready` ou `job.error`. |
| `job.chunk` | `job_id`, `seq`, `data` (base64), `last` (bool) | Entrega um bloco. Responde `job.ack`. |
| `job.cancel` | `job_id` | Aborta e fecha a conexão com a impressora. Resulta em `job.error` com `code: "canceled"`. |

Mensagens desconhecidas recebem `error` com `code: "unknown_type"`.

### 2.5 Fluxo de um trabalho de impressão

O dispositivo executa **um trabalho por vez**. Enquanto um estiver ativo, `job.start` responde `job.error` com `code: "busy"`.

O conteúdo é opaco para o dispositivo: envie exatamente o que a impressora aceita na porta 9100 (PCL, PostScript, PDF em impressoras que suportam PDF direto, ESC/POS etc.). A conversão de documentos é responsabilidade do aplicativo.

```
servidor                                   dispositivo                       impressora
   |-- job.start {job_id, printer, size} -->|                                   |
   |                                        |---- TCP connect :9100 ----------->|
   |<-- job.ready {chunk_max, buffer_free} -|                                   |
   |-- job.chunk {seq:0, data, last:false} >|                                   |
   |                                        |---- bytes -----------------------> |
   |<-- job.ack {seq:0, written, ...} ------|                                   |
   |-- job.chunk {seq:1, ...} ------------->|                                   |
   |<-- job.ack {seq:1} --------------------|                                   |
   |   ...                                  |                                   |
   |-- job.chunk {seq:N, last:true} ------->|                                   |
   |<-- job.ack {seq:N, last:true} ---------|                                   |
   |                                        |---- flush, ~0,8 s, close -------->|
   |<-- job.done {bytes, duration_ms} ------|                                   |
```

Regras de controle de fluxo:

1. **Aguarde `job.ack` antes de enviar o próximo `job.chunk`.** O dispositivo tem 32 KB de buffer; enviar sem esperar pode gerar `overflow` e cancelar o trabalho.
2. Cada bloco tem no máximo `chunk_max` bytes **binários** (8192). Blocos maiores geram `chunk_too_large` e cancelam o trabalho.
3. `seq` começa em 0 e é devolvido no `job.ack` para correlação. O dispositivo não reordena.
4. O último bloco leva `last: true`. Um `job.chunk` com `data` vazio e `last: true` é válido para encerrar.
5. Se o dispositivo ficar 20 s sem receber blocos, cancela com `data_timeout`. Se a impressora parar de aceitar dados por 20 s, `write_timeout`.

Exemplo de bloco:

```json
{"type": "job.chunk", "job_id": "7f3a", "seq": 0, "data": "G0UbJmwxTxsm...", "last": false}
```

`job.ack`:

```json
{"type": "job.ack", "job_id": "7f3a", "seq": 0, "received": 8192, "written": 5840, "buffer_free": 30416, "last": false, "ts": 99}
```

`job.done`:

```json
{"type": "job.done", "job_id": "7f3a", "bytes": 61234, "duration_ms": 2410, "ts": 3200}
```

`job.error`:

```json
{"type": "job.error", "job_id": "7f3a", "code": "connect_failed", "message": "trabalho nao iniciado", "ts": 101}
```

Códigos de erro de trabalho:

| `code` | Quando |
|---|---|
| `busy` | Já há trabalho ativo |
| `no_network` | Dispositivo sem WiFi STA |
| `bad_printer` | IP inválido em `printer` |
| `missing_job_id` | `job.start` sem `job_id` |
| `connect_failed` | Impressora não aceitou TCP na porta (3 s) |
| `no_such_job` | `job.chunk`/`job.cancel` para `job_id` que não está ativo |
| `chunk_too_large` | Bloco decodificado > 8192 bytes |
| `bad_base64` | `data` inválido |
| `overflow` | Buffer cheio (servidor não aguardou `job.ack`) |
| `data_timeout` | 20 s sem novos blocos |
| `write_timeout` | 20 s sem a impressora consumir dados |
| `connection_closed` | Impressora fechou a conexão antes do fim |
| `canceled` | `job.cancel` recebido |

Após `job.done` ou `job.error` o dispositivo está livre para o próximo `job.start`.

### 2.6 Página de teste

`print_test` gera no próprio dispositivo uma página com nome do dispositivo, firmware, MAC, rede, IP da impressora, modelo/nome/local e contador vindos do SNMP, um teste de caracteres e uma linha de confirmação. Formatos:

| `format` | Conteúdo | Indicado para |
|---|---|---|
| `pcl` (padrão) | `ESC E` + texto + `FF` + `ESC E` | Laser e multifuncionais PCL; a maioria aceita texto puro após o reset |
| `text` | Texto puro + `FF` | Matriciais, térmicas e impressoras de texto |
| `ps` | Programa PostScript de uma página | Impressoras só PostScript |

Impressoras "GDI/host-based" sem interpretador não imprimem nenhum dos formatos; nesse caso o teste falha silenciosamente na impressora (o TCP costuma ser aceito). O `job.done` indica apenas que os bytes foram entregues.

---

## 3. REST local (LAN)

Base: `http://<ip-do-dispositivo>/` ou `http://PrintService-XXXX.local/`. Se houver senha de portal, usar HTTP Basic com usuário `admin`. Corpo de `POST` em `application/x-www-form-urlencoded`, salvo indicação.

| Método e rota | Parâmetros | Resposta |
|---|---|---|
| `GET /api/status` | — | Estado geral: rede, AP, contadores de impressoras, `printing`, objeto `cloud` (igual a `GET /api/cloud`) |
| `GET /api/printers` | — | Mesmo objeto de `printers.data` da seção 2.3 |
| `POST /api/printers` | `ip` | `{"ok":true}` ou 400 `{"error":...}` |
| `POST /api/printers/remove` | `ip` | `{"ok":true}` ou 404 |
| `POST /api/printers/refresh` | — | `{"ok":true}` |
| `GET /api/cloud` | — | `{"enabled","url","token_set","state","connected","last_error","connected_for","reconnects"}` |
| `POST /api/cloud` | `url` (vazio = desativar), `token` (vazio = manter, espaço = remover) | Objeto de `GET /api/cloud` já com o novo estado |
| `GET /api/print/status` | — | `{"state","busy","buffer_free","chunk_max","job":{...}}` |
| `POST /api/print/test` | `ip`, `port` (9100), `format` (`pcl`/`text`/`ps`) | 202 `{"ok":true,"job_id":"test-123"}`; 409 se ocupado; 502 se a impressora recusou TCP |
| `POST /api/print/cancel` | — | `{"ok":true}` ou 404 |
| `POST /api/print?ip=&port=&name=` | corpo binário (`application/octet-stream`), **até 16 KB** | 202 `{"ok":true,"job_id","bytes"}`; 409 ocupado; 413 acima do limite |

O upload local (`POST /api/print`) existe para testes com arquivos pequenos. Documentos reais devem ir pelo WebSocket, que não tem limite de tamanho total.

Exemplo com curl:

```
curl -u admin:senha -d "ip=192.168.0.50&format=pcl" http://PrintService-3A7F.local/api/print/test
curl -u admin:senha --data-binary @etiqueta.prn "http://PrintService-3A7F.local/api/print?ip=192.168.0.50&name=etiqueta"
curl -u admin:senha http://PrintService-3A7F.local/api/print/status
```

---

## 4. Limites e comportamento

| Item | Valor |
|---|---|
| Trabalhos simultâneos | 1 |
| Bloco máximo (`job.chunk`, binário) | 8 192 bytes |
| Buffer de saída | 32 768 bytes |
| Frame WebSocket máximo recebido | 15 KB |
| Timeout de conexão com a impressora | 3 s |
| Timeout sem progresso (dados ou escrita) | 20 s |
| Intervalo de `status` | 30 s (ajustável por `welcome`) |
| Intervalo de `printers` | 60 s (ajustável por `welcome`) |
| Reconexão WebSocket | a cada 10 s |
| Impressoras monitoradas | até 16 |
| URL / token | até 200 / 128 caracteres |

Indicação no LED durante impressão: ciano piscando (ver `status-led.md`).

## 5. Checklist para implementar o servidor

1. Aceitar a conexão WebSocket, validar `Authorization: Bearer` e associar a sessão ao `X-Device-Id`.
2. Responder ao `hello` com `welcome` (opcionalmente ajustando intervalos).
3. Persistir `status` e `printers` recebidos; considerar o dispositivo offline se ficar sem `status` por 3 intervalos.
4. Para imprimir: converter o documento no formato da impressora, enviar `job.start`, depois blocos de até 8 KB em base64, um por vez, aguardando `job.ack`. Encerrar com `last: true`. Tratar `job.done`/`job.error`.
5. Não enviar `job.start` enquanto outro trabalho do mesmo dispositivo não terminar.
6. Responder pings WebSocket.
