#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// Motor de impressao. Um trabalho por vez. Tres transportes:
//   - raw: JetDirect/AppSocket, porta 9100 (bytes entregues como estao)
//   - ipp: IPP/1.1 Print-Job sobre HTTP (porta 631), corpo em chunked; impressoras
//          domesticas/AirPrint que nao abrem a 9100
//   - lpd: LPR/LPD (RFC 1179), porta 515; exige o tamanho do documento antes dos dados
// Os dados chegam em blocos (WebSocket do servidor externo ou upload local), sao
// enfileirados em um buffer e drenados em loop(), sem bloquear.
namespace PrintJob {

const size_t BUFFER_SIZE = 32 * 1024;   // capacidade do buffer de saida
const size_t CHUNK_MAX = 8 * 1024;      // maior bloco binario aceito por vez (contrato WS)
const uint16_t DEFAULT_RAW_PORT = 9100;
const uint16_t DEFAULT_IPP_PORT = 631;
const uint16_t DEFAULT_LPD_PORT = 515;
const uint16_t DEFAULT_PORT = DEFAULT_RAW_PORT;

enum class Transport : uint8_t { Auto, Raw, Ipp, Lpd };
enum class State : uint8_t { Idle, Streaming, Finishing, Done, Error };

// Destino de um trabalho. PrinterMonitor::fillTarget() preenche a partir do que o mDNS anunciou.
struct Target {
    IPAddress ip;
    uint16_t rawPort = DEFAULT_RAW_PORT;
    uint16_t ippPort = DEFAULT_IPP_PORT;
    uint16_t lpdPort = DEFAULT_LPD_PORT;
    String ippPath = "/ipp/print";
    String lpdQueue = "lp";
    Transport transport = Transport::Auto;
    // portas anunciadas via mDNS: em Auto, sao tentadas primeiro (raw, ipp, lpd), depois as demais
    bool rawKnown = false, ippKnown = false, lpdKnown = false;
};

struct Info {
    String id;          // identificador do trabalho (definido pelo solicitante)
    String name;        // nome descritivo (ex.: arquivo)
    String source;      // "cloud", "cloud-test", "local", "local-test", "serial-test"
    IPAddress ip;
    uint16_t port = DEFAULT_RAW_PORT;       // porta efetivamente usada
    Transport transport = Transport::Raw;   // transporte efetivamente usado
    String docFormat;   // MIME enviado no IPP (document-format)
    size_t expected = 0;   // tamanho anunciado (0 = desconhecido; obrigatorio para LPD)
    size_t received = 0;   // bytes recebidos do solicitante
    size_t written = 0;    // bytes de documento entregues a impressora
    State state = State::Idle;
    String error;          // codigo curto (ver docs/api-contract.md)
    String detail;         // texto livre para diagnostico (errno, status HTTP/IPP, ack LPD)
    uint32_t startedAt = 0, finishedAt = 0;
};

typedef void (*DoneCallback)(const Info& info);

// Conecta a impressora (bloqueia ate 3 s por porta tentada; chamar so da task principal).
// Falha se ja houver trabalho ativo (err = "busy"). Os cabecalhos de protocolo sao enviados
// no primeiro write()/finish(), entao setDocument() pode ser chamado depois de start().
bool start(const String& id, const Target& target, size_t expected, const String& name,
           const String& source, const String& docFormat, String* err);
// Ajusta tamanho e MIME antes do primeiro write(). docFormat "" = application/octet-stream
// (application/pdf se o nome termina em .pdf).
void setDocument(size_t expected, const String& docFormat);
size_t freeSpace();                                 // bytes que write() ainda aceita agora
bool write(const uint8_t* data, size_t len);        // enfileira; false se nao couber
void finish();                                      // sem mais dados: fecha apos drenar
void cancel(const char* reason);                    // aborta e fecha
void loop();
bool busy();
const Info& info();
void statusJson(String& out);
void onDone(DoneCallback cb);   // chamado ao terminar (Done ou Error); um unico ouvinte

// Tempo maximo sem receber blocos do solicitante antes de cancelar com "data_timeout" (padrao 20 s).
void setDataTimeout(uint32_t ms);
uint32_t dataTimeout();

// Pagina de teste. format: "auto" (pdf se conectou por IPP, senao pcl), "pdf", "pcl", "text", "ps".
bool printTest(const Target& target, const String& format, const String& id, const String& source, String* err);

const char* transportText(Transport t);
Transport parseTransport(const String& s);   // "raw" | "ipp" | "lpd" | qualquer outro = Auto

}  // namespace PrintJob
