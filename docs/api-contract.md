# Contrato de API: PrintService ⇄ aplicativo externo

Protocolo **1**. Documento válido para o firmware **0.3.5**.

| Firmware | Mudanças no contrato |
|---|---|
| 0.3.0 | WebSocket, `hello`/`status`/`printers`, trabalhos em blocos, página de teste, REST local |
| 0.3.1 | `pdl` por impressora, `last_job` no `hello`, `data_timeout` no `welcome`, código `link_lost` |
| 0.3.2 | Transporte `ipp`, campos `ports` e `ipp_path`, formato `pdf` na página de teste, `detail` nos erros |
| 0.3.3 | Transporte `lpd`, campos `lpd_queue`, ordem automática pelas portas anunciadas, `POST /api/print/test` assíncrono |
| 0.3.5 | `job.ack` retido até o buffer ter espaço para outro bloco (contrapressão real; evita `overflow` com impressora lenta mesmo com o servidor aguardando cada ack); `device.restart` pelo WebSocket; capability `restart` no `hello` |
| 0.3.4 | Busca mDNS só sob demanda (`discover`, `discover.results`, `GET /api/discover`); nada entra na lista monitorada sem inclusão explícita; `refresh` passa a só resondar SNMP |

O PrintService é um dispositivo ESP32-C6 instalado na rede local das impressoras. Ele:

1. monitora por SNMP as impressoras que o operador ou o aplicativo incluíram, e busca impressoras na rede (mDNS) quando solicitado;
2. mantém uma conexão WebSocket **de saída** com o aplicativo, pela qual envia status e a lista de impressoras;
3. recebe trabalhos de impressão pelo WebSocket e os entrega à impressora por raw (9100), IPP (631) ou LPD (515);
4. imprime uma página de teste gerada localmente.

Há duas interfaces:

| Interface | Direção | Uso |
|---|---|---|
| **WebSocket** (seção 2) | dispositivo → aplicativo (o dispositivo é o cliente) | Canal permanente. É a interface do aplicativo. |
| **REST local** (seção 3) | qualquer cliente na LAN → dispositivo | Configuração, diagnóstico e página de teste na rede local. |

---

## 1. Provisionamento

O aplicativo provisiona. Para cada dispositivo ele gera:

| Item | Formato | Exemplo |
|---|---|---|
| URL WebSocket | `ws://` ou `wss://` + host + porta opcional + caminho, até 200 caracteres | `wss://app.exemplo.com/ws/devices/42/PrintService-3A7F` |
| Token | string opaca, até 128 caracteres, sem espaços | `eyJhbGciOi...` |

Recomendação: incluir o tenant e o identificador do dispositivo no caminho, `/ws/devices/{empresa_id}/{device_id}`, e emitir um token revogável por dispositivo ou por empresa. O dispositivo usa a URL literalmente. O servidor deve conferir que o `device_id` do caminho coincide com o cabeçalho `X-Device-Id`.

O operador insere URL e token no dispositivo por um destes meios:

- Portal web → aba **Dispositivo** → cartão **Servidor externo**.
- Menu serial (115200 bps, `menu` + Enter) → opção **s**.
- REST local: `POST /api/cloud` com `url` e `token`.

Ao salvar, o dispositivo grava em memória não volátil e conecta. A conexão só é tentada com o WiFi (modo STA) ativo. Após qualquer queda, reconecta a cada 10 s, sem backoff.

### Identificação do dispositivo

O nome é fixo e derivado do MAC: `PrintService-XXXX`, com os dois últimos bytes do MAC. Ele é o hostname mDNS (`PrintService-XXXX.local`), o SSID do AP de configuração e o valor de `X-Device-Id`.

---

## 2. WebSocket

### 2.1 Handshake HTTP

```
GET <caminho da URL> HTTP/1.1
Host: <host>
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Protocol: arduino
Authorization: Bearer <token>
X-Device-Id: PrintService-3A7F
X-Device-Mac: AA:BB:CC:DD:3A:7F
X-Firmware: 0.3.3
```

