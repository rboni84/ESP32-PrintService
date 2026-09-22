#include "cloud.h"
#include "portal.h"
#include "printers.h"
#include "printjob.h"
#include "json_util.h"

#include <WiFi.h>
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
uint8_t chunkBuf[PrintJob::CHUNK_MAX];
bool lastJobReported = true;   // false quando o ultimo job.done/job.error nao pode ser enviado (link caido)

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
    caps.add("raw9100"); caps.add("snmp"); caps.add("mdns"); caps.add("test_page"); caps.add("pdl");
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

void sendJobStatus(const char* type) {
    String s;
    PrintJob::statusJson(s);
    String out = String("{\"type\":\"") + type + "\",\"ts\":" + String(millis()) + ",\"data\":" + s + "}";
    ws.sendTXT(out);
}

void onJobDone(const PrintJob::Info& info) {
    if (!isCloudSource(info.source)) return;
    if (!connected) { lastJobReported = false; return; }   // sera relatado no proximo hello
    lastJobReported = true;
    JsonDocument d;
    d["type"] = info.state == PrintJob::State::Done ? "job.done" : "job.error";
    d["job_id"] = info.id;
    d["bytes"] = info.written;
    d["duration_ms"] = info.finishedAt - info.startedAt;
    if (info.state != PrintJob::State::Done) d["code"] = info.error;
    send(d);
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

    if (!strcmp(type, "print_test")) {
        IPAddress ip;
        if (!parseIp(doc["printer"], ip)) { sendError("bad_printer", "IP invalido", jobId, "print_test"); return; }
        String id = jobId ? String(jobId) : "test-" + String(millis());
        String err;
        if (!PrintJob::printTest(ip, doc["port"] | PrintJob::DEFAULT_PORT, String(doc["format"] | "pcl"), id, "cloud-test", &err)) {
            sendError(err.c_str(), "pagina de teste nao iniciada", id.c_str(), "print_test");
            return;
        }
        JsonDocument d; d["type"] = "job.ready"; d["job_id"] = id; d["test"] = true; send(d);
        return;
    }

    if (!strcmp(type, "job.start")) {
        if (!jobId) { sendError("missing_job_id", "job_id obrigatorio"); return; }
        IPAddress ip;
        if (!parseIp(doc["printer"], ip)) { sendError("bad_printer", "IP invalido", jobId); return; }
        String err;
        if (!PrintJob::start(jobId, ip, doc["port"] | PrintJob::DEFAULT_PORT, doc["size"] | 0,
                             String(doc["name"] | ""), "cloud", &err)) {
            sendError(err.c_str(), "trabalho nao iniciado", jobId);
            return;
        }
        JsonDocument d;
        d["type"] = "job.ready"; d["job_id"] = jobId;
        d["chunk_max"] = PrintJob::CHUNK_MAX; d["buffer_free"] = PrintJob::freeSpace();
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
        if (olen && !PrintJob::write(chunkBuf, olen)) { PrintJob::cancel("overflow"); sendError("overflow", "buffer cheio: aguarde job.ack antes do proximo bloco", jobId); return; }
        bool last = doc["last"] | false;
        if (last) PrintJob::finish();
        JsonDocument d;
        d["type"] = "job.ack"; d["job_id"] = jobId; d["seq"] = doc["seq"] | 0;
        d["received"] = PrintJob::info().received; d["written"] = PrintJob::info().written;
        d["buffer_free"] = PrintJob::freeSpace(); d["last"] = last;
        send(d);
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
        case WStype_DISCONNECTED:
            if (connected) { Serial.println("[WS] desconectado"); reconnects++; }
            connected = false;
            // Sem link nao ha mais blocos: aborta ja, em vez de esperar data_timeout. O resultado
            // (link_lost) vai no last_job do proximo hello.
            if (PrintJob::busy() && isCloudSource(PrintJob::info().source)) PrintJob::cancel("link_lost");
            break;
        case WStype_TEXT:
            handleMessage(payload, length);
            break;
        case WStype_BIN:
            sendError("binary_unsupported", "use job.chunk com base64");
            break;
        case WStype_ERROR:
            lastErr = length ? String((const char*)payload).substring(0, 120) : "erro";
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
    authHeader = g_cfg->cloudToken.length() ? "Bearer " + g_cfg->cloudToken : String();
    extraHeaders = "X-Device-Id: " + g_cfg->deviceName + "\r\nX-Device-Mac: " + WiFi.macAddress() +
                   "\r\nX-Firmware: " FW_VERSION;
    ws.onEvent(onEvent);
    if (authHeader.length()) ws.setAuthorization(authHeader.c_str());
    ws.setExtraHeaders(extraHeaders.c_str());
    ws.setReconnectInterval(10000);
    ws.enableHeartbeat(15000, 5000, 3);
    // Sem CA configurada a biblioteca aceita qualquer certificado (setInsecure); o token
    // continua protegido pelo TLS contra escuta, mas nao contra um servidor impostor.
    if (ssl) ws.beginSSL(host.c_str(), port, path.c_str());
    else     ws.begin(host.c_str(), port, path.c_str());
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

void reconfigure() {
    if (started) { ws.disconnect(); }
    started = false;
    connected = false;
    lastErr = "";
}

void loop() {
    if (!g_cfg || !g_cfg->hasCloud()) return;
    if (!Portal::isStaConnected()) {
        if (started) { ws.disconnect(); started = false; connected = false; }
        return;
    }
    if (!started) { startClient(); return; }
    ws.loop();
    if (!connected) return;
    uint32_t now = millis();
    if (now - lastStatusSent > statusIntervalMs) sendStatus();
    if (now - lastPrintersSent > printersIntervalMs) sendPrinters();
}

bool isConfigured() { return g_cfg && g_cfg->hasCloud(); }
bool isConnected() { return connected; }

const char* stateText() {
    if (!isConfigured()) return "desativado";
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
    j += ",\"reconnects\":" + String(reconnects) + "}";
}

}  // namespace Cloud
