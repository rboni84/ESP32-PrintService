#include "cloud.h"
#include "portal.h"
#include "printers.h"
#include "printjob.h"
#include "json_util.h"

#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace Cloud {

namespace {

const char* PROTOCOL_VERSION = "1";

DeviceConfig* g_cfg = nullptr;
WebSocketsClient ws;
bool started = false;        // ws.begin() ja chamado para a configuracao atual
bool connected = false;
String lastErr;
String authHeader;           // "Bearer <token>"
String extraHeaders;         // cabecalhos adicionais de identificacao
uint32_t lastStatusSent = 0, lastPrintersSent = 0, connectedAt = 0, lastAttempt = 0;
uint32_t statusIntervalMs = 30000, printersIntervalMs = 60000;
uint32_t reconnects = 0;
uint32_t attempts = 0;        // tentativas de handshake desde a configuracao
bool probeOk = false;         // DNS + TCP alcancaram o servidor na ultima sondagem
uint8_t chunkBuf[PrintJob::CHUNK_MAX];
bool lastJobReported = true;   // false quando o ultimo job.done/job.error nao pode ser enviado (link caido)
// job.ack adiado: so sai quando o buffer volta a ter espaco para um bloco inteiro (CHUNK_MAX).
// E o que da contrapressao real ao servidor: ele espera o ack, e o ack espera a impressora consumir.
bool ackPending = false;
String ackJobId;
uint32_t ackSeq = 0;
uint32_t ackDeferredAt = 0;
volatile bool reconfigPending = false;   // reconfigure() so marca; loop() aplica (o cliente WS nao e thread-safe)

bool isCloudSource(const String& s) { return s == "cloud" || s == "cloud-test"; }

// ---------- utilidades ----------

bool parseUrl(const String& url, bool& ssl, String& host, uint16_t& port, String& path) {
    String u = url; u.trim();
    if      (u.startsWith("wss://"))   { ssl = true;  u = u.substring(6); }
    else if (u.startsWith("ws://"))    { ssl = false; u = u.substring(5); }
    else if (u.startsWith("https://")) { ssl = true;  u = u.substring(8); }
    else if (u.startsWith("http://"))  { ssl = false; u = u.substring(7); }
    else return false;
    int slash = u.indexOf('/');
    String hp = slash < 0 ? u : u.substring(0, slash);
    path = slash < 0 ? String("/") : u.substring(slash);
    int colon = hp.indexOf(':');
    if (colon >= 0) { host = hp.substring(0, colon); port = (uint16_t)hp.substring(colon + 1).toInt(); }
    else            { host = hp; port = ssl ? 443 : 80; }
    return host.length() > 0 && port > 0;
}

void send(JsonDocument& doc) {
    if (!connected) return;
    doc["ts"] = millis();
    String out;
    serializeJson(doc, out);
    ws.sendTXT(out);
}

void sendError(const char* code, const char* message, const char* jobId = nullptr, const char* replyTo = nullptr) {
    JsonDocument d;
    d["type"] = jobId ? "job.error" : "error";
    d["code"] = code;
    if (message) d["message"] = message;
    if (jobId) d["job_id"] = jobId;
    if (replyTo) d["reply_to"] = replyTo;
    send(d);
    Serial.printf("[WS] erro enviado: %s (%s)\n", code, message ? message : "");
}

void sendHello() {
    JsonDocument d;
    d["type"] = "hello";
    d["protocol"] = PROTOCOL_VERSION;
    d["device"] = g_cfg->deviceName;
    d["mac"] = WiFi.macAddress();
    d["fw"] = FW_VERSION;
    d["ip"] = WiFi.localIP().toString();
    d["hostname"] = g_cfg->deviceName + ".local";
    d["chunk_max"] = PrintJob::CHUNK_MAX;
    d["buffer"] = PrintJob::BUFFER_SIZE;
    d["data_timeout"] = PrintJob::dataTimeout() / 1000;
    JsonArray caps = d["capabilities"].to<JsonArray>();
    caps.add("raw9100"); caps.add("ipp"); caps.add("lpd"); caps.add("snmp"); caps.add("mdns"); caps.add("test_page"); caps.add("pdl"); caps.add("restart");
    // Ultimo trabalho originado do servidor: permite reconciliar a fila apos uma queda do link.
    const PrintJob::Info& lj = PrintJob::info();
    if ((lj.state == PrintJob::State::Done || lj.state == PrintJob::State::Error) && isCloudSource(lj.source)) {
        JsonObject o = d["last_job"].to<JsonObject>();
        o["job_id"] = lj.id;
        o["state"] = lj.state == PrintJob::State::Done ? "done" : "error";
        o["bytes"] = lj.written;
        if (lj.state == PrintJob::State::Error) o["code"] = lj.error;
        o["reported"] = lastJobReported;
    }
    send(d);
    lastJobReported = true;   // o hello ja levou a informacao
}

void sendStatus() {
    JsonDocument d;
    d["type"] = "status";
    d["uptime"] = millis() / 1000;
    d["heap"] = ESP.getFreeHeap();
    d["rssi"] = WiFi.RSSI();
    d["ip"] = WiFi.localIP().toString();
    d["ap"] = Portal::isApMode();
    d["printers"] = PrinterMonitor::count();
    d["printers_online"] = PrinterMonitor::onlineCount();
    d["printers_alert"] = PrinterMonitor::alertCount();
    d["printing"] = PrintJob::busy();
    if (PrintJob::busy()) d["job_id"] = PrintJob::info().id;
    send(d);
    lastStatusSent = millis();
}

void sendPrinters() {
    // Reaproveita o JSON do monitor como objeto "data" (mesmo formato de GET /api/printers).
    String data;
    data.reserve(2048);
    PrinterMonitor::toJson(data);
    String out = "{\"type\":\"printers\",\"ts\":" + String(millis()) + ",\"data\":" + data + "}";
    ws.sendTXT(out);
    lastPrintersSent = millis();
}

bool discoverPending = false;

void sendDiscovery() {
    String data;
    data.reserve(1024);
    PrinterMonitor::discoveryJson(data);
    String out = "{\"type\":\"discover.results\",\"ts\":" + String(millis()) + ",\"data\":" + data + "}";
    ws.sendTXT(out);
}

void sendJobStatus(const char* type) {
    String s;
    PrintJob::statusJson(s);
    String out = String("{\"type\":\"") + type + "\",\"ts\":" + String(millis()) + ",\"data\":" + s + "}";
    ws.sendTXT(out);
}

void sendAck(const String& jobId, uint32_t seq, bool last) {
    JsonDocument d;
    d["type"] = "job.ack"; d["job_id"] = jobId; d["seq"] = seq;
    d["received"] = PrintJob::info().received; d["written"] = PrintJob::info().written;
    d["buffer_free"] = PrintJob::freeSpace(); d["last"] = last;
    send(d);
}

// Envia o ack adiado quando couber outro bloco; descarta se o trabalho acabou (job.done/job.error ja informam).
void flushPendingAck() {
    if (!ackPending) return;
    if (!PrintJob::busy() || PrintJob::info().id != ackJobId) { ackPending = false; return; }
    if (PrintJob::freeSpace() < PrintJob::CHUNK_MAX) return;
    ackPending = false;
    uint32_t waited = millis() - ackDeferredAt;
    if (waited > 2000) Serial.printf("[WS] job.ack seq=%lu adiado %lu ms (impressora lenta)\n", (unsigned long)ackSeq, (unsigned long)waited);
    sendAck(ackJobId, ackSeq, false);
}

void onJobDone(const PrintJob::Info& info) {
    if (!isCloudSource(info.source)) return;
    ackPending = false;
    if (!connected) { lastJobReported = false; return; }   // sera relatado no proximo hello
    lastJobReported = true;
    JsonDocument d;
    d["type"] = info.state == PrintJob::State::Done ? "job.done" : "job.error";
    d["job_id"] = info.id;
    d["bytes"] = info.written;
    d["duration_ms"] = info.finishedAt - info.startedAt;
    d["transport"] = PrintJob::transportText(info.transport);
    d["port"] = info.port;
    if (info.detail.length()) d["detail"] = info.detail;
    if (info.state != PrintJob::State::Done) d["code"] = info.error;
    send(d);
}

// Monta o destino a partir do que o monitor sabe da impressora mais overrides da mensagem.
void targetFromMessage(JsonDocument& doc, const IPAddress& ip, PrintJob::Target& t) {
    PrinterMonitor::fillTarget(ip, t);
    t.transport = PrintJob::parseTransport(String(doc["transport"] | "auto"));
    uint16_t port = doc["port"] | 0;
    if (port) {
        if (t.transport == PrintJob::Transport::Ipp) t.ippPort = port;
        else if (t.transport == PrintJob::Transport::Lpd) t.lpdPort = port;
        else t.rawPort = port;
    }
    const char* path = doc["ipp_path"] | (const char*)nullptr;
    if (path && *path) t.ippPath = path[0] == '/' ? String(path) : "/" + String(path);
    const char* queue = doc["lpd_queue"] | (const char*)nullptr;
    if (queue && *queue) t.lpdQueue = queue;
}

// ---------- mensagens recebidas ----------

bool parseIp(JsonVariantConst v, IPAddress& ip) {
    const char* s = v.as<const char*>();
    return s && ip.fromString(s);
}

void handleMessage(const uint8_t* payload, size_t len) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, payload, len);
    if (e) { sendError("bad_json", e.c_str()); return; }
    const char* type = doc["type"] | "";
    const char* jobId = doc["job_id"] | (const char*)nullptr;

    if (!strcmp(type, "welcome")) {
        uint32_t hb = doc["status_interval"] | 0;
        if (hb >= 5 && hb <= 600) statusIntervalMs = hb * 1000;
        uint32_t pi = doc["printers_interval"] | 0;
        if (pi >= 10 && pi <= 3600) printersIntervalMs = pi * 1000;
        uint32_t dt = doc["data_timeout"] | 0;
        if (dt >= 10 && dt <= 120) PrintJob::setDataTimeout(dt * 1000);
        Serial.println("[WS] welcome recebido");
        sendStatus();
        sendPrinters();
        return;
    }
    if (!strcmp(type, "ping"))         { JsonDocument d; d["type"] = "pong"; send(d); return; }
    if (!strcmp(type, "get_status"))   { sendStatus(); return; }
    if (!strcmp(type, "get_printers")) { sendPrinters(); return; }
    if (!strcmp(type, "get_job"))      { sendJobStatus("job.status"); return; }
    if (!strcmp(type, "refresh"))      { PrinterMonitor::refreshNow(); JsonDocument d; d["type"] = "ack"; d["reply_to"] = "refresh"; send(d); return; }
    if (!strcmp(type, "discover")) {
        String err;
        bool ok = PrinterMonitor::startDiscovery(&err);
        if (ok) discoverPending = true;   // discover.results sai quando a busca terminar
        JsonDocument d; d["type"] = ok ? "ack" : "error"; d["reply_to"] = "discover";
        if (!ok) d["code"] = err;
        send(d);
        return;
    }
    if (!strcmp(type, "get_discovery")) { sendDiscovery(); return; }

    if (!strcmp(type, "printer.add")) {
        String err;
        bool ok = PrinterMonitor::addManual(String(doc["printer"] | ""), &err);
        JsonDocument d; d["type"] = ok ? "ack" : "error"; d["reply_to"] = "printer.add";
        if (!ok) d["code"] = err;
        send(d);
        return;
    }
    if (!strcmp(type, "printer.remove")) {
        bool ok = PrinterMonitor::remove(String(doc["printer"] | ""));
        JsonDocument d; d["type"] = ok ? "ack" : "error"; d["reply_to"] = "printer.remove";
        if (!ok) d["code"] = "not_found";
        send(d);
        return;
    }

    // Reinicio remoto. Com trabalho em curso, recusa (busy) a menos que force: true; nesse caso o
    // trabalho e cancelado antes (o job.error sai ainda neste link). Sem reset de fabrica pela cloud.
    if (!strcmp(type, "device.restart")) {
        bool force = doc["force"] | false;
        if (PrintJob::busy() && !force) { sendError("busy", "trabalho em curso; cancele antes ou envie force:true", nullptr, "device.restart"); return; }
        if (PrintJob::busy()) PrintJob::cancel("restart");
        JsonDocument d; d["type"] = "ack"; d["reply_to"] = "device.restart"; d["delay_ms"] = 1500;
        send(d);
        Serial.println("[WS] reinicio pedido pelo servidor");
        Portal::scheduleRestart(1500);
        return;
    }

    if (!strcmp(type, "print_test")) {
        IPAddress ip;
        if (!parseIp(doc["printer"], ip)) { sendError("bad_printer", "IP invalido", jobId, "print_test"); return; }
        String id = jobId ? String(jobId) : "test-" + String(millis());
        PrintJob::Target t;
        targetFromMessage(doc, ip, t);
        String err;
        if (!PrintJob::printTest(t, String(doc["format"] | "auto"), id, "cloud-test", &err)) {
            sendError(err.c_str(), PrintJob::info().detail.c_str(), id.c_str(), "print_test");
            return;
        }
        JsonDocument d; d["type"] = "job.ready"; d["job_id"] = id; d["test"] = true;
        d["transport"] = PrintJob::transportText(PrintJob::info().transport); d["port"] = PrintJob::info().port;
        send(d);
        return;
    }

    if (!strcmp(type, "job.start")) {
        if (!jobId) { sendError("missing_job_id", "job_id obrigatorio"); return; }
        IPAddress ip;
        if (!parseIp(doc["printer"], ip)) { sendError("bad_printer", "IP invalido", jobId); return; }
        PrintJob::Target t;
        targetFromMessage(doc, ip, t);
        String err;
        if (!PrintJob::start(jobId, t, doc["size"] | 0, String(doc["name"] | ""), "cloud", String(doc["format"] | ""), &err)) {
            sendError(err.c_str(), PrintJob::info().detail.length() ? PrintJob::info().detail.c_str() : "trabalho nao iniciado", jobId);
            return;
        }
        JsonDocument d;
        d["type"] = "job.ready"; d["job_id"] = jobId;
        d["chunk_max"] = PrintJob::CHUNK_MAX; d["buffer_free"] = PrintJob::freeSpace();
        d["transport"] = PrintJob::transportText(PrintJob::info().transport); d["port"] = PrintJob::info().port;
        if (PrintJob::info().docFormat.length()) d["format"] = PrintJob::info().docFormat;
        send(d);
        return;
    }

    if (!strcmp(type, "job.chunk")) {
        if (!jobId || !PrintJob::busy() || PrintJob::info().id != jobId) { sendError("no_such_job", "nenhum trabalho ativo com esse job_id", jobId); return; }
        const char* data = doc["data"] | "";
        size_t olen = 0;
        int rc = mbedtls_base64_decode(chunkBuf, sizeof(chunkBuf), &olen, (const uint8_t*)data, strlen(data));
        if (rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) { PrintJob::cancel("chunk_too_large"); sendError("chunk_too_large", "bloco maior que chunk_max", jobId); return; }
        if (rc != 0) { PrintJob::cancel("bad_base64"); sendError("bad_base64", "base64 invalido", jobId); return; }
        // Bloco chegou sem esperar o ack anterior: o servidor nao precisava dele; libera-o para manter a contagem de seq.
        if (ackPending) { ackPending = false; sendAck(ackJobId, ackSeq, false); }
        if (olen && !PrintJob::write(chunkBuf, olen)) { PrintJob::cancel("overflow"); sendError("overflow", "buffer cheio: aguarde job.ack antes do proximo bloco", jobId); return; }
        bool last = doc["last"] | false;
        uint32_t seq = doc["seq"] | 0;
        if (last) { PrintJob::finish(); sendAck(jobId, seq, true); return; }
        // Ack imediato so se ja couber outro bloco inteiro; senao fica pendente ate a impressora drenar
        if (PrintJob::freeSpace() >= PrintJob::CHUNK_MAX) { sendAck(jobId, seq, false); return; }
        ackPending = true; ackJobId = jobId; ackSeq = seq; ackDeferredAt = millis();
        return;
    }

    if (!strcmp(type, "job.cancel")) {
        if (jobId && PrintJob::busy() && PrintJob::info().id == jobId) PrintJob::cancel("canceled");
        else sendError("no_such_job", "nenhum trabalho ativo com esse job_id", jobId);
        return;
    }

    sendError("unknown_type", type);
}

void onEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            connected = true;
            connectedAt = millis();
            lastErr = "";
            Serial.printf("[WS] conectado a %s\n", g_cfg->cloudUrl.c_str());
            sendHello();
            break;
        case WStype_DISCONNECTED: {
            // A biblioteca passa o motivo como texto: "HTTP 404", "WebSocket handshake failed - HTTP 401"...
            String reason;
            if (payload && length) { char b[121]; size_t n = length > 120 ? 120 : length; memcpy(b, payload, n); b[n] = 0; reason = b; }
            if (reason.length()) {
                // So e recusa de handshake quando o servidor respondeu HTTP. Os demais textos da biblioteca
                // sao quedas de rede: "TCP connection cleanup" e a limpeza do objeto da tentativa anterior de
                // connect() (DNS/TCP/TLS falhou em silencio) e sai no inicio da tentativa seguinte.
                bool refused = reason.indexOf("HTTP") >= 0 || reason.indexOf("handshake") >= 0;
                if (reason.indexOf("401") >= 0 || reason.indexOf("403") >= 0) reason += " (token recusado pelo servidor)";
                else if (reason.indexOf("404") >= 0) reason += " (endpoint WebSocket inexistente nesse caminho)";
                else if (refused) reason += " (servidor respondeu HTTP em vez de aceitar o WebSocket)";
                else if (reason == "TCP connection cleanup") reason = "tentativa anterior de conexao TCP/TLS falhou (DNS, TCP ou TLS; heap livre " + String(ESP.getFreeHeap()) + " B); nova tentativa em 10 s";
                else if (reason == "Connection lost") reason = "conexao perdida (servidor ou rede fechou o TCP)";
                else if (reason == "Header response timeout") reason = "servidor nao respondeu ao handshake WebSocket em 5 s";
                lastErr = reason;
                Serial.printf(refused ? "[WS] handshake recusado: %s\n" : "[WS] queda de conexao: %s\n", reason.c_str());
            } else if (connected) {
                Serial.println("[WS] desconectado");
            }
            if (connected) reconnects++;
            attempts++;
            connected = false;
            ackPending = false;
            // Sem link nao ha mais blocos: aborta ja, em vez de esperar data_timeout. O resultado
            // (link_lost) vai no last_job do proximo hello.
            if (PrintJob::busy() && isCloudSource(PrintJob::info().source)) PrintJob::cancel("link_lost");
            break;
        }
        case WStype_TEXT:
            handleMessage(payload, length);
            break;
        case WStype_BIN:
            sendError("binary_unsupported", "use job.chunk com base64");
            break;
        case WStype_ERROR: {
            // payload nao e necessariamente terminado em NUL: copia com limite
            char b[121];
            size_t n = length > 120 ? 120 : length;
            if (n && payload) { memcpy(b, payload, n); b[n] = 0; lastErr = b; } else lastErr = "erro";
        }
            Serial.printf("[WS] erro: %s\n", lastErr.c_str());
            break;
        default: break;
    }
}

