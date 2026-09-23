#pragma once
#include <Arduino.h>
#include "config.h"

// Portal web + captive portal (AP) para configuracao basica.
namespace Portal {
    void begin(DeviceConfig& cfg);
    void loop();

    // Estado da rede (para exibir no portal / usar em outros modulos)
    bool isApMode();
    bool isStaConnected();

    // Acoes de sistema (usadas pelo portal web e pelo console serial)
    void scheduleRestart(unsigned long delayMs = 1500);
    void factoryReset();   // agenda reset de fabrica + reboot

    // Busca de redes WiFi (assincrona). Start devolve o codigo de WiFi.scanNetworks
    // (WIFI_SCAN_RUNNING / WIFI_SCAN_FAILED); com o STA em laco de reconexao, pausa a
    // reconexao para o scan poder iniciar. Finish descarta o resultado e retoma o STA.
    int16_t wifiScanStart();
    void wifiScanFinish();
}