- O servidor **deve** validar `Authorization`. Ao rejeitar, responda `401`. O dispositivo registra o erro e tenta de novo em 10 s, então tokens devem ser revogados no servidor.
- O subprotocolo `arduino` vem da biblioteca; pode ser ignorado ou ecoado.
- **TLS**: com `wss://` o dispositivo **não valida o certificado** do servidor. O tráfego fica cifrado contra escuta, não contra um servidor impostor. Se isso for inaceitável, use rede privada ou solicite a versão com CA embutida.
- **Keep-alive**: ping WebSocket a cada 15 s; após 3 pongs perdidos o dispositivo derruba e reconecta. O servidor deve responder pings no nível do protocolo.

### 2.2 Formato das mensagens

Frames de **texto** com um objeto JSON. Campo obrigatório: `type`. Mensagens do dispositivo trazem `ts`, milissegundos desde o boot, útil só para ordenação.

Frames binários geram `error` com `code: "binary_unsupported"`. O maior frame aceito pelo dispositivo é **15 KB**, o que limita `job.chunk` a 8 KB de dados binários (10,9 KB em base64) mais o envelope.

### 2.3 Mensagens dispositivo → servidor

#### `hello`, ao conectar

```json
{
  "type": "hello", "protocol": "1",
  "device": "PrintService-3A7F", "mac": "AA:BB:CC:DD:3A:7F",
  "fw": "0.3.3", "ip": "192.168.0.23", "hostname": "PrintService-3A7F.local",
  "chunk_max": 8192, "buffer": 32768, "data_timeout": 20,
  "capabilities": ["raw9100", "ipp", "lpd", "snmp", "mdns", "test_page", "pdl", "restart"],
  "last_job": {"job_id": "7f3a", "state": "error", "bytes": 16384, "code": "link_lost", "reported": false},
  "ts": 12345
}
```

`last_job` aparece quando o último trabalho pedido pelo servidor já terminou. `reported: false` significa que o `job.done` ou `job.error` correspondente **não foi entregue** porque a conexão caiu; o servidor deve fechar o registro na fila com base nele (seção 2.7).

#### `status`, a cada 30 s ou a pedido

```json
{
  "type": "status", "uptime": 3600, "heap": 180000, "rssi": -61, "ip": "192.168.0.23",
  "ap": false, "printers": 3, "printers_online": 2, "printers_alert": 1,
  "printing": false, "ts": 12345
}
```

Com `printing: true` vem também `job_id`.

