#include "portal.h"
#include "web_pages.h"
#include "json_util.h"
#include "printers.h"
#include "printjob.h"
#include "cloud.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace {

DeviceConfig* g_cfg = nullptr;
AsyncWebServer server(80);
DNSServer dns;

const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_MASK(255, 255, 255, 0);

bool apActive = false;
unsigned long staConnectedSince = 0;   // millis() em que STA conectou (0 = desconectado)
unsigned long staLostSince = 0;        // millis() em que STA caiu (0 = ok)
unsigned long lastReconnectTry = 0;
unsigned long restartAt = 0;           // agendamento de reboot (0 = nenhum)
bool doFactoryReset = false;

const unsigned long AP_SHUTDOWN_AFTER_STA_MS = 2 * 60 * 1000;  // desliga AP 2 min apos STA estavel
const unsigned long AP_RESTORE_AFTER_LOST_MS = 60 * 1000;      // religa AP 60 s sem STA
const unsigned long RECONNECT_INTERVAL_MS = 15 * 1000;

// Upload raw local para /api/print (buffer preenchido na task do servidor, consumido em Portal::loop)
const size_t UPLOAD_MAX = 16 * 1024;
struct {
    uint8_t buf[UPLOAD_MAX];
    size_t len = 0;
    volatile bool ready = false;   // corpo completo aguardando a task principal
    bool rejected = false;
    int rejectCode = 0;
    const char* rejectMsg = "";
    IPAddress ip;
    uint16_t port = 0;   // 0 = usar as portas conhecidas da impressora
    PrintJob::Transport transport = PrintJob::Transport::Auto;
    String name, id, format, queue;
} upload;

// Pedido de pagina de teste vindo da web. A conexao com a impressora bloqueia ate 3 s por porta e
// NAO pode rodar na task async_tcp (watchdog de 5 s): o handler so enfileira e Portal::loop executa.
struct {
    volatile bool pending = false;
    PrintJob::Target target;
    String format, id;
} testReq;

// ---------- utilidades ----------

String apSsid() { return g_cfg->deviceName; }

bool checkAuth(AsyncWebServerRequest* req) {
    if (g_cfg->adminPass.isEmpty()) return true;
    if (req->authenticate("admin", g_cfg->adminPass.c_str())) return true;
    req->requestAuthentication("PrintService");
    return false;
}

void sendJson(AsyncWebServerRequest* req, int code, const String& body) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", body);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

void sendError(AsyncWebServerRequest* req, int code, const char* msg) {
    sendJson(req, code, String("{\"error\":\"") + jsonEscape(msg) + "\"}");
}

void scheduleRestart(unsigned long delayMs = 1500) { restartAt = millis() + delayMs; }

// ---------- rede ----------

void startAP() {
    if (apActive) return;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
    const char* pass = g_cfg->apPass.length() >= 8 ? g_cfg->apPass.c_str() : nullptr;
    WiFi.softAP(apSsid().c_str(), pass);
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", AP_IP);
    apActive = true;
    if (pass) Serial.printf("[AP] ativo: SSID=%s senha=%s IP=%s\n", apSsid().c_str(), pass,
                            WiFi.softAPIP().toString().c_str());
    else      Serial.printf("[AP] ativo: SSID=%s (aberto) IP=%s\n", apSsid().c_str(),
                            WiFi.softAPIP().toString().c_str());
}

void stopAP() {
    if (!apActive) return;
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apActive = false;
    Serial.println("[AP] desligado (STA estavel)");
}

void startSTA() {
    if (!g_cfg->hasWifi()) return;
    WiFi.setAutoReconnect(true);
    WiFi.begin(g_cfg->wifiSsid.c_str(), g_cfg->wifiPass.c_str());
    lastReconnectTry = millis();
    Serial.printf("[STA] conectando a '%s'...\n", g_cfg->wifiSsid.c_str());
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            staConnectedSince = millis();
            staLostSince = 0;
            Serial.printf("[STA] conectado. IP=%s RSSI=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            // mDNS inicia uma unica vez: o componente acompanha mudancas de IP sozinho, e
            // reiniciar aqui invalidaria consultas assincronas em andamento (PrinterMonitor).
            {
                static bool mdnsStarted = false;
                if (!mdnsStarted && MDNS.begin(g_cfg->deviceName.c_str())) {
                    MDNS.addService("http", "tcp", 80);
                    mdnsStarted = true;
                }
            }
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (staConnectedSince) Serial.printf("[STA] desconectado (motivo %d)\n", info.wifi_sta_disconnected.reason);
            staConnectedSince = 0;
            if (!staLostSince) staLostSince = millis();
            break;
        default: break;
    }
}

