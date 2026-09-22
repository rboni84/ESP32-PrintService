#include "snmp.h"
#include <vector>

namespace Snmp {

namespace {

// ---------- codificacao BER ----------

void putLen(std::vector<uint8_t>& o, size_t n) {
    if (n < 0x80) { o.push_back((uint8_t)n); }
    else if (n < 0x100) { o.push_back(0x81); o.push_back((uint8_t)n); }
    else { o.push_back(0x82); o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)n); }
}

void putTlv(std::vector<uint8_t>& o, uint8_t tag, const std::vector<uint8_t>& v) {
    o.push_back(tag);
    putLen(o, v.size());
    o.insert(o.end(), v.begin(), v.end());
}

void putInt(std::vector<uint8_t>& o, int32_t v) {
    uint8_t tmp[4];
    for (int i = 3; i >= 0; i--) { tmp[i] = (uint8_t)(v & 0xFF); v >>= 8; }
    // remove bytes redundantes de sinal (complemento de dois minimo)
    int start = 0;
    while (start < 3 && ((tmp[start] == 0x00 && !(tmp[start + 1] & 0x80)) ||
                         (tmp[start] == 0xFF && (tmp[start + 1] & 0x80)))) start++;
    o.push_back(0x02);
    o.push_back((uint8_t)(4 - start));
    for (int i = start; i < 4; i++) o.push_back(tmp[i]);
}

void putStr(std::vector<uint8_t>& o, const String& s) {
    o.push_back(0x04);
    putLen(o, s.length());
    o.insert(o.end(), (const uint8_t*)s.c_str(), (const uint8_t*)s.c_str() + s.length());
}

void putSubId(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t tmp[5]; int n = 0;
    do { tmp[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v);
    for (int i = n - 1; i >= 0; i--) o.push_back(tmp[i] | (i ? 0x80 : 0x00));
}

bool putOid(std::vector<uint8_t>& o, const String& oid) {
    std::vector<uint32_t> ids;
    size_t i = 0;
    while (i < oid.length()) {
        if (oid[i] == '.') { i++; continue; }
        uint32_t v = 0; bool any = false;
        while (i < oid.length() && isdigit((unsigned char)oid[i])) { v = v * 10 + (oid[i] - '0'); i++; any = true; }
        if (!any) return false;
        ids.push_back(v);
    }
    if (ids.size() < 2 || ids[0] > 2 || ids[1] > 39) return false;
    std::vector<uint8_t> body;
    putSubId(body, ids[0] * 40 + ids[1]);
    for (size_t k = 2; k < ids.size(); k++) putSubId(body, ids[k]);
    putTlv(o, 0x06, body);
    return true;
}

// ---------- decodificacao BER ----------

struct Reader {
    const uint8_t* p; size_t len; size_t pos = 0;
    Reader(const uint8_t* b, size_t n) : p(b), len(n) {}
    bool eof() const { return pos >= len; }
    bool tl(uint8_t& tag, size_t& l) {
        if (pos + 2 > len) return false;
        tag = p[pos++];
        uint8_t b = p[pos++];
        if (b < 0x80) { l = b; }
        else {
            uint8_t n = b & 0x7F;
            if (n == 0 || n > 4 || pos + n > len) return false;
            l = 0;
            for (uint8_t i = 0; i < n; i++) l = (l << 8) | p[pos++];
        }
        return pos + l <= len;
    }
};

int64_t decodeInt(const uint8_t* b, size_t l, bool signedInt) {
    if (l == 0) return 0;
    int64_t v = (signedInt && (b[0] & 0x80)) ? -1 : 0;
    for (size_t i = 0; i < l && i < 9; i++) v = (v << 8) | b[i];
    return v;
}

String decodeOid(const uint8_t* b, size_t l) {
    String s;
    if (l == 0) return s;
    size_t i = 0;
    uint32_t v = 0;
    // primeiro subidentificador codifica dois arcos
    while (i < l) { v = (v << 7) | (b[i] & 0x7F); if (!(b[i++] & 0x80)) break; }
    if (v < 40) { s += "0."; s += v; }
    else if (v < 80) { s += "1."; s += (v - 40); }
    else { s += "2."; s += (v - 80); }
    v = 0;
    while (i < l) {
        v = (v << 7) | (b[i] & 0x7F);
        if (!(b[i++] & 0x80)) { s += '.'; s += v; v = 0; }
    }
    return s;
}

}  // namespace

// ---------- Client ----------

