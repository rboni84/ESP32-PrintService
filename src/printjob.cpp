#include "printjob.h"
#include "printers.h"
#include "json_util.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <errno.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace PrintJob {

namespace {

WiFiClient client;
Info job;
Target tgt;                 // destino do trabalho corrente (para cabecalhos IPP/LPD)
DoneCallback doneCb = nullptr;

uint8_t buf[BUFFER_SIZE];
size_t bufHead = 0;   // proximo byte a enviar
size_t bufTail = 0;   // fim dos dados validos
bool finishRequested = false;
bool headerSent = false;    // IPP: HTTP + cabecalho IPP; LPD: handshake concluido (fase Data)
bool trailerSent = false;   // IPP: chunk final; LPD: byte 0 apos os dados
volatile bool starting = false;   // start() em curso (conexao bloqueante): statusJson evita ler 'job' parcial
uint32_t lastProgress = 0;

// resposta HTTP/IPP (transporte ipp)
char resp[512];
size_t respLen = 0;

// LPD (RFC 1179): handshake por etapas, cada uma aguardando um byte de ack = 0
enum class LpdPhase : uint8_t { None, AckRecv, AckCtrlHdr, AckCtrl, AckDataHdr, Data, AckData };
LpdPhase lpdPhase = LpdPhase::None;
String lpdJobNum;   // "001".."999"
String lpdHost;

const uint32_t CONNECT_TIMEOUT_MS = 3000;
const uint32_t STALL_TIMEOUT_MS = 20000;   // impressora sem consumir dados por 20 s -> write_timeout
uint32_t dataTimeoutMs = 20000;            // solicitante sem enviar blocos -> data_timeout (ajustavel)
const uint32_t CLOSE_GRACE_MS = 800;       // raw: espera apos o ultimo byte antes de fechar
const uint32_t RESPONSE_TIMEOUT_MS = 15000;   // ipp: resposta HTTP; lpd: cada ack

void compact() {
    if (bufHead == 0) return;
    size_t n = bufTail - bufHead;
    if (n) memmove(buf, buf + bufHead, n);
    bufHead = 0;
    bufTail = n;
}

void end(State st, const char* err) {
    client.stop();
    job.state = st;
    job.error = err ? err : "";
    job.finishedAt = millis();
    bufHead = bufTail = 0;
    finishRequested = false;
    headerSent = trailerSent = false;
    lpdPhase = LpdPhase::None;
    if (doneCb) doneCb(job);
}

void addDetail(const String& d) {
    if (d.isEmpty()) return;
    job.detail = job.detail.length() ? job.detail + "; " + d : d;
}

// Escreve tudo ou falha (NetworkClient::write bloqueia ate enviar ou errar).
bool writeAll(const uint8_t* data, size_t len) {
    while (len) {
        size_t n = client.write(data, len);
        if (n == 0) return false;
        data += n; len -= n;
    }
    return true;
}
bool writeAll(const String& s) { return writeAll((const uint8_t*)s.c_str(), s.length()); }

String defaultMime(const String& name) {
    String lower = name; lower.toLowerCase();
    return lower.endsWith(".pdf") ? "application/pdf" : "application/octet-stream";
}

// ---------- IPP ----------

void ippPut16(String& o, uint16_t v) { o += (char)(v >> 8); o += (char)(v & 0xFF); }

void ippAttr(String& o, uint8_t tag, const char* name, const String& value) {
    o += (char)tag;
    ippPut16(o, strlen(name));
    o += name;
    ippPut16(o, value.length());
    o += value;
}

// Cabecalho da operacao Print-Job (IPP/1.1). O documento segue imediatamente apos.
String ippPrintJobHeader(const Target& t, const String& jobName, const String& mime) {
    String o;
    o.reserve(256);
    o += (char)0x01; o += (char)0x01;          // version 1.1
    ippPut16(o, 0x0002);                       // Print-Job
    o += (char)0; o += (char)0; o += (char)0; o += (char)1;   // request-id
    o += (char)0x01;                           // operation-attributes-tag
    ippAttr(o, 0x47, "attributes-charset", "utf-8");
    ippAttr(o, 0x48, "attributes-natural-language", "en");
    ippAttr(o, 0x45, "printer-uri", "ipp://" + t.ip.toString() + ":" + String(t.ippPort) + t.ippPath);
    ippAttr(o, 0x42, "requesting-user-name", "PrintService");
    ippAttr(o, 0x42, "job-name", jobName.length() ? jobName.substring(0, 60) : String("PrintService"));
    ippAttr(o, 0x49, "document-format", mime);
    o += (char)0x03;                           // end-of-attributes
    return o;
}

bool ippSendChunk(const uint8_t* data, size_t len) {
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "%X\r\n", (unsigned)len);
    if (!writeAll((const uint8_t*)hdr, strlen(hdr))) return false;
    if (!writeAll(data, len)) return false;
    return writeAll((const uint8_t*)"\r\n", 2);
}