void startClient() {
    bool ssl; String host, path; uint16_t port;
    if (!parseUrl(g_cfg->cloudUrl, ssl, host, port, path)) {
        lastErr = "URL invalida (use ws:// ou wss://)";
        started = true;   // nao tenta de novo ate reconfigurar
        Serial.printf("[WS] %s\n", lastErr.c_str());
        return;
    }
    // Sondagem previa (bloqueante, poucos segundos): separa "DNS nao resolve", "TCP nao responde"
    // e "TLS/handshake", que a biblioteca nao distingue por eventos.
    attempts = 0;
    probeOk = false;
    {
        IPAddress ip;
        if (!WiFi.hostByName(host.c_str(), ip) || ip == IPAddress((uint32_t)0)) {
            lastErr = "DNS nao resolveu '" + host + "' (verifique a URL e o DNS da rede)";
        } else {
            WiFiClient probe;
            if (probe.connect(ip, port, 3000)) { probeOk = true; probe.stop(); }
            else lastErr = "TCP " + host + ":" + String(port) + " sem resposta (saida para a internet bloqueada na rede, firewall ou porta errada)";
        }
        if (probeOk && ssl) {
            // Handshake TLS de teste (insecure, como a biblioteca): expoe o erro do mbedTLS se falhar.
            NetworkClientSecure tls;
            tls.setInsecure();
            tls.setHandshakeTimeout(10);
            if (tls.connect(host.c_str(), port)) { tls.stop(); }
            else {
                char eb[128] = {0};
                tls.lastError(eb, sizeof(eb));
                lastErr = String("TLS falhou com ") + host + ": " + (eb[0] ? eb : "sem detalhe") + " (heap livre " + String(ESP.getFreeHeap()) + ")";
                probeOk = false;
            }
        }
        if (!probeOk) Serial.printf("[WS] sondagem falhou: %s\n", lastErr.c_str());
        else Serial.printf("[WS] servidor alcancavel (%s:%u); iniciando handshake TLS/WebSocket\n", host.c_str(), port);
    }

    authHeader = g_cfg->cloudToken.length() ? "Bearer " + g_cfg->cloudToken : String();
    extraHeaders = "X-Device-Id: " + g_cfg->deviceName + "\r\nX-Device-Mac: " + WiFi.macAddress() +
                   "\r\nX-Firmware: " FW_VERSION;
    ws.onEvent(onEvent);
    // Sem CA configurada a biblioteca aceita qualquer certificado (setInsecure); o token
    // continua protegido pelo TLS contra escuta, mas nao contra um servidor impostor.
    if (ssl) ws.beginSSL(host.c_str(), port, path.c_str());
    else     ws.begin(host.c_str(), port, path.c_str());
    // ATENCAO: begin()/beginSSL() zeram a autorizacao e demais opcoes do cliente. Tudo o que
    // segue precisa vir DEPOIS, senao o handshake sai sem "Authorization" e o servidor devolve 401.
    if (authHeader.length()) ws.setAuthorization(authHeader.c_str());
    ws.setExtraHeaders(extraHeaders.c_str());
    ws.setReconnectInterval(10000);
    ws.enableHeartbeat(15000, 5000, 3);
    started = true;
    lastAttempt = millis();
    Serial.printf("[WS] conectando a %s://%s:%u%s\n", ssl ? "wss" : "ws", host.c_str(), port, path.c_str());
}

}  // namespace