#### `printers`, a cada 60 s, ao conectar ou a pedido

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
      "pdl": ["PDF", "PS", "PCL", "PCLXL", "PJL", "URF"],
      "ports": {"raw": 9100, "ipp": 631, "lpd": 515},
      "ipp_path": "/ipp/print", "lpd_queue": "lp",
      "supplies": [
        {"desc": "Black Cartridge HP CF259A", "level": 62, "max": 100, "pct": 62, "class": 3, "type": 3}
      ]
    }]
  }
}
```

| Campo | Significado |
|---|---|
| `manual` / `mdns` | Toda impressora monitorada foi incluída explicitamente (`manual: true`). `mdns: true` indica que a inclusão partiu de um resultado da busca, com nome e portas vindos do anúncio |
| `online` | Respondeu SNMP na última sondagem |
| `alert` | Offline confirmado (3 sondagens seguidas sem resposta), `dev_status` 3 ou 5, algum erro, toner ≤ 10 % ou resíduo ≥ 90 % |
| `dev_status` | hrDeviceStatus: 1 desconhecido, 2 operando, 3 alerta, 4 teste, 5 parada |
| `prn_status` | hrPrinterStatus: 1 outro, 2 desconhecido, 3 ociosa, 4 imprimindo, 5 aquecendo |
| `errors` | Bits ativos de hrPrinterDetectedErrorState em texto, separados por vírgula; vazio = nenhum |
| `pages` | prtMarkerLifeCount, ou `null` |
| `last_ok` | Segundos desde a última resposta SNMP, ou `null` |
| `pdl` | Linguagens que a impressora declara aceitar: união do TXT `pdl` do IPP e da tabela `prtInterpreterLangFamily`. Tokens: `PDF`, `PS`, `PCL`, `PCLXL`, `PJL`, `HPGL`, `ESCP`, `TEXT`, `XPS`, `URF`, `PWG`, `JPEG`, `RAW`. Vazio = desconhecido |
| `ports` | Portas anunciadas via mDNS: `raw` (`_pdl-datastream`), `ipp` (`_ipp`), `lpd` (`_printer`). `null` = não anunciada. Anunciar `ipp` sem `raw` quase sempre significa 9100 fechada |
| `ipp_path` | Recurso IPP (TXT `rp` do `_ipp`), ex. `/ipp/print`; vazio = padrão `/ipp/print` |
| `lpd_queue` | Fila LPD (TXT `rp` do `_printer`), ex. `lp`, `PASSTHRU`; vazio = padrão `lp` |
| `supplies[].class` | 3 = consumido (toner, tinta, cilindro); 4 = receptáculo que enche (resíduo) |
| `supplies[].type` | Printer-MIB: 3 toner, 4 resíduo de toner, 5 tinta, 15 cilindro |
| `supplies[].level` / `max` | Valores SNMP; `level` -1 outro, -2 desconhecido, -3 "resta algum" |
| `supplies[].pct` | Percentual calculado, ou -1 |

**Escolha de formato e transporte pelo servidor.** O dispositivo não converte conteúdo. Use `pdl` para decidir o formato: com `PDF` na lista, envie o PDF; sem `PDF`, converta antes do `job.start` (Ghostscript: `-sDEVICE=pxlmono` ou `pxlcolor` para PCL-XL, `ljet4` para PCL5, `ps2write` para PostScript). Use `ports` para decidir o transporte, ou deixe `auto`. Lista `pdl` vazia: tente PDF e confirme com uma página de teste.

#### `discover.results`, ao fim de uma busca pedida por `discover` ou em resposta a `get_discovery`

```json
{
  "type": "discover.results", "ts": 12345,
  "data": {
    "running": false, "count": 2,
    "found": [
      {"ip": "192.168.1.120", "name": "EPSON L6270 Series", "host": "EPSON431EC2", "model": "EPSON L6270 Series",
       "ports": {"raw": 9100, "ipp": 631, "lpd": 515}, "ipp_path": "/ipp/print", "lpd_queue": "PASSTHRU", "added": false},
      {"ip": "192.168.1.50", "name": "HP LaserJet Pro M404", "host": "NPI3A7F2C", "model": "",
       "ports": {"raw": 9100, "ipp": 631, "lpd": null}, "ipp_path": "/ipp/print", "lpd_queue": "", "added": true}
    ]
  }
}
```

A lista é leve por desenho: só nome, host, modelo (TXT `ty`), portas e caminhos anunciados, até 32 dispositivos. Ela **não** é monitorada; `added` indica se o IP já está na lista monitorada. Os resultados são liberados 5 min após a busca ou quando o operador fecha o modal no portal. Impressoras em outra VLAN não aparecem, porque multicast raramente cruza VLANs; inclua-as por IP com `printer.add`.

#### Respostas a trabalhos: `job.ready`, `job.ack`, `job.done`, `job.error`, `job.status`

Descritas na seção 2.5.

#### `ack`, `error`, `pong`

`ack` confirma `refresh`, `printer.add` e `printer.remove`, com `reply_to`. `error` traz `code`, `message` opcional e `reply_to` quando aplicável. `pong` responde ao `ping` de aplicação.

### 2.4 Mensagens servidor → dispositivo

| `type` | Campos | Efeito |
|---|---|---|
| `welcome` | opcionais `status_interval` (s, 5–600), `printers_interval` (s, 10–3600), `data_timeout` (s, 10–120) | Ajusta intervalos e o tempo de espera por blocos. O dispositivo responde com `status` e `printers`. Envie logo após o `hello`. |
| `ping` | — | Responde `pong`. |
| `get_status` | — | Responde `status`. |
| `get_printers` | — | Responde `printers`. |
| `get_job` | — | Responde `job.status` com `data` igual ao objeto de `GET /api/print/status`. |
| `refresh` | — | Força sondagem SNMP imediata de todas as impressoras cadastradas. Responde `ack`. |
| `discover` | — | Inicia a busca mDNS (cerca de 9 s). Responde `ack` e, ao terminar, envia `discover.results`. `error` com `code` `sem rede WiFi`, `busca em andamento` ou `mDNS indisponivel`. |
| `get_discovery` | — | Responde `discover.results` com a lista atual (pode estar vazia ou em andamento). |
| `printer.add` | `printer` (IP) | Inclui a impressora na lista monitorada e persiste. Se o IP estiver na última busca, aproveita nome e portas anunciadas. Responde `ack`, ou `error` com `code` `IP invalido`, `ja cadastrada` ou `limite de impressoras atingido`. |
| `printer.remove` | `printer` (IP) | Remove. Responde `ack` ou `error` (`not_found`). |
| `device.restart` | opcional `force` | Reinicia o dispositivo em 1,5 s. Responde `ack` (com `delay_ms`) e o WebSocket cai; o `hello` seguinte marca a volta. Com trabalho em curso responde `error` (`busy`), salvo `force: true`, que cancela o trabalho (`job.error` com `code` `restart`) antes de reiniciar. Não há reset de fábrica remoto. |
| `print_test` | `printer`; opcionais `transport`, `port`, `ipp_path`, `lpd_queue`, `format` (`auto`, `pdf`, `pcl`, `text`, `ps`), `job_id` | Imprime a página de teste (seção 2.6). Responde `job.ready` com `test: true`, depois `job.done` ou `job.error`. |
| `job.start` | `job_id`, `printer`; opcionais `transport`, `port`, `ipp_path`, `lpd_queue`, `format` (MIME, para IPP), `size` (obrigatório para LPD), `name` | Conecta à impressora. Responde `job.ready` ou `job.error`. |
| `job.chunk` | `job_id`, `seq`, `data` (base64), `last` | Entrega um bloco. Responde `job.ack` assim que houver espaço para outro bloco inteiro (pode demorar com impressora lenta). |
| `job.cancel` | `job_id` | Aborta e fecha a conexão com a impressora. Resulta em `job.error` com `code: "canceled"`. |

Tipo desconhecido recebe `error` com `code: "unknown_type"`.

### 2.5 Trabalho de impressão

O dispositivo executa **um trabalho por vez**. Com um ativo, `job.start` responde `job.error` com `code: "busy"`.

O conteúdo é opaco: envie exatamente o que a impressora aceita (PCL, PostScript, PDF, ESC/POS, PWG). A conversão é responsabilidade do aplicativo.

#### Transporte

| `transport` | Como | Quando usar |
|---|---|---|
| `raw` | TCP na porta 9100 (JetDirect/AppSocket), bytes como estão | Impressoras corporativas; qualquer linguagem |
| `ipp` | IPP/1.1 `Print-Job` sobre HTTP na porta 631, corpo em chunked, `document-format` = `format` | Impressoras domésticas e AirPrint, que não abrem a 9100; PDF, PWG ou URF |
| `lpd` | LPR/LPD (RFC 1179) na porta 515, fila `lpd_queue`. **Exige `size` exato**: o protocolo envia o tamanho antes dos dados | Modelos antigos que só expõem a 515; qualquer linguagem |
| `auto` (padrão) | Tenta as portas anunciadas em `ports`, na ordem raw, ipp, lpd, depois as não anunciadas; 3 s por porta | Sempre que não houver motivo para forçar |

`port`, `ipp_path` e `lpd_queue` no `job.start` sobrepõem o que o mDNS anunciou. Sem anúncio e sem override valem 9100, 631 e 515, `/ipp/print` e `lp`.

Com IPP, `format` é o MIME do documento: `application/pdf`, `image/pwg-raster`, `image/urf`, `application/postscript`, `application/vnd.hp-PCL`. Se omitido, o dispositivo usa `application/pdf` quando `name` termina em `.pdf`, senão `application/octet-stream`. A impressora valida o formato e pode rejeitar: `job.error` com `printer_rejected` e o status IPP em `detail`.

Filas LPD comuns quando o mDNS não informa: `lp` (maioria), `PASSTHRU` (Xerox, Epson), `BINARY_P1` (Brother), `raw` ou `auto` (HP), `print` (Lexmark). Fila inexistente resulta em `printer_rejected` com `detail` "LPD ack N em receive-job".

#### Fluxo

```
servidor                                    dispositivo                         impressora
   |-- job.start {job_id, printer, size} --->|                                     |
   |                                         |---- TCP connect (porta escolhida) ->|
   |<-- job.ready {transport, port, ...} ----|                                     |
   |-- job.chunk {seq:0, data, last:false} ->|                                     |
   |                                         |---- cabecalho de protocolo + bytes >|
   |<-- job.ack {seq:0, written, ...} -------|                                     |
   |-- job.chunk {seq:1, ...} -------------->|                                     |
   |<-- job.ack {seq:1} ---------------------|                                     |
   |   ...                                   |                                     |
   |-- job.chunk {seq:N, last:true} -------->|                                     |
   |<-- job.ack {seq:N, last:true} ----------|                                     |
   |                                         |---- fim: raw fecha; ipp/lpd aguardam confirmacao
   |<-- job.done {bytes, duration_ms} -------|                                     |