bool Client::request(const IPAddress& ip, uint16_t port, const String& community, uint8_t version,
                     bool getNext, const String* oids, uint8_t count, uint32_t timeoutMs) {
    if (status_ == Status::Pending || count == 0 || count > MAX_VARS) return false;
    if (!started_) { started_ = udp_.begin(0); if (!started_) return false; }

    std::vector<uint8_t> vbs;
    for (uint8_t i = 0; i < count; i++) {
        std::vector<uint8_t> one;
        if (!putOid(one, oids[i])) return false;
        one.push_back(0x05); one.push_back(0x00);  // NULL
        putTlv(vbs, 0x30, one);
    }
    reqId_ = (int32_t)(esp_random() & 0x7FFFFFFF);
    std::vector<uint8_t> pdu;
    putInt(pdu, reqId_);
    putInt(pdu, 0);  // error-status
    putInt(pdu, 0);  // error-index
    putTlv(pdu, 0x30, vbs);

    std::vector<uint8_t> msgBody;
    putInt(msgBody, version);
    putStr(msgBody, community);
    putTlv(msgBody, getNext ? 0xA1 : 0xA0, pdu);
    std::vector<uint8_t> msg;
    putTlv(msg, 0x30, msgBody);

    // descarta datagramas antigos pendentes
    while (udp_.parsePacket() > 0) udp_.clear();

    if (!udp_.beginPacket(ip, port)) return false;
    udp_.write(msg.data(), msg.size());
    if (!udp_.endPacket()) return false;

    vbCount_ = 0;
    errStatus_ = 0;
    sentAt_ = millis();
    timeout_ = timeoutMs;
    status_ = Status::Pending;
    return true;
}

void Client::cancel() { status_ = Status::Idle; }

Status Client::poll() {
    if (status_ != Status::Pending) return status_;
    int n = udp_.parsePacket();
    if (n > 0) {
        static uint8_t buf[1472];
        int r = udp_.read(buf, sizeof(buf));
        if (r > 0 && parseResponse(buf, (size_t)r)) { status_ = Status::Done; return status_; }
        // resposta de outro request (id diferente) ou malformada: continua aguardando
    }
    if (millis() - sentAt_ > timeout_) status_ = Status::Timeout;
    return status_;
}

bool Client::parseResponse(const uint8_t* p, size_t len) {
    Reader r(p, len);
    uint8_t tag; size_t l;
    if (!r.tl(tag, l) || tag != 0x30) return false;
    if (!r.tl(tag, l) || tag != 0x02) return false;  // version
    r.pos += l;
    if (!r.tl(tag, l) || tag != 0x04) return false;  // community
    r.pos += l;
    if (!r.tl(tag, l) || tag != 0xA2) return false;  // GetResponse
    if (!r.tl(tag, l) || tag != 0x02) return false;  // request-id
    int32_t id = (int32_t)decodeInt(p + r.pos, l, true);
    r.pos += l;
    if (id != reqId_) return false;
    if (!r.tl(tag, l) || tag != 0x02) return false;  // error-status
    errStatus_ = (int)decodeInt(p + r.pos, l, true);
    r.pos += l;
    if (!r.tl(tag, l) || tag != 0x02) return false;  // error-index
    r.pos += l;
    if (!r.tl(tag, l) || tag != 0x30) return false;  // varbind list
    size_t end = r.pos + l;

    vbCount_ = 0;
    while (r.pos < end && vbCount_ < MAX_VARS) {
        if (!r.tl(tag, l) || tag != 0x30) return false;
        size_t vbEnd = r.pos + l;
        if (!r.tl(tag, l) || tag != 0x06) return false;
        VarBind& v = vb_[vbCount_];
        v.oid = decodeOid(p + r.pos, l);
        v.num = 0; v.str = "";
        r.pos += l;
        if (!r.tl(tag, l)) return false;
        const uint8_t* val = p + r.pos;
        switch (tag) {
            case 0x02: v.type = VarType::Integer;   v.num = decodeInt(val, l, true); break;
            case 0x04: v.type = VarType::String;    v.str = String(); v.str.reserve(l); for (size_t i = 0; i < l; i++) v.str += (char)val[i]; break;
            case 0x05: v.type = VarType::Null; break;
            case 0x06: v.type = VarType::Oid;       v.str = decodeOid(val, l); break;
            case 0x40: v.type = VarType::IpAddress; if (l == 4) v.str = IPAddress(val[0], val[1], val[2], val[3]).toString(); break;
            case 0x41: v.type = VarType::Counter;   v.num = decodeInt(val, l, false); break;
            case 0x42: v.type = VarType::Gauge;     v.num = decodeInt(val, l, false); break;
            case 0x43: v.type = VarType::TimeTicks; v.num = decodeInt(val, l, false); break;
            case 0x46: v.type = VarType::Counter64; v.num = decodeInt(val, l, false); break;
            case 0x80: v.type = VarType::NoSuchObject; break;
            case 0x81: v.type = VarType::NoSuchInstance; break;
            case 0x82: v.type = VarType::EndOfMib; break;
            default:   v.type = VarType::Unknown; break;
        }
        r.pos = vbEnd;
        vbCount_++;
    }
    return true;
}

}  // namespace Snmp