// ---------- rotas ----------

bool isCaptiveProbe(const String& url) {
    static const char* probes[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
        "/connecttest.txt", "/ncsi.txt", "/fwlink", "/redirect", "/success.txt",
        "/canonical.html", "/check_network_status.txt", "/mobile/status.php"
    };
    for (auto p : probes) if (url.startsWith(p)) return true;
    return false;
}

void handleStatus(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    bool sta = WiFi.status() == WL_CONNECTED;
    String j = "{";
    j += "\"fw\":\"" FW_VERSION "\",";
    j += "\"devname\":\"" + jsonEscape(g_cfg->deviceName) + "\",";
    j += "\"ssid\":\"" + jsonEscape(g_cfg->wifiSsid) + "\",";
    j += "\"sta_connected\":" + String(sta ? "true" : "false") + ",";
    j += "\"sta_ip\":\"" + (sta ? WiFi.localIP().toString() : String("")) + "\",";
    j += "\"rssi\":" + String(sta ? WiFi.RSSI() : 0) + ",";
    j += "\"ap\":" + String(apActive ? "true" : "false") + ",";
    j += "\"ap_ssid\":\"" + jsonEscape(apSsid()) + "\",";
    j += "\"ap_secure\":" + String(g_cfg->apPass.length() >= 8 ? "true" : "false") + ",";
    j += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
    j += "\"ap_clients\":" + String(apActive ? WiFi.softAPgetStationNum() : 0) + ",";
    j += "\"mac\":\"" + WiFi.macAddress() + "\",";
    j += "\"uptime\":" + String(millis() / 1000) + ",";
    j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
    j += "\"admin_auth\":" + String(g_cfg->adminPass.isEmpty() ? "false" : "true") + ",";
    j += "\"community\":\"" + jsonEscape(g_cfg->snmpCommunity) + "\",";
    j += "\"printers\":" + String(PrinterMonitor::count()) + ",";
    j += "\"printers_online\":" + String(PrinterMonitor::onlineCount()) + ",";
    j += "\"printers_alert\":" + String(PrinterMonitor::alertCount()) + ",";
    j += "\"printing\":" + String(PrintJob::busy() ? "true" : "false") + ",";
    j += "\"cloud\":";
    Cloud::statusJson(j);
    j += "}";
    sendJson(req, 200, j);
}

void handleScan(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) { sendJson(req, 200, "{\"status\":\"scanning\"}"); return; }
    if (n == WIFI_SCAN_FAILED || req->hasParam("refresh")) {
        WiFi.scanDelete();
        WiFi.scanNetworks(true /*async*/, false /*hidden*/);
        sendJson(req, 200, "{\"status\":\"scanning\"}");
        return;
    }
    // resultado pronto: monta lista sem SSIDs duplicados, ordenada por RSSI (scan ja ordena)
    String j = "{\"status\":\"done\",\"networks\":[";
    bool first = true;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        bool dup = false;
        for (int k = 0; k < i; k++) if (WiFi.SSID(k) == ssid) { dup = true; break; }
        if (dup) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
             ",\"ch\":" + String(WiFi.channel(i)) +
             ",\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    j += "]}";
    WiFi.scanDelete();  // proximo GET dispara novo scan
    sendJson(req, 200, j);
}

void handleWifiSave(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    if (!req->hasParam("ssid", true)) { sendError(req, 400, "SSID obrigatorio"); return; }
    String ssid = req->getParam("ssid", true)->value();
    String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    ssid.trim();
    if (ssid.isEmpty() || ssid.length() > 32) { sendError(req, 400, "SSID invalido"); return; }
    if (pass.length() > 0 && pass.length() < 8) { sendError(req, 400, "Senha WiFi deve ter ao menos 8 caracteres"); return; }

    g_cfg->wifiSsid = ssid;
    g_cfg->wifiPass = pass;
    Config::save(*g_cfg);
    Serial.printf("[CFG] WiFi salvo: '%s'. Reiniciando...\n", ssid.c_str());
    sendJson(req, 200, "{\"ok\":true,\"restart\":true}");
    scheduleRestart();
}

