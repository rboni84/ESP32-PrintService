#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include "config.h"
#include "printjob.h"

// Fase 2: monitoramento (SNMP) das impressoras cadastradas e busca (mDNS) sob demanda.
//  - lista monitorada: apenas impressoras incluidas pelo operador (portal, serial, WebSocket),
//    persistidas em NVS por IP. Nada entra nela automaticamente.
//  - busca mDNS: so quando solicitada (modal "Incluir impressora", opcao 'f', comando WS).
//    Produz uma lista leve (nome, IP, portas anunciadas) de ate MAX_FOUND resultados, liberada
//    ao fechar o modal ou apos FOUND_TTL; economiza heap em relacao a descoberta continua.
//  - SNMP: HOST-RESOURCES-MIB (hrDeviceStatus, hrPrinterStatus, hrPrinterDetectedErrorState)
//          e Printer-MIB (prtMarkerLifeCount, prtMarkerSuppliesTable, prtInterpreterTable) via
//          GET/GETNEXT, um dispositivo por vez, sem bloquear o loop.
namespace PrinterMonitor {

const uint8_t MAX_PRINTERS = 16;
const uint8_t MAX_SUPPLIES = 8;
const uint8_t MAX_FOUND = 32;

// Resultado leve da busca mDNS (nao e monitorado ate ser incluido).
struct Found {
    IPAddress ip;
    String name;      // instancia mDNS (ex.: "EPSON L6270 Series")
    String host;      // hostname
    String model;     // TXT "ty"
    uint16_t rawPort = 0, ippPort = 0, lpdPort = 0;   // portas anunciadas (0 = nao)
    String ippPath, lpdQueue;                          // TXT "rp" de _ipp e _printer
};

struct Supply {
    String desc;          // prtMarkerSuppliesDescription
    int32_t level = -2;   // prtMarkerSuppliesLevel (-1 outro, -2 desconhecido, -3 "algum restante")
    int32_t max = -2;     // prtMarkerSuppliesMaxCapacity (-1 outro, -2 desconhecido)
    uint8_t cls = 0;      // prtMarkerSuppliesClass: 3 consumido (toner), 4 receptaculo (residuo)
    uint8_t type = 0;     // prtMarkerSuppliesType: 3 toner, 4 wasteToner, 5 ink, 15 opc (cilindro)...
    int percent() const { return (level >= 0 && max > 0) ? (int)((int64_t)level * 100 / max) : -1; }
};

struct Printer {
    IPAddress ip;
    String host;          // hostname mDNS ou sysName
    String name;          // prtGeneralPrinterName / instancia mDNS
    String model;         // TXT "ty" / sysDescr
    String location;      // TXT "note" / sysLocation
    String pdl;           // linguagens aceitas, separadas por virgula: PDF,PS,PCL,PCLXL,PJL,URF,PWG,TEXT...
                          // (TXT "pdl" do mDNS/IPP + prtInterpreterLangFamily via SNMP)
    uint16_t ippPort = 0; // portas anunciadas via mDNS (0 = nao anunciada)
    uint16_t rawPort = 0; //   _pdl-datastream (JetDirect)
    uint16_t lpdPort = 0; //   _printer (LPD)
    String ippPath;       // TXT "rp" do _ipp (ex.: "/ipp/print")
    String lpdQueue;      // TXT "rp" do _printer (fila LPD, ex.: "lp", "PASSTHRU", "BINARY_P1")
    bool manual = false;  // incluida pelo operador (persistida); hoje toda impressora monitorada e manual
    bool viaMdns = false; // incluida a partir da busca mDNS (portas/nome vieram do anuncio)
    uint8_t snmpVersion = 1;  // 1 = v2c, 0 = v1 (fallback apos timeouts)

    bool online = false;      // respondeu SNMP na ultima sondagem
    bool everOnline = false;
    uint8_t fails = 0;
    int deviceStatus = 0;     // hrDeviceStatus: 1 unknown 2 running 3 warning 4 testing 5 down
    int printerStatus = 0;    // hrPrinterStatus: 1 other 2 unknown 3 idle 4 printing 5 warmup
    uint16_t errorState = 0;  // hrPrinterDetectedErrorState (byte0<<8 | byte1)
    uint32_t pageCount = 0;
    bool hasPageCount = false;
    Supply supplies[MAX_SUPPLIES];
    uint8_t supplyCount = 0;

    uint32_t lastPoll = 0;
    uint32_t lastOk = 0;

    bool hasAlert() const;
    bool isOfflineKnown() const;   // offline confirmado (3 falhas seguidas), nao "ainda nao sondada"
};

void begin(DeviceConfig& cfg);
void loop();

// Consulta
uint8_t count();
uint8_t onlineCount();
uint8_t alertCount();
const Printer* get(uint8_t i);
void toJson(String& out);
void printList(Print& out);
const char* deviceStatusText(int v);
const char* printerStatusText(int v);
String errorStateText(uint16_t bits);  // lista separada por virgula (vazio = sem erros)

// Destino de impressao para um IP: portas e caminho IPP anunciados (ou padroes 9100/631).
void fillTarget(const IPAddress& ip, PrintJob::Target& t);

// Busca mDNS sob demanda (~9 s: tres servicos, 3 s cada)
bool startDiscovery(String* err);   // false se sem rede, mDNS indisponivel ou busca ja em curso
bool isDiscovering();
uint8_t foundCount();
const Found* foundAt(uint8_t i);
void discoveryJson(String& out);    // {"running","count","found":[{ip,name,host,model,ports,ipp_path,lpd_queue}]}
void printFound(Print& out);
void clearDiscovery();              // libera a lista de resultados

// Acoes
bool addManual(const String& ipText, String* err);   // usa portas/nome da busca, se o IP estiver nela
bool remove(const String& ipText);
void refreshNow();  // forca sondagem SNMP imediata de todas (nao dispara busca mDNS)
}  // namespace PrinterMonitor
