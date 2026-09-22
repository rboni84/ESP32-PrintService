// PrintService - servidor de monitoramento de impressoras (ESP32-C6)
//
// Fase 1 (atual): portal AP / captive portal para configuracao basica
//   - WiFi (SSID/senha), nome do dispositivo, senha do AP e do portal
//   - fallback automatico: sem STA -> AP ativo; STA estavel -> AP desliga
//   - nome do dispositivo e senha padrao do AP derivados do MAC (impressos no serial)
//   - menu interativo no serial ("menu" + Enter)
//   - LED RGB de status (GPIO8) e botao BOOT (GPIO9) segurado 5 s = reset de fabrica
//     (ver docs/status-led.md)
// Fase 2 (atual): descoberta e observacao de impressoras na rede
//   - mDNS (_ipp/_printer/_pdl-datastream) + cadastro manual por IP (persistido)
//   - SNMP: hrDeviceStatus, hrPrinterStatus, hrPrinterDetectedErrorState,
//     prtMarkerLifeCount (paginas) e prtMarkerSuppliesTable (toner/cilindro)
//   - aba "Impressoras" no portal (/api/printers) e listagem no menu serial
// Fase 3 (atual): servico de impressao
//   - cliente WebSocket para o servidor externo (URL + token provisionados), protocolo
//     JSON com trabalhos em blocos base64 (docs/api-contract.md)
//   - impressao raw na porta 9100 (PrintJob) e pagina de teste (portal, serial, WS)

#include <Arduino.h>
#include "config.h"
#include "portal.h"
#include "console.h"
#include "printers.h"
#include "status_led.h"
#include "reset_button.h"
#include "printjob.h"
#include "cloud.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef PIN_STATUS_LED
#define PIN_STATUS_LED 8   // WS2812 da DevKitM-1
#endif
#ifndef PIN_RESET_BTN
#define PIN_RESET_BTN 9    // botao BOOT
#endif

static DeviceConfig g_config;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n=== PrintService v%s ===\n", FW_VERSION);

    StatusLed::begin(PIN_STATUS_LED, g_config);   // branco enquanto inicializa
    ResetButton::begin(PIN_RESET_BTN);

    Config::load(g_config);
    Serial.printf("[CFG] dispositivo: %s | wifi: %s\n", g_config.deviceName.c_str(),
                  g_config.hasWifi() ? g_config.wifiSsid.c_str() : "(nao configurado)");

    Portal::begin(g_config);
    PrinterMonitor::begin(g_config);
    Cloud::begin(g_config);
    Console::begin(g_config);
}

void loop() {
    Portal::loop();
    PrinterMonitor::loop();   // so age com STA conectado
    PrintJob::loop();         // drena o trabalho de impressao em curso
    Cloud::loop();            // WebSocket com o servidor externo
    Console::loop();
    ResetButton::loop();
    StatusLed::loop();

    static unsigned long lastLog = 0;
    if (millis() - lastLog > 30000) {
        lastLog = millis();
        Serial.printf("[SYS] up=%lus heap=%u AP=%d STA=%d\n", millis() / 1000, ESP.getFreeHeap(),
                      Portal::isApMode(), Portal::isStaConnected());
    }
    delay(2);
}
