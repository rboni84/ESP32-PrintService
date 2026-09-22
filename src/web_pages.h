#pragma once
#include <Arduino.h>

// Paginas HTML embutidas (sem necessidade de upload de filesystem).
// Dados dinamicos sao carregados via /api/status e /api/scan (JSON).

static const char PAGE_INDEX[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PrintService</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--fg:#1d2733;--muted:#6b7785;--acc:#1f6feb;--ok:#1a7f37;--warn:#b35900;--err:#c8102e;--bd:#dde3ea}
@media(prefers-color-scheme:dark){:root{--bg:#0f141a;--card:#161d26;--fg:#e6edf3;--muted:#8b98a5;--acc:#4c8dff;--bd:#2b3543}}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{background:var(--acc);color:#fff;padding:14px 16px}header h1{margin:0;font-size:18px}header small{opacity:.85}
nav{display:flex;gap:4px;padding:8px 16px 0;max-width:640px;margin:0 auto}nav a{padding:8px 12px;border-radius:8px 8px 0 0;text-decoration:none;color:var(--muted)}
nav a.on{background:var(--card);color:var(--fg);font-weight:600}
main{max-width:640px;margin:0 auto;padding:16px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:16px;margin-bottom:16px}
.card h2{margin:0 0 12px;font-size:16px}
dl{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;margin:0}dt{color:var(--muted)}dd{margin:0;word-break:break-all}
label{display:block;margin:10px 0 4px;color:var(--muted);font-size:13px}
input,select{width:100%;padding:9px 10px;border:1px solid var(--bd);border-radius:8px;background:var(--bg);color:var(--fg);font-size:15px}
button{padding:10px 16px;border:0;border-radius:8px;background:var(--acc);color:#fff;font-size:15px;cursor:pointer;margin-top:14px}
button.sec{background:transparent;color:var(--acc);border:1px solid var(--acc)}button.danger{background:var(--err)}
button:disabled{opacity:.6;cursor:default}
.row{display:flex;gap:8px;flex-wrap:wrap}.row>*{flex:1}
.badge{display:inline-block;padding:2px 8px;border-radius:999px;font-size:12px;color:#fff;background:var(--muted)}
.badge.ok{background:var(--ok)}.badge.warn{background:var(--warn)}.badge.err{background:var(--err)}
.hint{color:var(--muted);font-size:13px;margin-top:6px}
#msg{display:none;padding:10px 12px;border-radius:8px;margin-bottom:12px}
#msg.ok{display:block;background:#e6f4ea;color:#1a7f37}#msg.err{display:block;background:#fde8ec;color:#c8102e}
.tab{display:none}.tab.on{display:block}
.lnk{color:var(--acc);font-size:14px}
.prn{border-left:4px solid var(--muted)}.prn.ok{border-left-color:var(--ok)}.prn.warn{border-left-color:var(--warn)}.prn.err{border-left-color:var(--err)}
.prn h2{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap}.prn h2 span{font-weight:400;font-size:13px;color:var(--muted)}
.prn .meta{color:var(--muted);font-size:13px;margin:0 0 8px}
.prn .errs{color:var(--err);font-size:13px;margin:0 0 8px}
.sup{display:grid;grid-template-columns:1fr auto;gap:2px 10px;align-items:center;font-size:13px;margin-top:6px}
.bar{grid-column:1/3;height:8px;background:var(--bd);border-radius:4px;overflow:hidden}.bar i{display:block;height:100%;background:var(--ok)}
.bar.warn i{background:var(--warn)}.bar.err i{background:var(--err)}
.prn .act{display:flex;gap:8px;justify-content:flex-end}.prn .act button{margin-top:8px;padding:6px 10px;font-size:13px}
</style></head><body>
<header><h1>PrintService <small id="fw"></small></h1><small id="devname">Servidor de impressoras</small></header>
<nav><a href="#status" class="on" data-t="status">Status</a><a href="#printers" data-t="printers">Impressoras</a><a href="#wifi" data-t="wifi">Rede WiFi</a><a href="#device" data-t="device">Dispositivo</a><a href="#system" data-t="system">Sistema</a></nav>
<main>
<div id="msg"></div>

<section class="tab on" id="t-status"><div class="card"><h2>Estado da conexão</h2>
<dl>
<dt>Modo</dt><dd id="s-mode">-</dd>
<dt>Rede (STA)</dt><dd id="s-sta">-</dd>
<dt>IP (STA)</dt><dd id="s-staip">-</dd>
<dt>Sinal</dt><dd id="s-rssi">-</dd>
<dt>AP</dt><dd id="s-ap">-</dd>
<dt>IP (AP)</dt><dd id="s-apip">-</dd>
<dt>MAC</dt><dd id="s-mac">-</dd>
<dt>Uptime</dt><dd id="s-up">-</dd>
<dt>Heap livre</dt><dd id="s-heap">-</dd>
</dl></div>
<div class="card"><h2>Impressoras</h2><p id="s-prn" class="hint">-</p><a href="#printers" data-t="printers" class="lnk">Ver impressoras</a></div>
</section>

<section class="tab" id="t-printers">
<div id="prn-list"><div class="card"><p class="hint">Carregando…</p></div></div>
<div class="card"><h2>Adicionar impressora por IP</h2>
<form id="f-prn" class="row"><input name="ip" id="p-ip" placeholder="192.168.0.50" pattern="\d{1,3}(\.\d{1,3}){3}" required style="flex:2"><button type="submit" style="margin-top:0;flex:0 0 auto">Adicionar</button><button type="button" class="sec" id="b-prn-refresh" style="margin-top:0;flex:0 0 auto">Atualizar agora</button></form>
<p class="hint">Impressoras que anunciam IPP/LPD/JetDirect via mDNS aparecem sozinhas. As demais podem ser cadastradas por IP (SNMP porta 161, community configurável na aba Dispositivo).</p>
</div>
<div class="card"><h2>Impressão</h2>
<label>Formato da página de teste</label><select id="p-fmt"><option value="pcl">PCL / texto (laser e multifuncionais em geral)</option><option value="text">Texto puro + avanço de página</option><option value="ps">PostScript</option></select>
<p class="hint">A página de teste é enviada em raw para a porta 9100 da impressora (JetDirect). Use o botão "Página de teste" em cada impressora acima.</p>
<h2 style="margin-top:14px">Último trabalho</h2><dl id="p-job"><dt>Estado</dt><dd>-</dd></dl>
<div class="row"><button class="sec" id="b-prn-cancel">Cancelar trabalho em curso</button></div>
</div></section>

<section class="tab" id="t-wifi"><div class="card"><h2>Conectar a uma rede WiFi</h2>
<form id="f-wifi">
<label>Rede (SSID)</label>
<div class="row"><select id="w-list"><option value="">— buscar redes —</option></select><button type="button" class="sec" id="w-scan" style="margin-top:0;flex:0 0 auto">Buscar</button></div>
<label>ou digite o SSID</label><input name="ssid" id="w-ssid" maxlength="32" autocomplete="off">
<label>Senha</label><input name="pass" id="w-pass" type="password" maxlength="63" autocomplete="off">
<p class="hint">Após salvar, o dispositivo reinicia e tenta conectar. Se falhar, volta ao modo AP para nova configuração.</p>
<button type="submit">Salvar e reiniciar</button>
</form></div></section>

<section class="tab" id="t-device"><div class="card"><h2>Dispositivo</h2>
<form id="f-device">
<dl><dt>Nome / hostname</dt><dd id="d-name">-</dd><dt>MAC</dt><dd id="d-mac">-</dd></dl>
<p class="hint">O nome é fixo e derivado do MAC da placa. Ele é também o SSID do AP de configuração e o hostname na rede (<span id="d-mdns">-</span>).</p>
<label>Community SNMP das impressoras <span class="hint">(padrão: public)</span></label><input name="community" id="d-community" maxlength="32" autocomplete="off">
<label>Senha do AP de configuração <span class="hint">(padrão: derivada do MAC, exibida no terminal serial; mín. 8 caracteres)</span></label><input name="appass" id="d-appass" type="password" maxlength="63" autocomplete="new-password">
<label>Senha do portal web (usuário: admin) <span class="hint">(vazio = sem senha)</span></label><input name="adminpw" id="d-adminpw" type="password" maxlength="63" autocomplete="new-password">
<p class="hint">Campos de senha deixados em branco mantêm o valor atual. Digite um espaço para remover uma senha (AP fica aberto / portal sem senha).</p>
<button type="submit">Salvar</button>
</form></div>
<div class="card"><h2>Servidor externo (aplicativo)</h2>
<dl><dt>Estado</dt><dd id="c-state">-</dd></dl>
<form id="f-cloud">
<label>URL WebSocket <span class="hint">(ws:// ou wss://; vazio = desativar)</span></label><input name="url" id="c-url" maxlength="200" placeholder="wss://app.exemplo.com/ws/devices/PrintService-3A7F" autocomplete="off">
<label>Token de acesso <span class="hint">(vazio = manter; espaço = remover)</span></label><input name="token" id="c-token" type="password" maxlength="128" autocomplete="new-password">
<p class="hint">O aplicativo provisiona a URL e o token. O dispositivo conecta como cliente, envia status e impressoras e recebe trabalhos de impressão. Protocolo em docs/api-contract.md.</p>
<button type="submit">Salvar e reconectar</button>
</form></div></section>

<section class="tab" id="t-system"><div class="card"><h2>Sistema</h2>
<div class="row">
<button class="sec" id="b-restart">Reiniciar</button>
<button class="danger" id="b-reset">Restaurar padrões</button>
</div>
<p class="hint">Restaurar padrões apaga WiFi, senhas, community SNMP e impressoras cadastradas, e reinicia em modo AP. O mesmo efeito é obtido segurando o botão BOOT da placa por 5 segundos.</p>
</div></section>
</main>
<script>
const $=s=>document.querySelector(s);
function show(t){document.querySelectorAll('.tab').forEach(e=>e.classList.toggle('on',e.id==='t-'+t));document.querySelectorAll('nav a').forEach(a=>a.classList.toggle('on',a.dataset.t===t));}
document.querySelectorAll('nav a,a.lnk').forEach(a=>a.onclick=e=>{e.preventDefault();show(a.dataset.t);location.hash=a.dataset.t});
if(location.hash)show(location.hash.slice(1));
function msg(t,ok){const m=$('#msg');m.textContent=t;m.className=ok?'ok':'err';setTimeout(()=>m.className='',8000)}
function fmtUp(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+h+'h '+m+'m'}
async function status(){try{const r=await fetch('/api/status');const j=await r.json();
$('#fw').textContent='v'+j.fw;$('#devname').textContent=j.devname;
$('#s-mode').innerHTML=j.sta_connected?'<span class="badge ok">Conectado (STA)</span>':(j.ap?'<span class="badge warn">Modo AP (configuração)</span>':'<span class="badge err">Desconectado</span>');
$('#s-sta').textContent=j.ssid||'(não configurado)';$('#s-staip').textContent=j.sta_ip||'-';
$('#s-rssi').textContent=j.sta_connected?j.rssi+' dBm':'-';
$('#s-ap').textContent=j.ap?j.ap_ssid+(j.ap_secure?' (protegido)':' (aberto)'):'desligado';$('#s-apip').textContent=j.ap?j.ap_ip:'-';
$('#s-mac').textContent=j.mac;$('#s-up').textContent=fmtUp(j.uptime);$('#s-heap').textContent=Math.round(j.heap/1024)+' KB';
$('#d-name').textContent=j.devname;$('#d-mac').textContent=j.mac;$('#d-mdns').textContent=j.devname+'.local';
if(j.cloud){const c=j.cloud;$('#c-state').innerHTML=c.enabled?('<span class="badge '+(c.connected?'ok':(c.last_error?'err':'warn'))+'">'+c.state+'</span>'+(c.connected?' há '+c.connected_for+' s':'')+(c.last_error?' · '+esc(c.last_error):'')+(c.token_set?'':' · <b>sem token</b>')):'desativado';if(!$('#c-url').value&&document.activeElement!==$('#c-url'))$('#c-url').value=c.url||''}if(!$('#w-ssid').value)$('#w-ssid').value=j.ssid||'';
if(!$('#d-community').value)$('#d-community').value=j.community||'';
$('#s-prn').textContent=j.sta_connected?(j.printers?j.printers+' impressora(s): '+j.printers_online+' online, '+j.printers_alert+' com alerta':'Nenhuma impressora encontrada ainda.'):'Monitoramento inativo: conecte o dispositivo a uma rede WiFi.';
}catch(e){}}
status();setInterval(status,5000);
const esc=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const supName={3:'Toner',4:'Toner residual',5:'Tinta',6:'Cartucho de tinta',8:'Revelador',9:'Fusor',15:'Cilindro',21:'Tinta residual'};
function supHtml(s){const p=s.pct;let cls='',txt;if(p<0){txt=s.level===-3?'ok':'n/d'}else{txt=p+'%'+(s['class']===4?' cheio':'');if(s['class']===4){cls=p>=90?'err':p>=75?'warn':''}else{cls=p<=5?'err':p<=15?'warn':''}}
const bar=p<0?'':'<div class="bar '+cls+'"><i style="width:'+p+'%"></i></div>';return '<span>'+esc(s.desc||supName[s.type]||'Suprimento')+'</span><b>'+txt+'</b>'+bar}
async function printers(){try{const r=await fetch('/api/printers');const j=await r.json();const el=$('#prn-list');
if(!j.printers.length){el.innerHTML='<div class="card"><h2>Impressoras</h2><p class="hint">'+(j.sta?'Nenhuma impressora encontrada. A descoberta mDNS roda a cada 2 minutos; você também pode adicionar por IP abaixo.':'Conecte o dispositivo a uma rede WiFi para iniciar o monitoramento.')+'</p></div>';return}
el.innerHTML=j.printers.map(p=>{const cls=!p.online?'err':(p.alert?'warn':'ok');const title=esc(p.name||p.model||p.host||p.ip);
const st=!p.online?'<span class="badge err">offline</span>':(p.alert?'<span class="badge warn">alerta</span>':'<span class="badge ok">ok</span>');
let meta=[p.model&&p.model!==p.name?esc(p.model):'',esc(p.ip)+(p.host?' · '+esc(p.host):''),p.location?esc(p.location):'',p.manual?'cadastro manual':'descoberta mDNS'].filter(Boolean).join(' · ');
let body='';if(p.online){body+='<p class="meta">'+esc(p.dev_status_text)+' / '+esc(p.prn_status_text)+(p.pages!=null?' · '+p.pages.toLocaleString('pt-BR')+' páginas':'')+(p.last_ok!=null?' · há '+p.last_ok+' s':'')+'</p>';
if(p.errors)body+='<p class="errs">⚠ '+esc(p.errors)+'</p>';if(p.supplies.length)body+='<div class="sup">'+p.supplies.map(supHtml).join('')+'</div>'}
else body+='<p class="meta">Sem resposta SNMP'+(p.last_ok!=null?' (última há '+p.last_ok+' s)':'')+'</p>';
return '<div class="card prn '+cls+'"><h2>'+title+' '+st+'</h2><p class="meta">'+meta+'</p>'+body+'<div class="act"><button class="sec" data-test="'+esc(p.ip)+'">Página de teste</button><button class="sec" data-rm="'+esc(p.ip)+'">Remover</button></div></div>'}).join('');
el.querySelectorAll('[data-rm]').forEach(b=>b.onclick=async()=>{if(confirm('Remover '+b.dataset.rm+'?')){try{await post('/api/printers/remove',null,{ip:b.dataset.rm});printers()}catch(x){msg(x.message,false)}}});
el.querySelectorAll('[data-test]').forEach(b=>b.onclick=async()=>{try{const j=await post('/api/print/test',null,{ip:b.dataset.test,format:$('#p-fmt').value});msg('Página de teste enviada ('+j.job_id+'). Acompanhe em "Último trabalho".',true);setTimeout(printStatus,1500)}catch(x){msg(x.message,false)}});
}catch(e){}}
printers();setInterval(printers,10000);
$('#f-prn').onsubmit=async e=>{e.preventDefault();try{await post('/api/printers',e.target);msg('Impressora adicionada.',true);$('#p-ip').value='';setTimeout(printers,500)}catch(x){msg(x.message,false)}};
$('#b-prn-refresh').onclick=async()=>{try{await post('/api/printers/refresh');msg('Descoberta e sondagem reagendadas.',true);setTimeout(printers,4000)}catch(x){msg(x.message,false)}};
const jobState={idle:'nenhum',streaming:'enviando',finishing:'finalizando',done:'concluído',error:'erro'};
async function printStatus(){try{const r=await fetch('/api/print/status');const j=await r.json();const d=$('#p-job');
let h='<dt>Estado</dt><dd>'+(jobState[j.state]||j.state)+'</dd>';
if(j.job){h+='<dt>Trabalho</dt><dd>'+esc(j.job.id)+(j.job.name?' · '+esc(j.job.name):'')+'</dd><dt>Impressora</dt><dd>'+esc(j.job.printer)+':'+j.job.port+'</dd><dt>Bytes</dt><dd>'+j.job.written+(j.job.expected?' / '+j.job.expected:'')+'</dd><dt>Duração</dt><dd>'+(j.job.duration_ms/1000).toFixed(1)+' s</dd>';if(j.job.error)h+='<dt>Erro</dt><dd class="errs">'+esc(j.job.error)+'</dd>'}
d.innerHTML=h;$('#b-prn-cancel').disabled=!j.busy;}catch(e){}}
printStatus();setInterval(printStatus,3000);
$('#b-prn-cancel').onclick=async()=>{try{await post('/api/print/cancel');msg('Trabalho cancelado.',true);printStatus()}catch(x){msg(x.message,false)}};
async function saveCloud(e){e.preventDefault();try{const j=await post('/api/cloud',e.target);msg('Servidor externo salvo. Estado: '+j.state,true);$('#c-token').value=''}catch(x){msg(x.message,false)}}
$('#f-cloud').onsubmit=saveCloud;
async function scan(first){if(first){$('#w-scan').disabled=true;$('#w-scan').textContent='Buscando…'}
try{const r=await fetch('/api/scan');const j=await r.json();
if(j.status==='scanning'){setTimeout(()=>scan(false),1500);return}
const l=$('#w-list');l.innerHTML='<option value="">— selecione —</option>';
j.networks.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+'  ('+n.rssi+' dBm'+(n.secure?', protegida':'')+')';l.appendChild(o)});
}catch(e){msg('Falha ao buscar redes',false)}
$('#w-scan').disabled=false;$('#w-scan').textContent='Buscar'}
$('#w-scan').onclick=()=>scan(true);$('#w-list').onchange=e=>{if(e.target.value)$('#w-ssid').value=e.target.value};
async function post(url,form,obj){const body=form?new URLSearchParams(new FormData(form)):(obj?new URLSearchParams(obj):'');const r=await fetch(url,{method:'POST',body});const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||('HTTP '+r.status));return j}
$('#f-wifi').onsubmit=async e=>{e.preventDefault();if(!$('#w-ssid').value){msg('Informe o SSID',false);return}
try{await post('/api/wifi',e.target);msg('Salvo. Reiniciando... conecte-se à mesma rede para acessar o dispositivo.',true)}catch(x){msg(x.message,false)}};
$('#f-device').onsubmit=async e=>{e.preventDefault();try{await post('/api/device',e.target);msg('Configurações salvas.',true);$('#d-appass').value='';$('#d-adminpw').value='';status()}catch(x){msg(x.message,false)}};
$('#b-restart').onclick=async()=>{if(confirm('Reiniciar o dispositivo?')){await post('/api/restart');msg('Reiniciando...',true)}};
$('#b-reset').onclick=async()=>{if(confirm('Apagar TODAS as configurações e reiniciar?')){await post('/api/reset');msg('Restaurado. Reiniciando em modo AP...',true)}};
</script></body></html>)HTML";
