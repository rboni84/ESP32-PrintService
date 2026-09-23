#include "config.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>

static const char* NS = "printsrv";

// MAC da estacao WiFi (6 bytes), o mesmo que WiFi.macAddress() mostra. Funciona antes do WiFi subir.
// NAO usar ESP.getEfuseMac(): no ESP32-C6 ela devolve o MAC em forma EUI-64 (8 bytes, com FF:FE
// inserido no meio), e (mac >> 32) caia num byte fixo (FE) mais o 1o byte da parte unica. Resultado:
// chips do mesmo lote recebiam o mesmo nome (ex.: 98:A3:16:91:19:84 -> "PrintService-91FE").
static void stationMac(uint8_t out[6]) {
    uint8_t m[8] = {0};
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) != ESP_OK) esp_efuse_mac_get_default(m);
    memcpy(out, m, 6);
}

String Config::defaultDeviceName() {
    uint8_t m[6];
    stationMac(m);
    char buf[24];
    snprintf(buf, sizeof(buf), "PrintService-%02X%02X", m[4], m[5]);   // 2 ultimos bytes do MAC
    return String(buf);
}

String Config::defaultApPass() {
    uint8_t m[6];
    stationMac(m);
    uint32_t h = 0x811C9DC5u ^ 0x50533131u;  // FNV-1a offset ^ sal "PS11"
    for (int i = 0; i < 6; i++) {
        h ^= m[i];
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