// ---------- API publica ----------

void begin(DeviceConfig& cfg) {
    g_cfg = &cfg;
    PrintJob::onDone(onJobDone);
    if (!cfg.hasCloud()) Serial.println("[WS] servidor externo nao configurado");
}

// Pode ser chamado da task do servidor web (POST /api/cloud). Nao toca no WebSocketsClient aqui:
// ele pode estar no meio de um handshake TLS dentro de ws.loop() na task principal, e o acesso
// concorrente derruba o chip. A troca efetiva acontece em loop().
void reconfigure() {
    reconfigPending = true;
}

void loop() {
    if (reconfigPending) {
        reconfigPending = false;
        if (started) ws.disconnect();
        started = false;
        connected = false;
        lastErr = "";
        Serial.println(g_cfg && g_cfg->hasCloud() ? "[WS] reconfigurado; reconectando" : "[WS] servidor externo desativado");
    }
    if (!g_cfg || !g_cfg->hasCloud()) return;
    if (!Portal::isStaConnected()) {
        if (started) { ws.disconnect(); started = false; connected = false; }
        return;
    }
    if (!started) { startClient(); return; }
    ws.loop();
    if (!connected) {
        // TCP alcancou o servidor, mas nenhum handshake concluiu nem foi recusado em 30 s:
        // quase sempre TLS (certificado/SNI/memoria) ou servidor que nao fala WebSocket nesse caminho.
        if (probeOk && lastErr.isEmpty() && millis() - lastAttempt > 30000) {
            lastErr = "handshake TLS/WebSocket sem resposta em 30 s (TLS falhou ou o servidor nao aceita WebSocket nesse caminho)";
            Serial.printf("[WS] %s\n", lastErr.c_str());
        }
        return;
    }
    lastErr = "";
    flushPendingAck();
    uint32_t now = millis();
    if (now - lastStatusSent > statusIntervalMs) sendStatus();
    if (now - lastPrintersSent > printersIntervalMs) sendPrinters();
    if (discoverPending && !PrinterMonitor::isDiscovering()) { discoverPending = false; sendDiscovery(); }
}

bool isConfigured() { return g_cfg && g_cfg->hasCloud(); }
bool isConnected() { return connected; }

const char* stateText() {
    if (!isConfigured()) return "desativado";
    if (reconfigPending) return "conectando";
    if (connected) return "conectado";
    if (lastErr.length()) return "erro";
    return "conectando";
}

String lastError() { return lastErr; }

void statusJson(String& j) {
    j += "{\"enabled\":" + String(isConfigured() ? "true" : "false");
    j += ",\"url\":\"" + jsonEscape(g_cfg ? g_cfg->cloudUrl : "") + "\"";
    j += ",\"token_set\":" + String((g_cfg && g_cfg->cloudToken.length()) ? "true" : "false");
    j += ",\"state\":\"" + String(stateText()) + "\"";
    j += ",\"connected\":" + String(connected ? "true" : "false");
    j += ",\"last_error\":\"" + jsonEscape(lastErr) + "\"";
    j += ",\"connected_for\":" + String(connected ? (millis() - connectedAt) / 1000 : 0);
    j += ",\"attempts\":" + String(attempts);
    j += ",\"reconnects\":" + String(reconnects) + "}";
}

}  // namespace Cloud
