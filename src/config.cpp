#include "config.h"
#include <Preferences.h>
#include <WiFi.h>

static const char* NS = "printsrv";

String Config::defaultDeviceName() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[24];
    snprintf(buf, sizeof(buf), "PrintService-%04X", (uint16_t)(mac >> 32));
    return String(buf);
}

String Config::defaultApPass() {
    uint64_t mac = ESP.getEfuseMac();
    uint32_t h = 0x811C9DC5u ^ 0x50533131u;  // FNV-1a offset ^ sal "PS11"
    for (int i = 0; i < 6; i++) {
        h ^= (uint8_t)(mac >> (8 * i));
        h *= 0x01000193u;
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%08X", h);
    return String(buf);
}

void Config::load(DeviceConfig& cfg) {
    Preferences p;
    p.begin(NS, true);
    cfg.deviceName = defaultDeviceName();   // nao editavel: identifica a placa pelo MAC
    cfg.wifiSsid   = p.getString("ssid", "");
    cfg.wifiPass   = p.getString("pass", "");
    // Chave ausente = nunca configurada -> senha derivada do MAC. Chave presente e vazia = AP aberto.
    cfg.apPass     = p.isKey("appass") ? p.getString("appass", "") : defaultApPass();
    cfg.adminPass  = p.getString("adminpw", "");
    cfg.snmpCommunity = p.getString("community", "public");
    if (cfg.snmpCommunity.isEmpty()) cfg.snmpCommunity = "public";
    cfg.cloudUrl   = p.getString("cloudurl", "");
    cfg.cloudToken = p.getString("cloudtok", "");
    p.end();
}

void Config::save(const DeviceConfig& cfg) {
    Preferences p;
    p.begin(NS, false);
    p.putString("ssid", cfg.wifiSsid);
    p.putString("pass", cfg.wifiPass);
    p.putString("appass", cfg.apPass);
    p.putString("adminpw", cfg.adminPass);
    p.putString("community", cfg.snmpCommunity);
    p.putString("cloudurl", cfg.cloudUrl);
    p.putString("cloudtok", cfg.cloudToken);
    p.end();
}

void Config::reset() {
    Preferences p;
    p.begin(NS, false);
    p.clear();
    p.end();
}