void handleDeviceSave(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    bool changed = false, apChanged = false;

    // Nome do dispositivo e fixo (derivado do MAC); nao ha campo para altera-lo.
    if (req->hasParam("community", true)) {
        String c = req->getParam("community", true)->value();
        c.trim();
        if (c.length() > 32) { sendError(req, 400, "Community muito longa (max 32)"); return; }
        if (c.isEmpty()) c = "public";
        if (c != g_cfg->snmpCommunity) { g_cfg->snmpCommunity = c; changed = true; PrinterMonitor::refreshNow(); }
    }
    // Regra dos campos de senha: vazio = manter; " " (espaco) = limpar; outro = definir.
    auto applyPass = [&](const char* field, String& target, size_t minLen, const char* err) -> bool {
        if (!req->hasParam(field, true)) return true;
        String v = req->getParam(field, true)->value();
        if (v.isEmpty()) return true;
        if (v == " ") { if (!target.isEmpty()) { target = ""; changed = true; } return true; }
        if (v.length() < minLen) { sendError(req, 400, err); return false; }
        target = v; changed = true;
        return true;
    };
    String oldAp = g_cfg->apPass;
    if (!applyPass("appass", g_cfg->apPass, 8, "Senha do AP deve ter ao menos 8 caracteres")) return;
    if (!applyPass("adminpw", g_cfg->adminPass, 4, "Senha do portal deve ter ao menos 4 caracteres")) return;
    if (oldAp != g_cfg->apPass) apChanged = true;

    if (changed) {
        Config::save(*g_cfg);
        Serial.println("[CFG] configuracoes do dispositivo salvas");
    }
    // Senha do AP so muda apos reboot (para nao derrubar quem esta configurando agora).
    String j = String("{\"ok\":true,\"restart_required\":") + (apChanged ? "true" : "false") + "}";
    sendJson(req, 200, j);
}