```

Os cabeçalhos de protocolo (HTTP+IPP ou handshake LPD) são enviados no primeiro bloco, não no `job.start`.

Regras de controle de fluxo:

1. **Aguarde `job.ack` antes do próximo `job.chunk`.** O buffer tem 32 KB. O `job.ack` só é enviado quando o buffer volta a ter espaço para um bloco inteiro (`buffer_free` ≥ `chunk_max`); com a impressora lenta, o ack pode demorar até que ela consuma dados. Isso é a contrapressão do protocolo: quem espera o ack nunca recebe `overflow`. Enviar sem esperar pode gerar `overflow` e cancelar o trabalho. Enquanto o ack está retido, o relógio de `data_timeout` não corre contra o servidor, porque há dados no buffer; só `write_timeout` (impressora parada 20 s) encerra o trabalho.
2. Cada bloco tem no máximo `chunk_max` bytes **binários** (8192). Maiores geram `chunk_too_large` e cancelam.
3. `seq` começa em 0 e volta no `job.ack`. O dispositivo não reordena.
4. O último bloco leva `last: true`. `data` vazio com `last: true` é válido para encerrar.
5. `data_timeout` segundos sem novos blocos (padrão 20, ajustável no `welcome`) cancelam com `data_timeout`. Impressora sem consumir dados por 20 s: `write_timeout`.
6. **Gere o conteúdo inteiro antes do `job.start`.** O relógio de `data_timeout` corre entre blocos; renderizar o documento depois de abrir o trabalho é a forma mais comum de estourá-lo. Para LPD o tamanho total precisa ser conhecido de antemão.

#### Exemplos

```json
{"type": "job.ready", "job_id": "7f3a", "transport": "ipp", "port": 631, "format": "application/pdf",
 "chunk_max": 8192, "buffer_free": 32768, "ts": 120}

