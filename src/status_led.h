#pragma once
#include <Arduino.h>
#include "config.h"

// LED RGB WS2812 de status (GPIO8 na DevKitM-1). Padroes documentados em docs/status-led.md.
// O estado normal e deduzido de Portal/PrinterMonitor; overrides sao usados pelo botao de reset.
namespace StatusLed {
    enum class Override : uint8_t {
        None,        // estado automatico
        ResetHold,   // botao BOOT sendo segurado (magenta piscando rapido)
        ResetDone,   // reset de fabrica disparado (branco fixo ate reiniciar)
    };

    void begin(uint8_t pin, const DeviceConfig& cfg);
    void loop();
    void setOverride(Override o);
}