bool ippSendHeader() {
    if (job.docFormat.isEmpty()) job.docFormat = defaultMime(job.name);
    String ippHdr = ippPrintJobHeader(tgt, job.name, job.docFormat);
    String http = "POST " + tgt.ippPath + " HTTP/1.1\r\n"
                  "Host: " + tgt.ip.toString() + ":" + String(tgt.ippPort) + "\r\n"
                  "User-Agent: PrintService/" FW_VERSION "\r\n"
                  "Content-Type: application/ipp\r\n"
                  "Transfer-Encoding: chunked\r\n"
                  "Connection: close\r\n\r\n";
    return writeAll(http) && ippSendChunk((const uint8_t*)ippHdr.c_str(), ippHdr.length());
}

// Le o que houver da resposta; retorna true quando ja da para decidir.
bool ippReadResponse(bool& ok, String& detail) {
    while (client.available() && respLen < sizeof(resp) - 1) resp[respLen++] = (char)client.read();
    resp[respLen] = 0;
    if (respLen < 12) return false;
    int httpCode = 0;
    if (!strncmp(resp, "HTTP/1.", 7)) httpCode = atoi(resp + 9);
    else { ok = false; detail = "resposta HTTP invalida"; return true; }
    const char* body = strstr(resp, "\r\n\r\n");
    if (!body) return false;
    body += 4;
    size_t bodyLen = respLen - (body - resp);
    if (httpCode != 200) { ok = false; detail = "HTTP " + String(httpCode); return true; }
    if (bodyLen < 4) {
        if (!client.connected()) { ok = true; detail = "HTTP 200 sem corpo IPP"; return true; }
        return false;
    }
    uint16_t status = ((uint8_t)body[2] << 8) | (uint8_t)body[3];
    char sbuf[24]; snprintf(sbuf, sizeof(sbuf), "0x%04X", status);
    ok = status < 0x0300;
    detail = String("IPP ") + sbuf;
    if (status == 0x040A) detail += " (formato de documento nao suportado)";
    else if (status == 0x0507) detail += " (impressora nao aceita trabalhos agora)";
    return true;
}

// ---------- LPD ----------

const char* lpdPhaseName(LpdPhase p) {
    switch (p) {
        case LpdPhase::AckRecv:    return "receive-job";
        case LpdPhase::AckCtrlHdr: return "cabecalho do controle";
        case LpdPhase::AckCtrl:    return "arquivo de controle";
        case LpdPhase::AckDataHdr: return "cabecalho dos dados";
        case LpdPhase::AckData:    return "fim dos dados";
        default: return "-";
    }
}

bool lpdSendRecvJob() {
    lpdJobNum = String((millis() / 7) % 1000);
    while (lpdJobNum.length() < 3) lpdJobNum = "0" + lpdJobNum;
    lpdHost = WiFi.getHostname();
    if (lpdHost.isEmpty()) lpdHost = "PrintService";
    String cmd = "\x02" + tgt.lpdQueue + "\n";
    lpdPhase = LpdPhase::AckRecv;
    lastProgress = millis();
    return writeAll(cmd);
}

String lpdControlFile() {
    String df = "dfA" + lpdJobNum + lpdHost;
    String jn = job.name.length() ? job.name.substring(0, 99) : String("PrintService");
    String c;
    c += "H" + lpdHost + "\n";
    c += "PPrintService\n";
    c += "J" + jn + "\n";
    c += "N" + jn + "\n";
    c += "l" + df + "\n";     // 'l': imprime como esta, sem filtrar caracteres de controle
    c += "U" + df + "\n";
    return c;
}

