#pragma once
#include <Arduino.h>

// Botao BOOT (GPIO9, ativo em nivel baixo): segurar por 5 s apaga as configuracoes
// (reset de fabrica) e reinicia. Feedback pelo LED de status e pelo serial.
namespace ResetButton {
    void begin(uint8_t pin);
    void loop();
}
