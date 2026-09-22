#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

// Cliente SNMP v1/v2c minimo e nao bloqueante (GET / GETNEXT) sobre UDP.
// Uso: request(...) -> chamar poll() em loop ate retornar Done/Timeout/Error -> ler results().
namespace Snmp {

enum class VarType : uint8_t {
    Null, Integer, String, Oid, IpAddress, Counter, Gauge, TimeTicks, Counter64,
    NoSuchObject, NoSuchInstance, EndOfMib, Unknown
};

struct VarBind {
    String oid;                    // "1.3.6.1.2.1.1.1.0"
    VarType type = VarType::Null;
    int64_t num = 0;               // Integer/Counter/Gauge/TimeTicks/Counter64
    String str;                    // OCTET STRING (bytes crus), OID (texto) ou IpAddress (a.b.c.d)

    bool isNumeric() const {
        return type == VarType::Integer || type == VarType::Counter || type == VarType::Gauge ||
               type == VarType::TimeTicks || type == VarType::Counter64;
    }
    bool isException() const {
        return type == VarType::NoSuchObject || type == VarType::NoSuchInstance || type == VarType::EndOfMib;
    }
};

enum class Status { Idle, Pending, Done, Timeout, Error };

class Client {
public:
    static const uint8_t MAX_VARS = 8;

    // version: 0 = SNMPv1, 1 = SNMPv2c
    bool request(const IPAddress& ip, uint16_t port, const String& community, uint8_t version,
                 bool getNext, const String* oids, uint8_t count, uint32_t timeoutMs = 2000);
    Status poll();
    void cancel();

    bool busy() const { return status_ == Status::Pending; }
    const VarBind* results() const { return vb_; }
    uint8_t resultCount() const { return vbCount_; }
    int errorStatus() const { return errStatus_; }   // error-status do PDU (0 = ok, 5 = genErr, ...)

private:
    WiFiUDP udp_;
    bool started_ = false;
    Status status_ = Status::Idle;
    uint32_t sentAt_ = 0, timeout_ = 0;
    int32_t reqId_ = 0;
    int errStatus_ = 0;
    VarBind vb_[MAX_VARS];
    uint8_t vbCount_ = 0;

    bool parseResponse(const uint8_t* p, size_t len);
};

}  // namespace Snmp