// Avanca o handshake LPD quando o ack da etapa corrente chega. Retorna false em erro (ja encerrou).
bool lpdStep(uint32_t now) {
    if (!client.available()) {
        if (now - lastProgress > RESPONSE_TIMEOUT_MS) { addDetail(String("sem ack em ") + lpdPhaseName(lpdPhase)); end(State::Error, "response_timeout"); return false; }
        if (!client.connected()) { addDetail(String("conexao fechada em ") + lpdPhaseName(lpdPhase)); end(State::Error, "connection_closed"); return false; }
        return true;
    }
    int ack = client.read();
    if (ack != 0) {
        addDetail(String("LPD ack ") + ack + " em " + lpdPhaseName(lpdPhase) +
                  (lpdPhase == LpdPhase::AckRecv ? " (fila '" + tgt.lpdQueue + "' recusada?)" : ""));
        end(State::Error, "printer_rejected");
        return false;
    }
    lastProgress = now;
    bool ok = true;
    switch (lpdPhase) {
        case LpdPhase::AckRecv: {
            String ctrl = lpdControlFile();
            String hdr = "\x02" + String(ctrl.length()) + " cfA" + lpdJobNum + lpdHost + "\n";
            ok = writeAll(hdr);
            lpdPhase = LpdPhase::AckCtrlHdr;
            break;
        }
        case LpdPhase::AckCtrlHdr: {
            String ctrl = lpdControlFile();
            ok = writeAll(ctrl) && writeAll((const uint8_t*)"\0", 1);
            lpdPhase = LpdPhase::AckCtrl;
            break;
        }
        case LpdPhase::AckCtrl: {
            if (job.expected == 0) { addDetail("LPD exige o tamanho do documento (size)"); end(State::Error, "size_required"); return false; }
            String hdr = "\x03" + String(job.expected) + " dfA" + lpdJobNum + lpdHost + "\n";
            ok = writeAll(hdr);
            lpdPhase = LpdPhase::AckDataHdr;
            break;
        }
        case LpdPhase::AckDataHdr:
            lpdPhase = LpdPhase::Data;   // dados fluem pelo buffer normal
            headerSent = true;
            break;
        case LpdPhase::AckData:
            Serial.printf("[PRT] trabalho '%s' concluido via LPD: %u bytes em %lu ms\n", job.id.c_str(),
                          (unsigned)job.written, (unsigned long)(now - job.startedAt));
            addDetail("LPD aceito (fila " + tgt.lpdQueue + ")");
            end(State::Done, nullptr);
            return false;
        default: break;
    }
    if (!ok) { addDetail(String("falha ao enviar ") + lpdPhaseName(lpdPhase)); end(State::Error, "write_failed"); return false; }
    return true;
}

// ---------- conexao ----------

bool tryConnect(const IPAddress& ip, uint16_t port, String& detail) {
    client.stop();
    client.setTimeout(5000);
    errno = 0;
    if (client.connect(ip, port, CONNECT_TIMEOUT_MS)) return true;
    int e = errno;
    const char* why;
    switch (e) {
        case 0: case EINPROGRESS: case ETIMEDOUT: case EALREADY:
            why = "sem resposta em 3 s (porta filtrada, host desligado ou outra rede)"; break;
        case ECONNREFUSED:
            why = "conexao recusada (porta fechada na impressora)"; break;
        case ECONNRESET: case ECONNABORTED:
            why = "conexao encerrada pela impressora"; break;
        case EHOSTUNREACH: case ENETUNREACH:
            why = "host inalcancavel (rota/gateway)"; break;
        default:
            why = strerror(e); break;
    }
    if (detail.length()) detail += "; ";
    detail += String(port) + ": " + why + " [errno " + String(e) + "]";
    return false;
}

uint16_t portFor(const Target& t, Transport tr) {
    return tr == Transport::Raw ? t.rawPort : tr == Transport::Ipp ? t.ippPort : t.lpdPort;
}

