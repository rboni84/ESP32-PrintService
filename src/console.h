#pragma once
#include <Arduino.h>
#include "config.h"

// Menu interativo no terminal serial (115200). Nao bloqueante: le a linha caractere
// a caractere em loop(). Permite ver status e configurar WiFi, nome, senhas, reiniciar
// e restaurar padroes sem depender do portal web.
namespace Console {
    void begin(DeviceConfig& cfg);
    void loop();

    void printMenu();
    void printStatus();
}
