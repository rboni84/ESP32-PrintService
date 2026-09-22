#include "reset_button.h"
#include "status_led.h"
#include "portal.h"

namespace {

uint8_t g_pin = 255;
uint32_t pressStart = 0;   // millis() do inicio do pressionamento (0 = solto)
uint32_t lastSecond = 0;   // ultimo segundo anunciado no serial
bool fired = false;        // reset ja disparado nesta pressao

const uint32_t HOLD_MS = 5000;
const uint32_t FEEDBACK_AFTER_MS = 300;   // ignora toques curtos antes de acender o LED

}  // namespace

void ResetButton::begin(uint8_t pin) {
    g_pin = pin;
    pinMode(g_pin, INPUT_PULLUP);
}

void ResetButton::loop() {
    if (g_pin == 255) return;
    bool pressed = digitalRead(g_pin) == LOW;
    uint32_t now = millis();

    if (!pressed) {
        if (pressStart && !fired) {
            if (now - pressStart >= FEEDBACK_AFTER_MS) Serial.println("[BTN] solto antes de 5 s: reset cancelado");
            StatusLed::setOverride(StatusLed::Override::None);
        }
        pressStart = 0;
        lastSecond = 0;
        return;
    }

    if (!pressStart) { pressStart = now; fired = false; lastSecond = 0; return; }
    if (fired) return;

    uint32_t held = now - pressStart;
    if (held < FEEDBACK_AFTER_MS) return;

    StatusLed::setOverride(StatusLed::Override::ResetHold);
    uint32_t sec = held / 1000;
    if (sec != lastSecond) {
        lastSecond = sec;
        Serial.printf("[BTN] BOOT segurado: %lus / 5s (solte para cancelar)\n", (unsigned long)sec);
    }

    if (held >= HOLD_MS) {
        fired = true;
        Serial.println("[BTN] 5 s atingidos: restaurando padroes de fabrica e reiniciando...");
        StatusLed::setOverride(StatusLed::Override::ResetDone);
        Portal::factoryReset();
    }
}