// Ordem de tentativa em Auto: portas anunciadas primeiro (raw, ipp, lpd), depois as demais.
int buildOrder(const Target& t, Transport order[3]) {
    if (t.transport != Transport::Auto) { order[0] = t.transport; return 1; }
    int n = 0;
    const Transport all[3] = { Transport::Raw, Transport::Ipp, Transport::Lpd };
    const bool known[3] = { t.rawKnown, t.ippKnown, t.lpdKnown };
    for (int i = 0; i < 3; i++) if (known[i]) order[n++] = all[i];
    for (int i = 0; i < 3; i++) if (!known[i]) order[n++] = all[i];
    return n;
}

// Envia cabecalhos pendentes antes do primeiro dado. Retorna false se ainda nao pode enviar dados.
bool ensureHeader() {
    if (headerSent) return true;
    if (job.transport == Transport::Ipp) {
        if (!ippSendHeader()) { addDetail("cabecalho IPP"); end(State::Error, "write_failed"); return false; }
        headerSent = true;
        return true;
    }
    if (job.transport == Transport::Lpd) return false;   // headerSent vira true ao fim do handshake
    headerSent = true;   // raw: nada a enviar
    return true;
}

// ---------- pagina de teste ----------

String testPageText(const IPAddress& ip) {
    const PrinterMonitor::Printer* pr = nullptr;
    for (uint8_t i = 0; i < PrinterMonitor::count(); i++) {
        const PrinterMonitor::Printer* p = PrinterMonitor::get(i);
        if (p && p->ip == ip) { pr = p; break; }
    }
    String s;
    s.reserve(1200);
    s += "PrintService - Pagina de teste\r\n";
    s += "==============================================================\r\n\r\n";
    s += String("Dispositivo   : ") + WiFi.getHostname() + "\r\n";
    s += "Firmware      : v" FW_VERSION "\r\n";
    s += "MAC           : " + WiFi.macAddress() + "\r\n";
    s += "Rede          : " + WiFi.SSID() + "  IP " + WiFi.localIP().toString() + "\r\n";
    s += "Impressora    : " + ip.toString() + ":" + String(job.port) + " via " + transportText(job.transport) + "\r\n";
    if (pr) {
        if (pr->model.length())    s += "Modelo        : " + pr->model + "\r\n";
        if (pr->name.length())     s += "Nome          : " + pr->name + "\r\n";
        if (pr->location.length()) s += "Local         : " + pr->location + "\r\n";
        if (pr->pdl.length())      s += "Linguagens    : " + pr->pdl + "\r\n";
        if (pr->hasPageCount)      s += "Contador      : " + String(pr->pageCount) + " paginas\r\n";
        s += "Estado SNMP   : " + String(PrinterMonitor::deviceStatusText(pr->deviceStatus)) + " / " +
             PrinterMonitor::printerStatusText(pr->printerStatus) + "\r\n";
    }
    s += "Uptime        : " + String(millis() / 1000) + " s\r\n\r\n";
    s += "Teste de caracteres:\r\n";
    s += "  ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz\r\n";
    s += "  0123456789  !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\r\n\r\n";
    for (int i = 1; i <= 5; i++) {
        s += "  Linha " + String(i) + "  ";
        for (int k = 0; k < 10; k++) s += (i % 2) ? "=-" : "-=";
        s += "\r\n";
    }
    s += "\r\nSe voce consegue ler esta pagina, o caminho de impressao entre o\r\n";
    s += "PrintService e a impressora esta funcionando.\r\n";
    return s;
}

String escapeParens(const String& s) {
    String o;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '(' || c == ')' || c == '\\') o += '\\';
        o += ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) ? c : '?';
    }
    return o;
}

String testPagePostScript(const String& text) {
    String ps;
    ps.reserve(text.length() + 600);
    ps += "%!PS-Adobe-3.0\r\n%%Title: PrintService - Pagina de teste\r\n%%Pages: 1\r\n%%EndComments\r\n";
    ps += "/Courier findfont 10 scalefont setfont\r\n";
    ps += "/y 740 def\r\n/nl { /y y 13 sub def 60 y moveto } def\r\n60 y moveto\r\n";
    int start = 0;
    while (start < (int)text.length()) {
        int e = text.indexOf("\r\n", start);
        if (e < 0) e = text.length();
        ps += "(" + escapeParens(text.substring(start, e)) + ") show nl\r\n";
        start = e + 2;
    }
    ps += "showpage\r\n%%EOF\r\n";
    return ps;
}

