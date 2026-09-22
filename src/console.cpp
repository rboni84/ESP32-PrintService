#include "console.h"
#include "portal.h"
#include "printers.h"
#include "printjob.h"
#include "cloud.h"

#include <WiFi.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace {

DeviceConfig* g_cfg = nullptr;

enum class State {
    Menu,          // aguardando opcao do menu
    AskSsid,       // 2) SSID da rede WiFi
    AskWifiPass,   //    senha da rede WiFi
    AskApPass,     // 3) senha do AP
    AskAdminPass,  // 4) senha do portal web
    Scanning,      // 5) scan assincrono em andamento
    ConfirmReset,  // 7) confirmacao do reset de fabrica
    AskAddIp,      // a) IP de impressora a adicionar
    AskRemoveIp,   // d) IP de impressora a remover
    AskCommunity,  // c) community SNMP
    AskTestIp,     // t) IP da impressora para pagina de teste
    AskTestFormat, //    formato da pagina de teste
    AskCloudUrl,   // s) URL do servidor externo
    AskCloudToken, //    token do servidor externo
};

State state = State::Menu;
String line;            // linha em digitacao
String pendingSsid;     // SSID informado, aguardando senha
String pendingValue;    // valor intermediario (IP da pagina de teste, URL do servidor)
bool hideEcho = false;  // ecoa '*' no lugar dos caracteres (senhas)
char lastEol = 0;       // trata CR+LF como um unico fim de linha

const size_t MAX_LINE = 96;

// ---------- saida ----------

void prompt(const char* text) {
    Serial.print(text);
    Serial.print("> ");
}

void backToMenu() {
    state = State::Menu;
    hideEcho = false;
    prompt("menu");
}

