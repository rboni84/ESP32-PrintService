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
}