// PDF 1.4 minimo, A4, Courier 10pt, uma pagina. Offsets do xref calculados ao montar.
String testPagePdf(const String& text) {
    String content;
    content.reserve(text.length() + 200);
    content += "BT /F1 10 Tf 13 TL 50 790 Td\n";
    int start = 0;
    while (start < (int)text.length()) {
        int e = text.indexOf("\r\n", start);
        if (e < 0) e = text.length();
        content += "(" + escapeParens(text.substring(start, e)) + ") Tj T*\n";
        start = e + 2;
    }
    content += "ET\n";

    String pdf;
    pdf.reserve(content.length() + 700);
    size_t off[6];
    pdf += "%PDF-1.4\n";
    off[1] = pdf.length(); pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    off[2] = pdf.length(); pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    off[3] = pdf.length(); pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                                  "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n";
    off[4] = pdf.length(); pdf += "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>\nendobj\n";
    off[5] = pdf.length(); pdf += "5 0 obj\n<< /Length " + String(content.length()) + " >>\nstream\n" + content + "endstream\nendobj\n";
    size_t xref = pdf.length();
    pdf += "xref\n0 6\n0000000000 65535 f \n";
    for (int i = 1; i <= 5; i++) { char l[24]; snprintf(l, sizeof(l), "%010u 00000 n \n", (unsigned)off[i]); pdf += l; }
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + String(xref) + "\n%%EOF\n";
    return pdf;
}

}  // namespace

// ---------- API ----------

void onDone(DoneCallback cb) { doneCb = cb; }
bool busy() { return job.state == State::Streaming || job.state == State::Finishing; }
const Info& info() { return job; }
const char* transportText(Transport t) {
    switch (t) { case Transport::Raw: return "raw"; case Transport::Ipp: return "ipp"; case Transport::Lpd: return "lpd"; default: return "auto"; }
}
Transport parseTransport(const String& s) {
    String v = s; v.toLowerCase();
    if (v == "raw" || v == "9100") return Transport::Raw;
    if (v == "ipp" || v == "631") return Transport::Ipp;
    if (v == "lpd" || v == "lpr" || v == "515") return Transport::Lpd;
    return Transport::Auto;
}

bool start(const String& id, const Target& target, size_t expected, const String& name,
           const String& source, const String& docFormat, String* err) {
    if (busy()) { if (err) *err = "busy"; return false; }
    if (!WiFi.isConnected()) { if (err) *err = "no_network"; return false; }

    struct StartingGuard { StartingGuard() { starting = true; } ~StartingGuard() { starting = false; } } guard;
    job = Info();
    job.id = id; job.name = name; job.source = source;
    job.ip = target.ip;
    job.expected = expected;
    job.docFormat = docFormat;
    job.startedAt = millis();
    tgt = target;
    bufHead = bufTail = 0;
    finishRequested = false;
    headerSent = trailerSent = false;
    lpdPhase = LpdPhase::None;
    respLen = 0;

    Transport order[3];
    int n = buildOrder(target, order);
    String detail;
    bool ok = false;
    for (int i = 0; i < n && !ok; i++) {
        uint16_t port = portFor(target, order[i]);
        if (tryConnect(target.ip, port, detail)) { ok = true; job.transport = order[i]; job.port = port; }
    }
    if (!ok) {
        job.state = State::Error;
        job.error = "connect_failed";
        job.detail = detail;
        job.finishedAt = millis();
        if (err) *err = job.error;
        Serial.printf("[PRT] falha ao conectar em %s [%s]\n", target.ip.toString().c_str(), detail.c_str());
        return false;
    }
    client.setNoDelay(false);
    if (detail.length()) job.detail = "tentativas: " + detail;   // portas que falharam antes da escolhida

    job.state = State::Streaming;
    lastProgress = millis();
    if (job.transport == Transport::Lpd && !lpdSendRecvJob()) {
        job.state = State::Error; job.error = "write_failed"; job.detail = "LPD receive-job"; job.finishedAt = millis();
        client.stop();
        if (err) *err = job.error;
        return false;
    }
    Serial.printf("[PRT] trabalho '%s' iniciado -> %s:%u via %s (%s)%s%s\n", id.c_str(), target.ip.toString().c_str(),
                  job.port, transportText(job.transport), source.c_str(),
                  detail.length() ? " apos falha em " : "", detail.c_str());
    return true;
}

