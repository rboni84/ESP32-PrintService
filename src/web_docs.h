#pragma once
#include <Arduino.h>

// Documentacao da API servida pelo proprio dispositivo em /docs (HTML) e /docs/openapi.json.
// Fonte de verdade completa: docs/api-contract.md no repositorio. Manter as duas em sincronia
// ao alterar rotas ou mensagens.

static const char PAGE_DOCS[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PrintService · API</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--fg:#1d2733;--muted:#6b7785;--acc:#1f6feb;--bd:#dde3ea;--code:#eef2f6}
@media(prefers-color-scheme:dark){:root{--bg:#0f141a;--card:#161d26;--fg:#e6edf3;--muted:#8b98a5;--acc:#4c8dff;--bd:#2b3543;--code:#0d1218}}
*{box-sizing:border-box}body{margin:0;font:15px/1.55 system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{background:var(--acc);color:#fff;padding:14px 16px}header h1{margin:0;font-size:18px}header a{color:#fff;opacity:.9;font-size:13px}
main{max-width:900px;margin:0 auto;padding:16px}
nav.toc{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:12px}nav.toc a{font-size:13px;color:var(--acc);text-decoration:none;border:1px solid var(--bd);border-radius:999px;padding:3px 10px;background:var(--card)}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:16px;margin-bottom:16px}
h2{margin:0 0 10px;font-size:17px}h3{margin:16px 0 6px;font-size:15px}
table{width:100%;border-collapse:collapse;font-size:13.5px}th,td{text-align:left;vertical-align:top;padding:6px 8px;border-bottom:1px solid var(--bd)}th{color:var(--muted);font-weight:600}
code,pre{font:12.5px/1.5 ui-monospace,Consolas,monospace;background:var(--code);border-radius:6px}code{padding:1px 5px}pre{padding:10px;overflow:auto;margin:8px 0}
.m{display:inline-block;min-width:44px;text-align:center;font-weight:700;font-size:11px;color:#fff;border-radius:4px;padding:2px 6px;margin-right:4px}.get{background:#1a7f37}.post{background:#1f6feb}
.hint{color:var(--muted);font-size:13px}.dev{font-weight:600}
</style></head><body>
<header><h1>PrintService · Documentação da API <small id="fw"></small></h1><a href="/">← voltar ao portal</a> · <a href="/docs/openapi.json">openapi.json</a></header>
<main>
<nav class="toc"><a href="#geral">Geral</a><a href="#status">Status e rede</a><a href="#printers">Impressoras</a><a href="#discover">Busca mDNS</a><a href="#print">Impressão</a><a href="#cloud">Servidor externo</a><a href="#ws">WebSocket</a><a href="#erros">Códigos de erro</a></nav>

<section class="card" id="geral"><h2>Geral</h2>
<p>Base: <code id="base">http://…/</code>. Todas as rotas <code>/api/*</code> respondem JSON com <code>Cache-Control: no-store</code>. Quando há senha de portal, use HTTP Basic com usuário <code>admin</code>. Corpo de <code>POST</code> em <code>application/x-www-form-urlencoded</code>, salvo indicação. Erros retornam <code>{"error":"mensagem"}</code> com status 4xx/5xx.</p>
<p class="hint">Esta página descreve a API REST local (LAN). O protocolo WebSocket com o aplicativo externo está resumido ao final; o contrato completo é o arquivo <code>docs/api-contract.md</code> do repositório.</p>
<pre>curl -u admin:senha <span class="dev">http://PrintService-XXXX.local</span>/api/status</pre>
</section>

<section class="card" id="status"><h2>Status e rede</h2>
<table><tr><th>Rota</th><th>Parâmetros</th><th>Resposta</th></tr>
<tr><td><span class="m get">GET</span><code>/api/status</code></td><td>—</td><td><code>fw, devname, ssid, sta_connected, sta_ip, rssi, ap, ap_ssid, ap_secure, ap_ip, ap_clients, mac, uptime, heap, admin_auth, community, printers, printers_online, printers_alert, printing, cloud{…}</code></td></tr>
<tr><td><span class="m get">GET</span><code>/api/scan</code></td><td><code>refresh</code> (opcional) força novo scan</td><td><code>{"status":"scanning"}</code> ou <code>{"status":"done","networks":[{ssid,rssi,ch,secure}]}</code>. Repita a chamada até <code>done</code>.</td></tr>
<tr><td><span class="m post">POST</span><code>/api/wifi</code></td><td><code>ssid</code>, <code>pass</code> (vazio = rede aberta; senão ≥ 8)</td><td><code>{"ok":true,"restart":true}</code>. O dispositivo reinicia em 1,5 s.</td></tr>
<tr><td><span class="m post">POST</span><code>/api/device</code></td><td><code>community</code> (SNMP, padrão <code>public</code>), <code>appass</code> (senha do AP, ≥ 8), <code>adminpw</code> (senha do portal, ≥ 4). Senhas: vazio = manter, espaço = remover</td><td><code>{"ok":true,"restart_required":bool}</code>. Nome do dispositivo é fixo (derivado do MAC).</td></tr>
<tr><td><span class="m post">POST</span><code>/api/restart</code></td><td>—</td><td><code>{"ok":true}</code></td></tr>
<tr><td><span class="m post">POST</span><code>/api/reset</code></td><td>—</td><td><code>{"ok":true}</code>. Apaga WiFi, senhas, community, impressoras e servidor externo; reinicia em modo AP.</td></tr>
</table></section>

<section class="card" id="printers"><h2>Impressoras</h2>
<table><tr><th>Rota</th><th>Parâmetros</th><th>Resposta</th></tr>
<tr><td><span class="m get">GET</span><code>/api/printers</code></td><td>—</td><td><code>{"count","online","alerts","sta","printers":[…]}</code></td></tr>
<tr><td><span class="m post">POST</span><code>/api/printers</code></td><td><code>ip</code></td><td>Inclui e persiste. Se o IP estiver na última busca mDNS, aproveita nome e portas. <code>{"ok":true}</code>; 400 <code>IP invalido</code>, <code>ja cadastrada</code>, <code>limite de impressoras atingido</code></td></tr>
<tr><td><span class="m post">POST</span><code>/api/printers/remove</code></td><td><code>ip</code></td><td><code>{"ok":true}</code> ou 404</td></tr>
<tr><td><span class="m post">POST</span><code>/api/printers/refresh</code></td><td>—</td><td>Sondagem SNMP imediata de todas. <code>{"ok":true}</code></td></tr>
</table>
<h3>Objeto impressora</h3>
<pre>{"ip":"192.168.1.120","host":"EPSON431EC2","name":"EPSON L6270 Series","model":"EPSON L6270 Series","location":"",
 "manual":true,"mdns":true,"online":true,"alert":false,
 "dev_status":2,"dev_status_text":"operando","prn_status":3,"prn_status_text":"ociosa",
 "errors":"","pages":12034,"last_ok":8,"snmp_version":"v2c",
 "pdl":["PDF","PS","PCL","URF"],"ports":{"raw":9100,"ipp":631,"lpd":515},"ipp_path":"/ipp/print","lpd_queue":"PASSTHRU",
 "supplies":[{"desc":"Black","level":62,"max":100,"pct":62,"class":3,"type":5}]}</pre>
<table><tr><th>Campo</th><th>Significado</th></tr>
<tr><td><code>dev_status</code></td><td>hrDeviceStatus: 1 desconhecido, 2 operando, 3 alerta, 4 teste, 5 parada</td></tr>
<tr><td><code>prn_status</code></td><td>hrPrinterStatus: 1 outro, 2 desconhecido, 3 ociosa, 4 imprimindo, 5 aquecendo</td></tr>
<tr><td><code>errors</code></td><td>Bits de hrPrinterDetectedErrorState em texto (pouco papel, sem toner, tampa aberta, atolado…); vazio = nenhum</td></tr>
<tr><td><code>alert</code></td><td>Offline confirmado (3 sondagens), dev_status 3/5, algum erro, toner ≤ 10 % ou resíduo ≥ 90 %</td></tr>
<tr><td><code>pdl</code></td><td>Linguagens aceitas (TXT <code>pdl</code> do IPP + prtInterpreterLangFamily): PDF, PS, PCL, PCLXL, PJL, URF, PWG, TEXT…</td></tr>
<tr><td><code>ports</code></td><td>Portas anunciadas via mDNS ao incluir (<code>null</code> = não anunciada)</td></tr>
<tr><td><code>supplies[].class</code></td><td>3 consumido (toner, tinta, cilindro); 4 receptáculo que enche (resíduo). <code>pct</code> -1 = desconhecido</td></tr>
</table></section>

<section class="card" id="discover"><h2>Busca mDNS (sob demanda)</h2>
<p class="hint">A busca só roda quando pedida e não inclui nada sozinha. Dura ~9 s (serviços <code>_ipp</code>, <code>_printer</code>, <code>_pdl-datastream</code>), lista até 32 dispositivos e é liberada 5 min depois ou em <code>clear</code>. Só enxerga a mesma VLAN.</p>
<table><tr><th>Rota</th><th>Parâmetros</th><th>Resposta</th></tr>
<tr><td><span class="m post">POST</span><code>/api/discover/start</code></td><td>—</td><td>202 <code>{"ok":true,"running":true}</code>; 409 <code>sem rede WiFi</code>, <code>busca em andamento</code>, <code>mDNS indisponivel</code></td></tr>
<tr><td><span class="m get">GET</span><code>/api/discover</code></td><td>—</td><td><code>{"running":bool,"count":n,"found":[{ip,name,host,model,ports{raw,ipp,lpd},ipp_path,lpd_queue,added}]}</code></td></tr>
<tr><td><span class="m post">POST</span><code>/api/discover/clear</code></td><td>—</td><td><code>{"ok":true}</code></td></tr>
</table></section>

<section class="card" id="print"><h2>Impressão</h2>
<p class="hint">Um trabalho por vez. Transportes: <code>raw</code> (TCP 9100), <code>ipp</code> (IPP/1.1 Print-Job na 631), <code>lpd</code> (RFC 1179 na 515, exige tamanho conhecido) e <code>auto</code> (portas anunciadas primeiro, 3 s cada). O dispositivo não converte conteúdo.</p>
<table><tr><th>Rota</th><th>Parâmetros</th><th>Resposta</th></tr>
<tr><td><span class="m get">GET</span><code>/api/print/status</code></td><td>—</td><td><code>{"state","busy","buffer_free","chunk_max","job":{id,name,source,printer,port,transport,format,queue,expected,received,written,error,detail,duration_ms}}</code>. <code>state</code>: idle, connecting, streaming, finishing, done, error</td></tr>
<tr><td><span class="m post">POST</span><code>/api/print/test</code></td><td><code>ip</code>; opcionais <code>transport</code> (auto/raw/ipp/lpd), <code>port</code>, <code>queue</code> (fila LPD), <code>format</code> (auto/pdf/pcl/text/ps)</td><td>202 <code>{"ok":true,"queued":true,"job_id"}</code>; 409 ocupado. A conexão acontece no loop principal: acompanhe por <code>/api/print/status</code></td></tr>
<tr><td><span class="m post">POST</span><code>/api/print/cancel</code></td><td>—</td><td><code>{"ok":true}</code> ou 404</td></tr>
<tr><td><span class="m post">POST</span><code>/api/print?ip=&amp;transport=&amp;port=&amp;queue=&amp;format=&amp;name=</code></td><td>Corpo binário (<code>application/octet-stream</code>), até 16 KB. <code>format</code> = MIME para IPP</td><td>202 <code>{"ok":true,"job_id","bytes"}</code>; 409 ocupado; 413 acima do limite. Documentos maiores: via WebSocket</td></tr>
</table>
<pre>curl -u admin:senha -d "ip=192.168.1.120&amp;transport=auto&amp;format=auto" <span class="dev">http://PrintService-XXXX.local</span>/api/print/test
curl -u admin:senha <span class="dev">http://PrintService-XXXX.local</span>/api/print/status
curl -u admin:senha --data-binary @etiqueta.prn "<span class="dev">http://PrintService-XXXX.local</span>/api/print?ip=192.168.1.120&amp;transport=raw&amp;name=etiqueta"</pre>
</section>

<section class="card" id="cloud"><h2>Servidor externo</h2>
<table><tr><th>Rota</th><th>Parâmetros</th><th>Resposta</th></tr>
<tr><td><span class="m get">GET</span><code>/api/cloud</code></td><td>—</td><td><code>{"enabled","url","token_set","state","connected","last_error","connected_for","reconnects"}</code>. <code>state</code>: desativado, conectando, conectado, erro</td></tr>
<tr><td><span class="m post">POST</span><code>/api/cloud</code></td><td><code>url</code> (ws:// ou wss://, vazio = desativar, ≤ 200), <code>token</code> (vazio = manter, espaço = remover, ≤ 128)</td><td>Objeto de <code>GET /api/cloud</code> já reconectando</td></tr>
</table></section>

<section class="card" id="ws"><h2>WebSocket com o aplicativo (resumo)</h2>
<p>O dispositivo é o <b>cliente</b>: conecta na URL provisionada com <code>Authorization: Bearer &lt;token&gt;</code>, <code>X-Device-Id</code>, <code>X-Device-Mac</code> e <code>X-Firmware</code>. Mensagens são frames de texto JSON com campo <code>type</code>. Reconecta a cada 10 s; ping WebSocket a cada 15 s.</p>
<table><tr><th>Dispositivo → servidor</th><th>Servidor → dispositivo</th></tr>
<tr><td><code>hello</code> (ao conectar; traz <code>chunk_max</code>, <code>data_timeout</code>, <code>capabilities</code>, <code>last_job</code>)<br><code>status</code> (30 s) · <code>printers</code> (60 s) · <code>discover.results</code><br><code>job.ready</code> · <code>job.ack</code> · <code>job.done</code> · <code>job.error</code> · <code>job.status</code><br><code>ack</code> · <code>error</code> · <code>pong</code></td>
<td><code>welcome</code> {status_interval, printers_interval, data_timeout}<br><code>ping</code> · <code>get_status</code> · <code>get_printers</code> · <code>get_job</code> · <code>refresh</code><br><code>discover</code> · <code>get_discovery</code> · <code>printer.add</code> · <code>printer.remove</code> · <code>device.restart</code><br><code>print_test</code> {printer, transport, format…}<br><code>job.start</code> {job_id, printer, transport, port, ipp_path, lpd_queue, format, size, name}<br><code>job.chunk</code> {job_id, seq, data base64 ≤ 8 KB, last} → aguardar <code>job.ack</code><br><code>job.cancel</code> {job_id}</td></tr></table>
<p class="hint">Fluxo: <code>job.start</code> → <code>job.ready</code> → N × (<code>job.chunk</code> → <code>job.ack</code>) → <code>job.done</code> ou <code>job.error</code>. Gere o documento inteiro antes do <code>job.start</code>; para LPD, <code>size</code> é obrigatório. Detalhes, exemplos e regras de reconciliação (<code>last_job</code>, <code>link_lost</code>) em <code>docs/api-contract.md</code>.</p>
</section>

<section class="card" id="erros"><h2>Códigos de erro de impressão</h2>
<table><tr><th>Código</th><th>Quando</th></tr>
<tr><td><code>busy</code></td><td>Já há trabalho ativo</td></tr>
<tr><td><code>no_network</code></td><td>Sem WiFi STA</td></tr>
<tr><td><code>connect_failed</code></td><td>Nenhuma porta aceitou TCP em 3 s. <code>detail</code> lista cada tentativa: "conexão recusada" = porta fechada; "sem resposta" = filtro, host desligado ou outra rede</td></tr>
<tr><td><code>printer_rejected</code></td><td>IPP: HTTP 4xx/5xx ou status 0x04xx/0x05xx (0x040A = formato não suportado). LPD: ack ≠ 0 (fila inexistente em receive-job)</td></tr>
<tr><td><code>response_timeout</code></td><td>15 s sem resposta IPP ou sem ack LPD</td></tr>
<tr><td><code>size_required</code> / <code>size_mismatch</code></td><td>LPD sem tamanho, ou total enviado diferente do anunciado</td></tr>
<tr><td><code>write_failed</code> / <code>write_timeout</code></td><td>Falha ao enviar cabeçalho, ou impressora sem consumir dados por 20 s</td></tr>
<tr><td><code>data_timeout</code></td><td>Solicitante parou de enviar blocos (padrão 20 s)</td></tr>
<tr><td><code>overflow</code> / <code>chunk_too_large</code> / <code>bad_base64</code></td><td>Controle de fluxo do WebSocket violado</td></tr>
<tr><td><code>connection_closed</code> / <code>canceled</code> / <code>link_lost</code></td><td>Impressora fechou; cancelado; WebSocket caiu durante o trabalho</td></tr>
</table></section>
<p class="hint">PrintService · <span id="dev"></span></p>
</main>
<script>
fetch('/api/status').then(r=>r.json()).then(j=>{const h=location.origin+'/';document.getElementById('base').textContent=h;document.getElementById('fw').textContent='v'+j.fw;document.getElementById('dev').textContent=j.devname+' · '+j.mac;document.querySelectorAll('.dev').forEach(e=>e.textContent=location.origin)}).catch(()=>{});
</script></body></html>)HTML";

// OpenAPI 3.0 da API REST local. Servido em /docs/openapi.json.
static const char DOCS_OPENAPI[] PROGMEM = R"JSON({"openapi":"3.0.3","info":{"title":"PrintService API local","version":"FWVER","description":"API REST do dispositivo PrintService (ESP32-C6). Autenticacao HTTP Basic (usuario admin) quando ha senha de portal. O protocolo WebSocket com o aplicativo externo esta em docs/api-contract.md."},
"servers":[{"url":"/"}],
"components":{"securitySchemes":{"basic":{"type":"http","scheme":"basic"}},"schemas":{
"Error":{"type":"object","properties":{"error":{"type":"string"},"detail":{"type":"string"}}},
"Ok":{"type":"object","properties":{"ok":{"type":"boolean"}}},
"Supply":{"type":"object","properties":{"desc":{"type":"string"},"level":{"type":"integer"},"max":{"type":"integer"},"pct":{"type":"integer"},"class":{"type":"integer"},"type":{"type":"integer"}}},
"Ports":{"type":"object","properties":{"raw":{"type":"integer","nullable":true},"ipp":{"type":"integer","nullable":true},"lpd":{"type":"integer","nullable":true}}},
"Printer":{"type":"object","properties":{"ip":{"type":"string"},"host":{"type":"string"},"name":{"type":"string"},"model":{"type":"string"},"location":{"type":"string"},"manual":{"type":"boolean"},"mdns":{"type":"boolean"},"online":{"type":"boolean"},"alert":{"type":"boolean"},"dev_status":{"type":"integer"},"dev_status_text":{"type":"string"},"prn_status":{"type":"integer"},"prn_status_text":{"type":"string"},"errors":{"type":"string"},"pages":{"type":"integer","nullable":true},"last_ok":{"type":"integer","nullable":true},"snmp_version":{"type":"string"},"pdl":{"type":"array","items":{"type":"string"}},"ports":{"$ref":"#/components/schemas/Ports"},"ipp_path":{"type":"string"},"lpd_queue":{"type":"string"},"supplies":{"type":"array","items":{"$ref":"#/components/schemas/Supply"}}}},
"Printers":{"type":"object","properties":{"count":{"type":"integer"},"online":{"type":"integer"},"alerts":{"type":"integer"},"sta":{"type":"boolean"},"printers":{"type":"array","items":{"$ref":"#/components/schemas/Printer"}}}},
"Found":{"type":"object","properties":{"ip":{"type":"string"},"name":{"type":"string"},"host":{"type":"string"},"model":{"type":"string"},"ports":{"$ref":"#/components/schemas/Ports"},"ipp_path":{"type":"string"},"lpd_queue":{"type":"string"},"added":{"type":"boolean"}}},
"Discovery":{"type":"object","properties":{"running":{"type":"boolean"},"count":{"type":"integer"},"found":{"type":"array","items":{"$ref":"#/components/schemas/Found"}}}},
"Cloud":{"type":"object","properties":{"enabled":{"type":"boolean"},"url":{"type":"string"},"token_set":{"type":"boolean"},"state":{"type":"string","enum":["desativado","conectando","conectado","erro"]},"connected":{"type":"boolean"},"last_error":{"type":"string"},"connected_for":{"type":"integer"},"reconnects":{"type":"integer"}}},
"Job":{"type":"object","properties":{"id":{"type":"string"},"name":{"type":"string"},"source":{"type":"string"},"printer":{"type":"string"},"port":{"type":"integer"},"transport":{"type":"string","enum":["raw","ipp","lpd"]},"format":{"type":"string"},"queue":{"type":"string"},"expected":{"type":"integer"},"received":{"type":"integer"},"written":{"type":"integer"},"error":{"type":"string"},"detail":{"type":"string"},"duration_ms":{"type":"integer"}}},
"PrintStatus":{"type":"object","properties":{"state":{"type":"string","enum":["idle","connecting","streaming","finishing","done","error"]},"busy":{"type":"boolean"},"buffer_free":{"type":"integer"},"chunk_max":{"type":"integer"},"job":{"$ref":"#/components/schemas/Job"}}},
"Status":{"type":"object","properties":{"fw":{"type":"string"},"devname":{"type":"string"},"ssid":{"type":"string"},"sta_connected":{"type":"boolean"},"sta_ip":{"type":"string"},"rssi":{"type":"integer"},"ap":{"type":"boolean"},"ap_ssid":{"type":"string"},"ap_secure":{"type":"boolean"},"ap_ip":{"type":"string"},"ap_clients":{"type":"integer"},"mac":{"type":"string"},"uptime":{"type":"integer"},"heap":{"type":"integer"},"admin_auth":{"type":"boolean"},"community":{"type":"string"},"printers":{"type":"integer"},"printers_online":{"type":"integer"},"printers_alert":{"type":"integer"},"printing":{"type":"boolean"},"cloud":{"$ref":"#/components/schemas/Cloud"}}}
}},
"security":[{"basic":[]}],
"paths":{
"/api/status":{"get":{"summary":"Estado geral","tags":["status"],"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"$ref":"#/components/schemas/Status"}}}}}}},
"/api/scan":{"get":{"summary":"Scan de redes WiFi (assincrono)","tags":["status"],"parameters":[{"name":"refresh","in":"query","schema":{"type":"string"}}],"responses":{"200":{"description":"scanning ou done"}}}},
"/api/wifi":{"post":{"summary":"Configura WiFi e reinicia","tags":["status"],"requestBody":{"content":{"application/x-www-form-urlencoded":{"schema":{"type":"object","required":["ssid"],"properties":{"ssid":{"type":"string"},"pass":{"type":"string"}}}}}},"responses":{"200":{"description":"OK"},"400":{"description":"Erro","content":{"application/json":{"schema":{"$ref":"#/components/schemas/Error"}}}}}}},
"/api/device":{"post":{"summary":"Community SNMP, senha do AP e do portal","tags":["status"],"requestBody":{"content":{"application/x-www-form-urlencoded":{"schema":{"type":"object","properties":{"community":{"type":"string"},"appass":{"type":"string"},"adminpw":{"type":"string"}}}}}},"responses":{"200":{"description":"OK"}}}},
"/api/restart":{"post":{"summary":"Reinicia","tags":["status"],"responses":{"200":{"description":"OK"}}}},
"/api/reset":{"post":{"summary":"Reset de fabrica","tags":["status"],"responses":{"200":{"description":"OK"}}}},
"/api/printers":{"get":{"summary":"Impressoras monitoradas","tags":["printers"],"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"$ref":"#/components/schemas/Printers"}}}}}},"post":{"summary":"Inclui impressora por IP","tags":["printers"],"requestBody":{"content":{"application/x-www-form-urlencoded":{"schema":{"type":"object","required":["ip"],"properties":{"ip":{"type":"string"}}}}}},"responses":{"200":{"description":"OK"},"400":{"description":"IP invalido, ja cadastrada ou limite"}}}},
"/api/printers/remove":{"post":{"summary":"Remove impressora","tags":["printers"],"requestBody":{"content":{"application/x-www-form-urlencoded":{"schema":{"type":"object","required":["ip"],"properties":{"ip":{"type":"string"}}}}}},"responses":{"200":{"description":"OK"},"404":{"description":"Nao encontrada"}}}},
"/api/printers/refresh":{"post":{"summary":"Sondagem SNMP imediata","tags":["printers"],"responses":{"200":{"description":"OK"}}}},
"/api/discover/start":{"post":{"summary":"Inicia busca mDNS (~9 s)","tags":["discover"],"responses":{"202":{"description":"Iniciada"},"409":{"description":"Sem rede, em andamento ou mDNS indisponivel"}}}},
"/api/discover":{"get":{"summary":"Resultados da busca","tags":["discover"],"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"$ref":"#/components/schemas/Discovery"}}}}}}},
"/api/discover/clear":{"post":{"summary":"Libera resultados","tags":["discover"],"responses":{"200":{"description":"OK"}}}},
"/api/print/status":{"get":{"summary":"Estado do trabalho atual/ultimo","tags":["print"],"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"$ref":"#/components/schemas/PrintStatus"}}}}}}},
"/api/print/test":{"post":{"summary":"Pagina de teste (assincrona)","tags":["print"],"requestBody":{"content":{"application/x-www-form-urlencoded":{"schema":{"type":"object","required":["ip"],"properties":{"ip":{"type":"string"},"transport":{"type":"string","enum":["auto","raw","ipp","lpd"]},"port":{"type":"integer"},"queue":{"type":"string"},"format":{"type":"string","enum":["auto","pdf","pcl","text","ps"]}}}}}},"responses":{"202":{"description":"Enfileirada; acompanhar em /api/print/status"},"409":{"description":"Ocupado"}}}},
"/api/print/cancel":{"post":{"summary":"Cancela trabalho em curso","tags":["print"],"responses":{"200":{"description":"OK"},"404":{"description":"Nenhum trabalho"}}}},
"/api/print":{"post":{"summary":"Upload raw local (ate 16 KB)","tags":["print"],"parameters":[{"name":"ip","in":"query","required":true,"schema":{"type":"string"}},{"name":"transport","in":"query","schema":{"type":"string","enum":["auto","raw","ipp","lpd"]}},{"name":"port","in":"query","schema":{"type":"integer"}},{"name":"queue","in":"query","schema":{"type":"string"}},{"name":"format","in":"query","schema":{"type":"string"}},{"name":"name","in":"query","schema":{"type":"string"}}],"requestBody":{"content":{"application/octet-stream":{"schema":{"type":"string","format":"binary"}}}},"responses":{"202":{"description":"Enfileirado"},"409":{"description":"Ocupado"},"413":{"description":"Acima de 16 KB"}}}},
"/api/cloud":{"get":{"summary":"Estado do servidor externo","tags":["cloud"],"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"$ref":"#/components/schemas/Cloud"}}}}}},"post":{"summary":"Configura URL e token","tags":["cloud"],"requestBody":{"content":{"application/x-www-form-urlencoded":{"schema":{"type":"object","properties":{"url":{"type":"string"},"token":{"type":"string"}}}}}},"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"$ref":"#/components/schemas/Cloud"}}}}}}}
}})JSON";
