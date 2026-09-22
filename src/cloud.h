#pragma once
#include <Arduino.h>
#include "config.h"

// Ligacao com o servidor externo (aplicativo) por WebSocket. O ESP e o cliente:
// conecta na URL provisionada, autentica com o token (Authorization: Bearer) e
// troca mensagens JSON conforme docs/api-contract.md (status, impressoras, trabalhos
// de impressao em blocos, pagina de teste).
namespace Cloud {
    void begin(DeviceConfig& cfg);
    void loop();
    void reconfigure();          // aplicar nova URL/token sem reiniciar

    bool isConfigured();
    bool isConnected();
    const char* stateText();     // "desativado" | "conectando" | "conectado" | "erro"
    String lastError();
    void statusJson(String& out);
}