void setDocument(size_t expected, const String& docFormat) {
    if (job.state != State::Streaming || headerSent) return;
    if (expected) job.expected = expected;
    if (docFormat.length()) job.docFormat = docFormat;
}

size_t freeSpace() {
    if (!busy()) return 0;
    return BUFFER_SIZE - (bufTail - bufHead);
}

bool write(const uint8_t* data, size_t len) {
    if (job.state != State::Streaming || finishRequested) return false;
    if (len > freeSpace()) return false;
    if (bufTail + len > BUFFER_SIZE) compact();
    memcpy(buf + bufTail, data, len);
    bufTail += len;
    job.received += len;
    return true;
}

void finish() {
    if (job.state != State::Streaming) return;
    finishRequested = true;
}

void cancel(const char* reason) {
    if (!busy()) return;
    Serial.printf("[PRT] trabalho '%s' cancelado (%s)\n", job.id.c_str(), reason ? reason : "-");
    end(State::Error, reason ? reason : "canceled");
}

void loop() {
    if (!busy()) return;
    uint32_t now = millis();

    // LPD: handshake antes dos dados e ack final depois deles
    if (job.transport == Transport::Lpd && lpdPhase != LpdPhase::Data) {
        lpdStep(now);
        return;
    }

    // IPP: aguardando a resposta HTTP apos o ultimo chunk
    if (job.state == State::Finishing && job.transport == Transport::Ipp) {
        bool ok; String detail;
        if (ippReadResponse(ok, detail)) {
            addDetail(detail);
            if (ok) {
                Serial.printf("[PRT] trabalho '%s' concluido via IPP: %u bytes, %s, %lu ms\n", job.id.c_str(),
                              (unsigned)job.written, detail.c_str(), (unsigned long)(now - job.startedAt));
                end(State::Done, nullptr);
            } else {
                Serial.printf("[PRT] trabalho '%s' rejeitado pela impressora: %s\n", job.id.c_str(), detail.c_str());
                end(State::Error, "printer_rejected");
            }
            return;
        }
        if (!client.connected() && !client.available()) { addDetail("conexao fechada sem resposta"); end(State::Error, "connection_closed"); return; }
        if (now - lastProgress > RESPONSE_TIMEOUT_MS) { addDetail("sem resposta IPP"); end(State::Error, "response_timeout"); }
        return;
    }

    if (!client.connected()) {
        if (bufTail > bufHead || !finishRequested) { end(State::Error, "connection_closed"); return; }
    }

    if (bufTail > bufHead) {
        if (!ensureHeader()) return;
        size_t pending = bufTail - bufHead;
        size_t chunk = pending > 1460 ? 1460 : pending;
        if (job.transport == Transport::Lpd && job.written + chunk > job.expected) chunk = job.expected - job.written;
        if (chunk == 0) { addDetail("recebido mais que o tamanho anunciado"); end(State::Error, "size_mismatch"); return; }
        bool ok = job.transport == Transport::Ipp ? ippSendChunk(buf + bufHead, chunk) : writeAll(buf + bufHead, chunk);
        if (ok) {
            bufHead += chunk;
            job.written += chunk;
            lastProgress = now;
            if (bufHead == bufTail) bufHead = bufTail = 0;
        } else if (now - lastProgress > STALL_TIMEOUT_MS || !client.connected()) {
            end(State::Error, "write_timeout");
        }
        return;
    }

    // buffer vazio
    if (finishRequested) {
        if (job.state == State::Streaming) {
            if (!ensureHeader()) return;   // documento vazio: ainda precisa do cabecalho (IPP)
            if (job.transport == Transport::Ipp && !trailerSent) {
                trailerSent = true;
                if (!writeAll((const uint8_t*)"0\r\n\r\n", 5)) { end(State::Error, "write_timeout"); return; }
            }
            if (job.transport == Transport::Lpd && !trailerSent) {
                if (job.written != job.expected) {
                    addDetail("enviados " + String(job.written) + " de " + String(job.expected) + " bytes anunciados");
                    end(State::Error, "size_mismatch");
                    return;
                }
                trailerSent = true;
                if (!writeAll((const uint8_t*)"\0", 1)) { end(State::Error, "write_timeout"); return; }
                lpdPhase = LpdPhase::AckData;   // loop volta para lpdStep ate o ack final
            }
            job.state = State::Finishing;
            lastProgress = now;
            client.flush();
        } else if (job.transport == Transport::Raw && now - lastProgress > CLOSE_GRACE_MS) {
            Serial.printf("[PRT] trabalho '%s' concluido: %u bytes em %lu ms\n", job.id.c_str(),
                          (unsigned)job.written, (unsigned long)(now - job.startedAt));
            end(State::Done, nullptr);
        }
    } else if (now - lastProgress > dataTimeoutMs) {
        end(State::Error, "data_timeout");   // solicitante parou de enviar blocos
    }
}

