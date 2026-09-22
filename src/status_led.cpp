#include "status_led.h"
#include "portal.h"
#include "printers.h"
#include "printjob.h"

namespace {

uint8_t g_pin = 255;
const DeviceConfig* g_cfg = nullptr;
StatusLed::Override g_override = StatusLed::Override::None;

const uint8_t BRIGHT = 32;   // intensidade maxima (0-255); LED da placa e bem forte
uint8_t lastR = 255, lastG = 255, lastB = 255;

void write(uint8_t r, uint8_t g, uint8_t b) {
    if (r == lastR && g == lastG && b == lastB) return;   // evita reenviar o mesmo frame ao WS2812
    rgbLedWrite(g_pin, r, g, b);
    lastR = r; lastG = g; lastB = b;
}

// Cores base (escala aplicada por 'level' 0..BRIGHT)
void color(uint8_t level, uint8_t r, uint8_t g, uint8_t b) {
    write((uint16_t)r * level / 255, (uint16_t)g * level / 255, (uint16_t)b * level / 255);
}
void off() { write(0, 0, 0); }

bool blinkOn(uint32_t periodMs) { return (millis() / periodMs) % 2 == 0; }

// Respiracao: sobe e desce suavemente entre ~0 e BRIGHT no periodo indicado.
uint8_t breathe(uint32_t periodMs) {
    uint32_t t = millis() % periodMs;
    uint32_t half = periodMs / 2;
    uint32_t x = t < half ? t : periodMs - t;   // 0..half..0
    return (uint8_t)(2 + (uint32_t)(BRIGHT - 2) * x / half);
}

}  // namespace

void StatusLed::begin(uint8_t pin, const DeviceConfig& cfg) {
    g_pin = pin;
    g_cfg = &cfg;
    color(BRIGHT, 255, 255, 255);   // branco: inicializando
}

void StatusLed::setOverride(Override o) { g_override = o; }

void StatusLed::loop() {
    if (g_pin == 255) return;

    switch (g_override) {
        case Override::ResetHold:
            if (blinkOn(150)) color(BRIGHT, 255, 0, 255); else off();   // magenta rapido
            return;
        case Override::ResetDone:
            color(BRIGHT, 255, 255, 255);                                 // branco fixo
            return;
        default: break;
    }

    if (PrintJob::busy()) {
        if (blinkOn(200)) color(BRIGHT, 0, 255, 255); else off();   // ciano piscando: imprimindo
        return;
    }

    if (!g_cfg->hasWifi()) {
        color(breathe(2000), 0, 0, 255);                 // azul respirando: modo AP, aguardando configuracao
        return;
    }
    if (!Portal::isStaConnected()) {
        if (blinkOn(300)) color(BRIGHT, 255, 120, 0); else off();   // laranja piscando rapido: conectando
        return;
    }

    bool offline = false, alert = false;
    uint8_t n = PrinterMonitor::count();
    for (uint8_t i = 0; i < n; i++) {
        const PrinterMonitor::Printer* p = PrinterMonitor::get(i);
        if (!p) continue;
        if (!p->online) { if (p->everOnline || p->manual) offline = true; }
        else if (p->hasAlert()) alert = true;
    }
    if (offline)      { if (blinkOn(500)) color(BRIGHT, 255, 0, 0); else color(4, 255, 0, 0); }  // vermelho piscando
    else if (alert)   color(BRIGHT, 255, 120, 0);                                                // laranja fixo
    else if (n == 0)  color(breathe(3000), 0, 255, 0);                                          // verde respirando: sem impressoras
    else              color(BRIGHT / 2, 0, 255, 0);                                             // verde fixo: tudo ok
}