{"type": "job.chunk", "job_id": "7f3a", "seq": 0, "data": "JVBERi0xLjQK...", "last": false}

{"type": "job.ack", "job_id": "7f3a", "seq": 0, "received": 8192, "written": 5840,
 "buffer_free": 30416, "last": false, "ts": 190}

{"type": "job.done", "job_id": "7f3a", "bytes": 61234, "duration_ms": 2410,
 "transport": "ipp", "port": 631, "detail": "IPP 0x0000", "ts": 3200}

{"type": "job.error", "job_id": "7f3a", "code": "connect_failed",
 "message": "9100: conexao recusada (porta fechada na impressora) [errno 111]; 631: sem resposta em 3 s (porta filtrada, host desligado ou outra rede) [errno 119]; 515: conexao recusada (porta fechada na impressora) [errno 111]",
 "ts": 9301}
```

`job.done` e `job.error` trazem `transport`, `port` e, quando houver, `detail` com diagnóstico em texto: tentativas de conexão que falharam antes da porta escolhida, status HTTP/IPP, ack LPD.

#### Códigos de erro

| `code` | Quando |
|---|---|
| `busy` | Já há trabalho ativo |
| `no_network` | Dispositivo sem WiFi STA |
| `bad_printer` | IP inválido em `printer` |
| `missing_job_id` | `job.start` sem `job_id` |
| `connect_failed` | Nenhuma porta aceitou TCP em 3 s. `detail` lista cada tentativa: "conexão recusada" = porta fechada na impressora; "sem resposta" = filtro, host desligado ou outra rede |
| `write_failed` | Falha ao enviar cabeçalho IPP ou etapa do handshake LPD |
| `printer_rejected` | IPP: `HTTP 4xx/5xx` ou status `IPP 0x04xx/0x05xx` (`0x040A` = formato não suportado). LPD: ack diferente de 0; `detail` informa a etapa |
| `response_timeout` | IPP: 15 s sem resposta após o último bloco. LPD: 15 s sem ack em alguma etapa |
| `size_required` | LPD sem `size` |
| `size_mismatch` | LPD: total enviado diferente de `size` |
| `no_such_job` | `job.chunk` ou `job.cancel` para `job_id` que não está ativo |
| `chunk_too_large` | Bloco decodificado maior que 8192 bytes |
| `bad_base64` | `data` inválido |
| `overflow` | Buffer cheio: o servidor enviou `job.chunk` sem aguardar o `job.ack` anterior (o ack só sai quando cabe outro bloco) |
| `data_timeout` | Sem novos blocos pelo tempo configurado |
| `write_timeout` | 20 s sem a impressora consumir dados |
| `connection_closed` | Impressora fechou a conexão antes do fim |
| `canceled` | `job.cancel` recebido |
| `restart` | `device.restart` com `force: true` durante o trabalho |
| `link_lost` | WebSocket caiu durante o trabalho; o dispositivo abortou. Relatado só em `last_job` no próximo `hello` |

Após `job.done` ou `job.error` o dispositivo está livre para o próximo `job.start`.

### 2.6 Página de teste

`print_test` gera no dispositivo uma página com nome do dispositivo, firmware, MAC, rede, IP e porta usados, modelo, nome, local, linguagens e contador vindos do SNMP, um teste de caracteres e uma linha de confirmação.

| `format` | Conteúdo | Indicado para |
|---|---|---|
| `auto` (padrão) | `pdf` se a conexão foi por IPP, senão `pcl` | Uso geral |
| `pdf` | PDF 1.4 de uma página A4, Courier | IPP; impressoras com PDF direto |
| `pcl` | `ESC E` + texto + `FF` + `ESC E` | Laser e multifuncionais via 9100 ou LPD; a maioria aceita texto após o reset |
| `text` | Texto puro + `FF` | Matriciais, térmicas e impressoras de texto |
| `ps` | PostScript de uma página | Impressoras só PostScript |

Via raw ou LPD a impressora não valida o conteúdo: impressoras "GDI/host-based" sem interpretador aceitam os bytes e não imprimem nada; `job.done` indica apenas entrega. Via IPP a impressora valida o formato e devolve erro quando não o suporta, o que torna a página em PDF por IPP a verificação mais confiável.

### 2.7 Quedas de conexão e reconciliação

O dispositivo reconecta a cada 10 s após qualquer queda: timeout de requisição do servidor, deploy, perda de WiFi.

- Um trabalho em curso na queda é abortado imediatamente com `link_lost`. Parte do documento pode ter chegado à impressora; o servidor decide se reenvia.
- O `job.error` dessa queda não é entregue. No `hello` seguinte, `last_job` traz `job_id`, estado e `reported: false`; o servidor deve fechar o registro como erro e, se for o caso, reenfileirar. Com `reported: true` nada precisa ser feito.
- A fila deve ser lida do banco, não da memória do processo: com várias instâncias, o job pode nascer em uma e o dispositivo estar conectado em outra.
- Um dispositivo conectado mantém a instância viva. Em Cloud Run, use `min-instances` ≥ 1 e timeout de requisição de 3600 s, ou um gateway WebSocket dedicado que consulte a mesma fila.

### 2.8 Roteamento junto a outros agentes

Impressoras reportadas pelo dispositivo pertencem a ele para fins de entrega: registre-as com o identificador do dispositivo como agente e torne-as elegíveis **apenas** para ele. Sem esse filtro, um agente de impressão convencional na mesma rede pode disputar a mesma fila e enviar um formato que a impressora não aceita.

---

## 3. REST local (LAN)

Base `http://<ip-do-dispositivo>/` ou `http://PrintService-XXXX.local/`. Com senha de portal, HTTP Basic com usuário `admin`. Corpo de `POST` em `application/x-www-form-urlencoded`, salvo indicação.