void setDataTimeout(uint32_t ms) { if (ms >= 5000 && ms <= 300000) dataTimeoutMs = ms; }
uint32_t dataTimeout() { return dataTimeoutMs; }

void statusJson(String& j) {
    if (starting) {
        // chamado da task web enquanto o loop principal conecta: nao toca nas Strings de 'job'
        j += "{\"state\":\"connecting\",\"busy\":true,\"buffer_free\":0,\"chunk_max\":" + String(CHUNK_MAX) + "}";
        return;
    }
    const char* st = "idle";
    switch (job.state) {
        case State::Streaming: st = "streaming"; break;
        case State::Finishing: st = "finishing"; break;
        case State::Done:      st = "done"; break;
        case State::Error:     st = "error"; break;
        default: break;
    }
    j += "{\"state\":\"" + String(st) + "\"";
    j += ",\"busy\":" + String(busy() ? "true" : "false");
    j += ",\"buffer_free\":" + String(freeSpace());
    j += ",\"chunk_max\":" + String(CHUNK_MAX);
    if (job.state != State::Idle) {
        j += ",\"job\":{\"id\":\"" + jsonEscape(job.id) + "\",\"name\":\"" + jsonEscape(job.name) + "\"";
        j += ",\"source\":\"" + jsonEscape(job.source) + "\",\"printer\":\"" + job.ip.toString() + "\",\"port\":" + String(job.port);
        j += ",\"transport\":\"" + String(transportText(job.transport)) + "\"";
        if (job.docFormat.length()) j += ",\"format\":\"" + jsonEscape(job.docFormat) + "\"";
        if (job.transport == Transport::Lpd) j += ",\"queue\":\"" + jsonEscape(tgt.lpdQueue) + "\"";
        j += ",\"expected\":" + String(job.expected) + ",\"received\":" + String(job.received) + ",\"written\":" + String(job.written);
        j += ",\"error\":\"" + jsonEscape(job.error) + "\",\"detail\":\"" + jsonEscape(job.detail) + "\"";
        uint32_t endT = job.finishedAt ? job.finishedAt : millis();
        j += ",\"duration_ms\":" + String(endT - job.startedAt) + "}";
    }
    j += "}";
}

bool printTest(const Target& target, const String& format, const String& id, const String& source, String* err) {
    String jobId = id.length() ? id : "test-" + String(millis());
    // Conecta primeiro: o formato automatico depende do transporte que aceitou a conexao.
    if (!start(jobId, target, 0, "Pagina de teste", source, "", err)) return false;

    String fmt = format; fmt.toLowerCase(); fmt.trim();
    if (fmt.isEmpty() || fmt == "auto") fmt = job.transport == Transport::Ipp ? "pdf" : "pcl";

    String text = testPageText(target.ip);
    String payload, mime;
    if (fmt == "pdf")                            { payload = testPagePdf(text); mime = "application/pdf"; }
    else if (fmt == "ps" || fmt == "postscript") { payload = testPagePostScript(text); mime = "application/postscript"; fmt = "ps"; }
    else if (fmt == "text" || fmt == "txt")      { payload = text + "\f"; mime = "text/plain"; fmt = "text"; }
    else                                         { payload = "\x1b" "E" + text + "\f" "\x1b" "E"; mime = "application/vnd.hp-PCL"; fmt = "pcl"; }

    job.name = "Pagina de teste (" + fmt + ")";
    setDocument(payload.length(), mime);
    if (!write((const uint8_t*)payload.c_str(), payload.length())) { cancel("overflow"); if (err) *err = "overflow"; return false; }
    finish();
    return true;
}

}  // namespace PrintJob