void setupRoutes() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        AsyncWebServerResponse* r = req->beginResponse(200, "text/html", PAGE_INDEX);
        r->addHeader("Cache-Control", "no-store");
        req->send(r);
    });
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/scan", HTTP_GET, handleScan);
    server.on("/api/wifi", HTTP_POST, handleWifiSave);
    server.on("/api/device", HTTP_POST, handleDeviceSave);

    // ---- impressoras (fase 2) ----
    // ATENCAO: um handler em "/x" tambem atende "/x/qualquer-coisa" (canHandle aceita url que comeca
    // com uri + "/"). Rotas mais especificas devem ser registradas ANTES da generica.
    server.on("/api/printers/remove", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        if (!req->hasParam("ip", true) || !PrinterMonitor::remove(req->getParam("ip", true)->value())) {
            sendError(req, 404, "impressora nao encontrada"); return;
        }
        sendJson(req, 200, "{\"ok\":true}");
    });
    server.on("/api/printers/refresh", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        PrinterMonitor::refreshNow();
        sendJson(req, 200, "{\"ok\":true}");
    });
    server.on("/api/printers", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        String j;
        j.reserve(2048);
        PrinterMonitor::toJson(j);
        sendJson(req, 200, j);
    });
    server.on("/api/printers", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        if (!req->hasParam("ip", true)) { sendError(req, 400, "IP obrigatorio"); return; }
        String err;
        if (!PrinterMonitor::addManual(req->getParam("ip", true)->value(), &err)) { sendError(req, 400, err.c_str()); return; }
        sendJson(req, 200, "{\"ok\":true}");
    });

    // ---- servidor externo (fase 3) ----
    server.on("/api/cloud", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        String j; Cloud::statusJson(j);
        sendJson(req, 200, j);
    });
    server.on("/api/cloud", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        bool changed = false;
        if (req->hasParam("url", true)) {
            String u = req->getParam("url", true)->value(); u.trim();
            if (u.length() > 200) { sendError(req, 400, "URL muito longa (max 200)"); return; }
            if (u.length() && !(u.startsWith("ws://") || u.startsWith("wss://"))) { sendError(req, 400, "URL deve comecar com ws:// ou wss://"); return; }
            if (u != g_cfg->cloudUrl) { g_cfg->cloudUrl = u; changed = true; }
        }
        // token: vazio = manter; " " = limpar; outro = definir
        if (req->hasParam("token", true)) {
            String t = req->getParam("token", true)->value();
            if (t == " ") { if (g_cfg->cloudToken.length()) { g_cfg->cloudToken = ""; changed = true; } }
            else if (t.length()) {
                if (t.length() > 128) { sendError(req, 400, "Token muito longo (max 128)"); return; }
                t.trim();
                if (t != g_cfg->cloudToken) { g_cfg->cloudToken = t; changed = true; }
            }
        }
        if (changed) { Config::save(*g_cfg); Cloud::reconfigure(); Serial.println("[CFG] servidor externo atualizado"); }
        String j; Cloud::statusJson(j);
        sendJson(req, 200, j);
    });

    // ---- impressao (fase 3) ----
    server.on("/api/print/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        String j; PrintJob::statusJson(j);
        sendJson(req, 200, j);
    });
    server.on("/api/print/test", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        IPAddress ip;
        if (!req->hasParam("ip", true) || !ip.fromString(req->getParam("ip", true)->value())) { sendError(req, 400, "IP invalido"); return; }
        PrintJob::Target t;
        PrinterMonitor::fillTarget(ip, t);
        t.transport = PrintJob::parseTransport(req->hasParam("transport", true) ? req->getParam("transport", true)->value() : "auto");
        if (req->hasParam("port", true)) {
            uint16_t port = (uint16_t)req->getParam("port", true)->value().toInt();
            if (port) {
                if (t.transport == PrintJob::Transport::Ipp) t.ippPort = port;
                else if (t.transport == PrintJob::Transport::Lpd) t.lpdPort = port;
                else t.rawPort = port;
            }
        }
        if (req->hasParam("queue", true) && req->getParam("queue", true)->value().length()) t.lpdQueue = req->getParam("queue", true)->value();
        if (PrintJob::busy() || testReq.pending || upload.ready) { sendError(req, 409, "busy"); return; }
        testReq.target = t;
        testReq.format = req->hasParam("format", true) ? req->getParam("format", true)->value() : "auto";
        testReq.id = "test-" + String(millis());
        testReq.pending = true;   // executado em Portal::loop; acompanhe por GET /api/print/status
        sendJson(req, 202, "{\"ok\":true,\"queued\":true,\"job_id\":\"" + testReq.id + "\"}");
    });
    server.on("/api/print/cancel", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        if (!PrintJob::busy()) { sendError(req, 404, "nenhum trabalho em curso"); return; }
        PrintJob::cancel("canceled");
        sendJson(req, 200, "{\"ok\":true}");
    });
    // Upload raw local (corpo binario, ate UPLOAD_MAX bytes). O corpo chega na task do servidor
    // assincrono; o trabalho e efetivamente iniciado em Portal::loop() (task principal).
    server.on("/api/print", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!checkAuth(req)) return;
            if (upload.rejected) { upload.rejected = false; sendError(req, upload.rejectCode, upload.rejectMsg); return; }
            if (!upload.len) { sendError(req, 400, "corpo vazio"); return; }
            IPAddress ip;
            if (!req->hasParam("ip") || !ip.fromString(req->getParam("ip")->value())) { upload.len = 0; sendError(req, 400, "parametro ip invalido"); return; }
            upload.ip = ip;
            upload.port = req->hasParam("port") ? (uint16_t)req->getParam("port")->value().toInt() : 0;
            upload.transport = PrintJob::parseTransport(req->hasParam("transport") ? req->getParam("transport")->value() : "auto");
            upload.format = req->hasParam("format") ? req->getParam("format")->value() : "";
            upload.queue = req->hasParam("queue") ? req->getParam("queue")->value() : "";
            upload.name = req->hasParam("name") ? req->getParam("name")->value() : "upload";
            upload.id = "local-" + String(millis());
            upload.ready = true;
            sendJson(req, 202, "{\"ok\":true,\"job_id\":\"" + upload.id + "\",\"bytes\":" + String(upload.len) + "}");
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                upload.rejected = false;
                if (upload.ready || PrintJob::busy()) { upload.rejected = true; upload.rejectCode = 409; upload.rejectMsg = "trabalho em curso"; }
                else if (total > UPLOAD_MAX) { upload.rejected = true; upload.rejectCode = 413; upload.rejectMsg = "corpo maior que o limite local (use o servidor externo)"; }
                upload.len = 0;
            }
            if (upload.rejected) return;
            if (upload.len + len > UPLOAD_MAX) { upload.rejected = true; upload.rejectCode = 413; upload.rejectMsg = "corpo maior que o limite local"; upload.len = 0; return; }
            memcpy(upload.buf + upload.len, data, len);
            upload.len += len;
        });
    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        sendJson(req, 200, "{\"ok\":true}");
        scheduleRestart();
    });
    server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        sendJson(req, 200, "{\"ok\":true}");
        doFactoryReset = true;
        scheduleRestart();
    });

    // Captive portal: sondas dos sistemas operacionais e qualquer host desconhecido
    // sao redirecionados para a pagina do portal quando o AP esta ativo.
    server.onNotFound([](AsyncWebServerRequest* req) {
        // So redireciona quem chegou pela interface do AP (evita sequestrar requisicoes
        // feitas pelo IP da rede local quando AP e STA estao ativos ao mesmo tempo).
        bool viaAp = apActive && req->client() && req->client()->localIP() == AP_IP;
        bool toAp = viaAp && req->host() != AP_IP.toString();
        if (toAp || (viaAp && isCaptiveProbe(req->url()))) {
            req->redirect(String("http://") + AP_IP.toString() + "/");
            return;
        }
        sendError(req, 404, "nao encontrado");
    });
}

}  // namespace