void printStatusImpl() {
    bool sta = WiFi.status() == WL_CONNECTED;
    Serial.println();
    Serial.println("---------------- STATUS ----------------");
    Serial.printf("Firmware      : v%s\n", FW_VERSION);
    Serial.printf("Dispositivo   : %s (fixo, derivado do MAC)\n", g_cfg->deviceName.c_str());
    Serial.printf("MAC           : %s\n", WiFi.macAddress().c_str());
    Serial.printf("Uptime        : %lus   Heap livre: %u bytes\n", millis() / 1000, ESP.getFreeHeap());
    Serial.println("-- Rede WiFi (STA)");
    Serial.printf("SSID          : %s\n", g_cfg->hasWifi() ? g_cfg->wifiSsid.c_str() : "(nao configurado)");
    Serial.printf("Estado        : %s\n", sta ? "conectado" : (g_cfg->hasWifi() ? "desconectado" : "-"));
    if (sta) {
        Serial.printf("IP            : %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("Gateway       : %s\n", WiFi.gatewayIP().toString().c_str());
        Serial.printf("RSSI          : %d dBm\n", WiFi.RSSI());
        Serial.printf("Portal        : http://%s/  ou  http://%s.local/\n",
                      WiFi.localIP().toString().c_str(), g_cfg->deviceName.c_str());
    }
    Serial.println("-- AP de configuracao");
    Serial.printf("Estado        : %s\n", Portal::isApMode() ? "ativo" : "desligado");
    Serial.printf("SSID          : %s\n", g_cfg->deviceName.c_str());
    if (g_cfg->apPass.length() >= 8) Serial.printf("Senha         : %s\n", g_cfg->apPass.c_str());
    else                             Serial.println("Senha         : (rede aberta)");
    if (Portal::isApMode()) {
        Serial.printf("IP            : %s   clientes: %d\n", WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum());
    }
    Serial.println("-- Portal web");
    Serial.printf("Autenticacao  : %s\n", g_cfg->adminPass.isEmpty() ? "sem senha" : "usuario 'admin' com senha");
    Serial.println("-- Servidor externo (WebSocket)");
    Serial.printf("URL           : %s\n", g_cfg->hasCloud() ? g_cfg->cloudUrl.c_str() : "(nao configurado)");
    Serial.printf("Token         : %s\n", g_cfg->cloudToken.length() ? "definido" : "(nenhum)");
    Serial.printf("Estado        : %s%s%s\n", Cloud::stateText(), Cloud::lastError().length() ? " - " : "",
                  Cloud::lastError().c_str());
    Serial.println("-- Impressao");
    {
        const PrintJob::Info& jb = PrintJob::info();
        if (jb.state == PrintJob::State::Idle) Serial.println("Trabalho      : nenhum");
        else Serial.printf("Trabalho      : %s -> %s:%u (%s)  %s  %u bytes%s%s%s%s\n", jb.id.c_str(), jb.ip.toString().c_str(), jb.port,
                           PrintJob::transportText(jb.transport),
                           PrintJob::busy() ? "em curso" : (jb.state == PrintJob::State::Done ? "concluido" : "erro"),
                           (unsigned)jb.written, jb.error.length() ? " - " : "", jb.error.c_str(),
                           jb.detail.length() ? " - " : "", jb.detail.c_str());
    }
    Serial.println("----------------------------------------");
}

void printMenuImpl() {
    Serial.println();
    Serial.println("========== PrintService - menu ==========");
    Serial.println(" 1) Mostrar status");
    Serial.println(" 2) Configurar rede WiFi (SSID e senha)");
    Serial.println(" 3) Senha do AP de configuracao");
    Serial.println(" 4) Senha do portal web (usuario admin)");
    Serial.println(" 5) Buscar redes WiFi");
    Serial.println(" 6) Reiniciar");
    Serial.println(" 7) Restaurar padroes de fabrica");
    Serial.println(" 8) Listar impressoras");
    Serial.println(" a) Adicionar impressora por IP");
    Serial.println(" d) Remover impressora");
    Serial.println(" c) Community SNMP");
    Serial.println(" r) Forcar nova descoberta/sondagem");
    Serial.println(" t) Imprimir pagina de teste");
    Serial.println(" s) Servidor externo (URL WebSocket e token)");
    Serial.println(" h) ou 'menu': mostrar este menu");
    Serial.println("=========================================");
}

// ---------- scan ----------

void startScan() {
    Serial.println("Buscando redes (aguarde)...");
    WiFi.scanDelete();
    WiFi.scanNetworks(true /*async*/, false /*hidden*/);
    state = State::Scanning;
}

void pollScan() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    if (n < 0) {
        Serial.println("Falha ao buscar redes.");
    } else {
        Serial.printf("%d rede(s) encontrada(s):\n", n);
        for (int i = 0; i < n; i++) {
            Serial.printf("  %2d) %-32s %4d dBm  ch%-2d %s\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                          WiFi.channel(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "aberta" : "protegida");
        }
    }
    WiFi.scanDelete();
    backToMenu();
}

// ---------- tratamento de linhas ----------

void handleMenu(const String& cmd) {
    if (cmd.isEmpty()) { prompt("menu"); return; }
    if (cmd.equalsIgnoreCase("menu") || cmd.equalsIgnoreCase("help")) {
        printMenuImpl();
        prompt("menu");
        return;
    }
    if (cmd.length() > 1) {
        Serial.println("Comando desconhecido. Digite 'menu' para ver as opcoes.");
        prompt("menu");
        return;
    }
    switch (cmd[0]) {
        case '1':
            printStatusImpl();
            prompt("menu");
            break;
        case '2':
            Serial.println("SSID da rede WiFi (vazio = cancelar)");
            state = State::AskSsid;
            prompt("ssid");
            break;
        case '3':
            Serial.println("Senha do AP: min. 8 caracteres; '-' = rede aberta; vazio = cancelar");
            Serial.printf("Senha atual: %s\n", g_cfg->apPass.length() >= 8 ? g_cfg->apPass.c_str() : "(rede aberta)");
            state = State::AskApPass;
            hideEcho = true;
            prompt("senha AP");
            break;
        case '4':
            Serial.println("Senha do portal web: min. 4 caracteres; '-' = sem senha; vazio = cancelar");
            state = State::AskAdminPass;
            hideEcho = true;
            prompt("senha portal");
            break;
        case '5':
            startScan();
            break;
        case '6':
            Serial.println("Reiniciando...");
            Portal::scheduleRestart(500);
            break;
        case '7':
            Serial.println("ATENCAO: apaga WiFi, senhas, community e impressoras cadastradas. Digite SIM para confirmar.");
            state = State::ConfirmReset;
            prompt("confirmar");
            break;
        case '8':
            PrinterMonitor::printList(Serial);
            prompt("menu");
            break;
        case 'a': case 'A':
            Serial.println("IP da impressora (vazio = cancelar)");
            state = State::AskAddIp;
            prompt("ip");
            break;
        case 'd': case 'D':
            PrinterMonitor::printList(Serial);
            Serial.println("IP da impressora a remover (vazio = cancelar)");
            state = State::AskRemoveIp;
            prompt("ip");
            break;
        case 'c': case 'C':
            Serial.printf("Community SNMP [%s] (vazio = cancelar)\n", g_cfg->snmpCommunity.c_str());
            state = State::AskCommunity;
            prompt("community");
            break;
        case 'r': case 'R':
            PrinterMonitor::refreshNow();
            Serial.println("Descoberta mDNS e sondagem SNMP reagendadas.");
            prompt("menu");
            break;
        case 't': case 'T':
            if (PrintJob::busy()) { Serial.println("Ja existe um trabalho de impressao em curso."); prompt("menu"); break; }
            PrinterMonitor::printList(Serial);
            Serial.println("IP da impressora para a pagina de teste (vazio = cancelar)");
            state = State::AskTestIp;
            prompt("ip");
            break;
        case 's': case 'S':
            Serial.printf("URL WebSocket do servidor [%s] (ws:// ou wss://; '-' = desativar; vazio = manter)\n",
                          g_cfg->hasCloud() ? g_cfg->cloudUrl.c_str() : "nao configurado");
            state = State::AskCloudUrl;
            prompt("url");
            break;
        case 'h': case 'H': case '?':
            printMenuImpl();
            prompt("menu");
            break;
        default:
            Serial.println("Opcao invalida. Digite 'menu' para ver as opcoes.");
            prompt("menu");
    }
}

void handleLine(String l) {
    // Senhas podem conter espacos internos; nas demais entradas o trim e desejavel.
    if (state != State::AskWifiPass && state != State::AskApPass && state != State::AskAdminPass &&
        state != State::AskCloudToken) l.trim();

    switch (state) {
        case State::Menu:
            handleMenu(l);
            break;

        case State::AskSsid:
            if (l.isEmpty()) { Serial.println("Cancelado."); backToMenu(); break; }
            if (l.length() > 32) { Serial.println("SSID muito longo (max 32)."); prompt("ssid"); break; }
            pendingSsid = l;
            Serial.println("Senha da rede (vazio = rede aberta)");
            state = State::AskWifiPass;
            hideEcho = true;
            prompt("senha");
            break;

        case State::AskWifiPass:
            if (!l.isEmpty() && l.length() < 8) { Serial.println("Senha WiFi deve ter ao menos 8 caracteres."); prompt("senha"); break; }
            if (l.length() > 63) { Serial.println("Senha muito longa (max 63)."); prompt("senha"); break; }
            g_cfg->wifiSsid = pendingSsid;
            g_cfg->wifiPass = l;
            Config::save(*g_cfg);
            Serial.printf("WiFi salvo: '%s'. Reiniciando para conectar...\n", pendingSsid.c_str());
            hideEcho = false;
            state = State::Menu;
            Portal::scheduleRestart();
            break;

        case State::AskApPass:
            if (l.isEmpty()) { Serial.println("Cancelado."); backToMenu(); break; }
            if (l == "-") {
                g_cfg->apPass = "";
                Config::save(*g_cfg);
                Serial.println("AP ficara ABERTO apos reiniciar (opcao 6).");
                backToMenu();
                break;
            }
            if (l.length() < 8 || l.length() > 63) { Serial.println("Senha do AP deve ter 8 a 63 caracteres."); prompt("senha AP"); break; }
            g_cfg->apPass = l;
            Config::save(*g_cfg);
            Serial.println("Senha do AP salva. Aplica apos reiniciar (opcao 6).");
            backToMenu();
            break;

        case State::AskAdminPass:
            if (l.isEmpty()) { Serial.println("Cancelado."); backToMenu(); break; }
            if (l == "-") {
                g_cfg->adminPass = "";
                Config::save(*g_cfg);
                Serial.println("Portal web sem senha.");
                backToMenu();
                break;
            }
            if (l.length() < 4 || l.length() > 63) { Serial.println("Senha do portal deve ter 4 a 63 caracteres."); prompt("senha portal"); break; }
            g_cfg->adminPass = l;
            Config::save(*g_cfg);
            Serial.println("Senha do portal salva (usuario: admin). Vale imediatamente.");
            backToMenu();
            break;

        case State::ConfirmReset:
            if (l == "SIM") {
                Serial.println("Restaurando padroes e reiniciando...");
                state = State::Menu;
                Portal::factoryReset();
            } else {
                Serial.println("Cancelado.");
                backToMenu();
            }
            break;

        case State::AskAddIp: {
            if (l.isEmpty()) { Serial.println("Cancelado."); backToMenu(); break; }
            String err;
            if (PrinterMonitor::addManual(l, &err)) Serial.println("Impressora adicionada; sondagem em instantes.");
            else Serial.printf("Falha: %s.\n", err.c_str());
            backToMenu();
            break;
        }

        case State::AskRemoveIp:
            if (l.isEmpty()) { Serial.println("Cancelado."); backToMenu(); break; }
            Serial.println(PrinterMonitor::remove(l) ? "Impressora removida." : "IP nao encontrado.");
            backToMenu();
            break;

        case State::AskCommunity:
            if (l.isEmpty()) { Serial.println("Cancelado."); backToMenu(); break; }
            if (l.length() > 32) { Serial.println("Muito longa (max 32)."); prompt("community"); break; }
            g_cfg->snmpCommunity = l;
            Config::save(*g_cfg);
            PrinterMonitor::refreshNow();
            Serial.printf("Community salva: '%s'.\n", l.c_str());
            backToMenu();
            break;

        case State::AskTestIp: {
            if (l.isEmpty()) { Serial.println("Cancelado."); backToMenu(); break; }
            IPAddress ip;
            if (!ip.fromString(l)) { Serial.println("IP invalido."); prompt("ip"); break; }
            pendingValue = l;
            Serial.println("Formato: pcl, text, ps ou pdf (vazio = automatico: pdf via IPP, pcl via porta 9100)");
            state = State::AskTestFormat;
            prompt("formato");
            break;
        }

        case State::AskTestFormat: {
            IPAddress ip; ip.fromString(pendingValue);
            PrintJob::Target t;
            PrinterMonitor::fillTarget(ip, t);
            String fmt = l.isEmpty() ? "auto" : l;
            String err;
            if (PrintJob::printTest(t, fmt, "", "serial-test", &err)) {
                const PrintJob::Info& jb = PrintJob::info();
                Serial.printf("Pagina de teste enviada para %s:%u via %s (%s). Acompanhe pelo LED (ciano) e pela opcao 1.\n",
                              pendingValue.c_str(), jb.port, PrintJob::transportText(jb.transport), jb.name.c_str());
            } else {
                Serial.printf("Falha: %s%s%s.\n", err.c_str(), PrintJob::info().detail.length() ? " - " : "",
                              PrintJob::info().detail.c_str());
            }
            backToMenu();
            break;
        }

        case State::AskCloudUrl:
            if (l == "-") {
                g_cfg->cloudUrl = "";
                Config::save(*g_cfg);
                Cloud::reconfigure();
                Serial.println("Servidor externo desativado.");
                backToMenu();
                break;
            }
            if (l.length()) {
                if (!(l.startsWith("ws://") || l.startsWith("wss://"))) { Serial.println("URL deve comecar com ws:// ou wss://."); prompt("url"); break; }
                if (l.length() > 200) { Serial.println("URL muito longa (max 200)."); prompt("url"); break; }
                pendingValue = l;
            } else pendingValue = g_cfg->cloudUrl;
            Serial.printf("Token de acesso [%s] ('-' = remover; vazio = manter)\n", g_cfg->cloudToken.length() ? "definido" : "nenhum");
            state = State::AskCloudToken;
            hideEcho = true;
            prompt("token");
            break;

        case State::AskCloudToken:
            if (l == "-") g_cfg->cloudToken = "";
            else if (l.length()) { if (l.length() > 128) { Serial.println("Token muito longo (max 128)."); prompt("token"); break; } g_cfg->cloudToken = l; }
            g_cfg->cloudUrl = pendingValue;
            Config::save(*g_cfg);
            Cloud::reconfigure();
            Serial.printf("Servidor salvo: %s (token %s). Reconectando...\n",
                          g_cfg->hasCloud() ? g_cfg->cloudUrl.c_str() : "desativado",
                          g_cfg->cloudToken.length() ? "definido" : "nenhum");
            backToMenu();
            break;

        case State::Scanning:
            // Entrada ignorada enquanto o scan roda.
            break;
    }
}

void processChar(char c) {
    if (c == '\r' || c == '\n') {
        bool duplicateEol = (c == '\n' && lastEol == '\r');
        lastEol = c;
        if (duplicateEol) return;
        Serial.println();
        String l = line;
        line = "";
        handleLine(l);
        return;
    }
    lastEol = 0;
    if (c == 0x08 || c == 0x7F) {  // backspace / delete
        if (line.length()) {
            line.remove(line.length() - 1);
            Serial.print("\b \b");
        }
        return;
    }
    if (c == 0x1B) return;  // ESC e sequencias de seta: ignora o byte inicial
    if ((uint8_t)c < 0x20) return;
    if (line.length() >= MAX_LINE) return;
    line += c;
    Serial.print(hideEcho ? '*' : c);
}

}  // namespace

// ---------- API publica ----------

void Console::begin(DeviceConfig& cfg) {
    g_cfg = &cfg;
    Serial.println();
    Serial.println("Digite 'menu' + Enter para abrir o menu de configuracao.");
}

void Console::loop() {
    if (state == State::Scanning) pollScan();
    while (Serial.available() > 0) processChar((char)Serial.read());
}

void Console::printMenu() { printMenuImpl(); }
void Console::printStatus() { printStatusImpl(); }
