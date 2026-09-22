#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// Motor de impressao raw (JetDirect/AppSocket, porta 9100). Um trabalho por vez.
// Os dados chegam em blocos (WebSocket do servidor externo ou upload local), sao
// enfileirados em um buffer e drenados para a impressora em loop(), sem bloquear.
namespace PrintJob {

const size_t BUFFER_SIZE = 32 * 1024;   // capacidade do buffer de saida
const size_t CHUNK_MAX = 8 * 1024;      // maior bloco binario aceito por vez (contrato WS)
const uint16_t DEFAULT_PORT = 9100;

enum class State : uint8_t { Idle, Streaming, Finishing, Done, Error };

struct Info {
    String id;          // identificador do trabalho (definido pelo solicitante)
    String name;        // nome descritivo (ex.: arquivo)
    String source;      // "cloud", "local", "test"
    IPAddress ip;
    uint16_t port = DEFAULT_PORT;
    size_t expected = 0;   // tamanho anunciado (0 = desconhecido)
    size_t received = 0;   // bytes recebidos do solicitante
    size_t written = 0;    // bytes entregues a impressora
    State state = State::Idle;
    String error;          // codigo curto: connect_failed, write_failed, timeout, canceled, overflow
    uint32_t startedAt = 0, finishedAt = 0;
};

typedef void (*DoneCallback)(const Info& info);

// Abre a conexao com a impressora. Falha se ja houver trabalho ativo (err = "busy").
bool start(const String& id, const IPAddress& ip, uint16_t port, size_t expected,
           const String& name, const String& source, String* err);
size_t freeSpace();                                 // bytes que write() ainda aceita agora
bool write(const uint8_t* data, size_t len);        // enfileira; false se nao couber
void finish();                                      // sem mais dados: fecha apos drenar
void cancel(const char* reason);                    // aborta e fecha
void loop();
bool busy();
const Info& info();
void statusJson(String& out);
void onDone(DoneCallback cb);   // chamado ao terminar (Done ou Error); um unico ouvinte

// Pagina de teste. format: "pcl" (padrao: texto com reset PCL), "text" (texto puro + FF) ou "ps" (PostScript).
bool printTest(const IPAddress& ip, uint16_t port, const String& format, const String& id,
               const String& source, String* err);

}  // namespace PrintJob