// ---------- API publica ----------

void Portal::begin(DeviceConfig& cfg) {
    g_cfg = &cfg;

    WiFi.persistent(false);
    WiFi.setHostname(cfg.deviceName.c_str());
    WiFi.onEvent(onWiFiEvent);

    // AP sempre sobe no boot: garante acesso ao portal enquanto o STA tenta conectar.
    // Se o STA estabilizar, o AP e desligado apos AP_SHUTDOWN_AFTER_STA_MS.
    startAP();
    if (cfg.hasWifi()) startSTA();
    else Serial.println("[STA] sem rede configurada; use o portal para configurar.");

    setupRoutes();
    server.begin();
    Serial.println("[WEB] servidor iniciado na porta 80");
}

void Portal::loop() {
    unsigned long now = millis();

    if (apActive) dns.processNextRequest();

    if (testReq.pending) {
        String err;
        if (!PrintJob::printTest(testReq.target, testReq.format, testReq.id, "local-test", &err))
            Serial.printf("[PRT] pagina de teste nao iniciada: %s\n", err.c_str());
        testReq.pending = false;
    }

    if (upload.ready) {
        String err;
        PrintJob::Target t;
        PrinterMonitor::fillTarget(upload.ip, t);
        t.transport = upload.transport;
        if (upload.port) {
            if (upload.transport == PrintJob::Transport::Ipp) t.ippPort = upload.port;
            else if (upload.transport == PrintJob::Transport::Lpd) t.lpdPort = upload.port;
            else t.rawPort = upload.port;
        }
        if (upload.queue.length()) t.lpdQueue = upload.queue;
        if (PrintJob::start(upload.id, t, upload.len, upload.name, "local", upload.format, &err)) {
            if (!PrintJob::write(upload.buf, upload.len)) PrintJob::cancel("overflow");
            else PrintJob::finish();
        } else {
            Serial.printf("[PRT] upload local descartado: %s\n", err.c_str());
        }
        upload.len = 0;
        upload.ready = false;
    }

    if (restartAt && now >= restartAt) {
        if (doFactoryReset) { Config::reset(); Serial.println("[CFG] reset de fabrica"); }
        Serial.println("[SYS] reiniciando...");
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    if (!g_cfg->hasWifi()) return;

    bool sta = WiFi.status() == WL_CONNECTED;
    if (sta) {
        if (apActive && staConnectedSince && now - staConnectedSince > AP_SHUTDOWN_AFTER_STA_MS &&
            WiFi.softAPgetStationNum() == 0) {
            stopAP();
        }
    } else {
        if (!staLostSince) staLostSince = now;
        if (!apActive && now - staLostSince > AP_RESTORE_AFTER_LOST_MS) startAP();
        if (now - lastReconnectTry > RECONNECT_INTERVAL_MS) {
            lastReconnectTry = now;
            Serial.println("[STA] tentando reconectar...");
            WiFi.disconnect();
            WiFi.begin(g_cfg->wifiSsid.c_str(), g_cfg->wifiPass.c_str());
        }
    }
}

void Portal::scheduleRestart(unsigned long delayMs) { restartAt = millis() + delayMs; }
void Portal::factoryReset() { doFactoryReset = true; restartAt = millis() + 1500; }

bool Portal::isApMode() { return apActive; }
bool Portal::isStaConnected() { return WiFi.status() == WL_CONNECTED; }