O dispositivo também serve esta referência em `GET /docs` (HTML) e a descrição OpenAPI 3.0 em `GET /docs/openapi.json`.

| Método e rota | Parâmetros | Resposta |
|---|---|---|
| `GET /api/status` | — | Rede, AP, contadores de impressoras, `printing`, objeto `cloud` (igual a `GET /api/cloud`) |
| `GET /api/printers` | — | Mesmo objeto de `printers.data` (seção 2.3) |
| `POST /api/printers` | `ip` | Inclui e persiste (aproveita a busca se o IP estiver nela). `{"ok":true}` ou 400 `{"error"}` |
| `POST /api/printers/remove` | `ip` | `{"ok":true}` ou 404 |
| `POST /api/printers/refresh` | — | Sondagem SNMP imediata de todas. `{"ok":true}` |
| `POST /api/discover/start` | — | Inicia a busca mDNS. 202 `{"ok":true,"running":true}`; 409 se sem rede ou já em curso |
| `GET /api/discover` | — | Mesmo objeto de `discover.results.data` |
| `POST /api/discover/clear` | — | Libera a lista de resultados |
| `GET /api/cloud` | — | `{"enabled","url","token_set","state","connected","last_error","connected_for","reconnects"}` |
| `POST /api/cloud` | `url` (vazio = desativar), `token` (vazio = manter, espaço = remover) | Objeto de `GET /api/cloud` com o novo estado |
| `GET /api/print/status` | — | `{"state","busy","buffer_free","chunk_max","job":{"id","name","source","printer","port","transport","format","queue","expected","received","written","error","detail","duration_ms"}}`. `state`: `idle`, `connecting`, `streaming`, `finishing`, `done`, `error` |
| `POST /api/print/test` | `ip`; opcionais `transport`, `port`, `queue`, `format` | 202 `{"ok":true,"queued":true,"job_id"}`; 409 se ocupado. A conexão acontece em seguida no loop principal: acompanhe por `GET /api/print/status` |
| `POST /api/print/cancel` | — | `{"ok":true}` ou 404 |
| `POST /api/print?ip=&transport=&port=&queue=&format=&name=` | corpo binário, **até 16 KB**; `format` é o MIME para IPP | 202 `{"ok":true,"job_id","bytes"}`; 409 ocupado; 413 acima do limite |

