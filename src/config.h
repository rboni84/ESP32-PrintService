#pragma once
#include <Arduino.h>

// Configuracao persistente do dispositivo (armazenada em NVS via Preferences).
struct DeviceConfig {
    String deviceName;   // nome do dispositivo / hostname / SSID do AP (fixo, derivado do MAC)
    String wifiSsid;     // rede WiFi (modo STA)
    String wifiPass;     // senha WiFi
    String apPass;       // senha do AP de configuracao (vazio = aberto; padrao = derivada do MAC)
    String adminPass;    // senha do portal web (vazio = sem autenticacao)
    String snmpCommunity; // community SNMP usada nas impressoras (padrao "public")
    String cloudUrl;      // URL WebSocket do servidor externo (ws:// ou wss://); vazio = desativado
    String cloudToken;    // token de acesso enviado como "Authorization: Bearer <token>"

    bool hasCloud() const { return cloudUrl.length() > 0; }

    bool hasWifi() const { return wifiSsid.length() > 0; }
};

namespace Config {
    void load(DeviceConfig& cfg);
    void save(const DeviceConfig& cfg);
    void reset();
    String defaultDeviceName();
    // Senha padrao do AP: 8 hex derivados do MAC (hash FNV-1a com sal). Exibida no terminal serial.
    String defaultApPass();
}