O upload local serve para testes com arquivos pequenos. Documentos reais devem ir pelo WebSocket, sem limite de tamanho total.

```
curl -u admin:senha -d "ip=192.168.0.50&transport=auto&format=auto" http://PrintService-3A7F.local/api/print/test
curl -u admin:senha http://PrintService-3A7F.local/api/print/status
curl -u admin:senha --data-binary @etiqueta.prn "http://PrintService-3A7F.local/api/print?ip=192.168.0.50&transport=raw&name=etiqueta"
```

---

## 4. Limites

| Item | Valor |
|---|---|
| Trabalhos simultâneos | 1 |
| Bloco máximo em `job.chunk`, binário | 8 192 bytes |
| Buffer de saída | 32 768 bytes |
| Frame WebSocket máximo recebido | 15 KB |
| Timeout de conexão com a impressora | 3 s por porta tentada (até 9 s em `auto`) |
| Resposta IPP ou ack LPD | 15 s |
| Sem receber blocos (`data_timeout`) | 20 s, ajustável de 10 a 120 s no `welcome` |
| Impressora sem consumir dados | 20 s |
| Intervalo de `status` / `printers` | 30 s / 60 s, ajustáveis no `welcome` |
| Reconexão WebSocket | a cada 10 s |
| Impressoras monitoradas | até 16 |
| URL / token | até 200 / 128 caracteres |

Durante um trabalho o LED do dispositivo pisca em ciano (ver `status-led.md`).

## 5. Checklist para o servidor

1. Aceitar a conexão WebSocket, validar `Authorization: Bearer` contra o tenant do caminho e conferir `X-Device-Id`.
2. Ler `last_job` do `hello`; com `reported: false`, fechar o job na fila como erro. Responder `welcome`, ajustando `data_timeout` se a geração de conteúdo for lenta.
3. Persistir `status` e `printers`, incluindo `pdl`, `ports`, `ipp_path` e `lpd_queue`. Considerar o dispositivo offline após 3 intervalos sem `status`. Registrar as impressoras como pertencentes ao dispositivo. Para cadastrar impressoras a partir do aplicativo, use `discover` e depois `printer.add` com o IP escolhido; a busca sozinha não inclui nada.
4. Para imprimir: gerar o documento inteiro; escolher formato pelo `pdl` e transporte pelas `ports`, ou deixar `auto`; enviar `job.start` com `size`; enviar blocos de até 8 KB em base64, um por vez, aguardando `job.ack`; encerrar com `last: true`; tratar `job.done` e `job.error`.
5. Não enviar `job.start` enquanto outro trabalho do mesmo dispositivo não terminar.
6. Ler a fila do banco periodicamente para o dispositivo conectado, não da memória.
7. Responder pings WebSocket.
