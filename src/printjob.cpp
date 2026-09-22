#include "printjob.h"
#include "printers.h"
#include "json_util.h"

#include <WiFi.h>
#include <WiFiClient.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace PrintJob {

namespace {

WiFiClient client;
Info job;
DoneCallback doneCb = nullptr;

uint8_t buf[BUFFER_SIZE];
size_t bufHead = 0;   // proximo byte a enviar
size_t bufTail = 0;   // fim dos dados validos
bool finishRequested = false;
uint32_t lastProgress = 0;

const uint32_t CONNECT_TIMEOUT_MS = 3000;
const uint32_t STALL_TIMEOUT_MS = 20000;   // sem progresso de escrita por 20 s -> erro
const uint32_t CLOSE_GRACE_MS = 800;       // espera apos o ultimo byte antes de fechar

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
    if (doneCb) doneCb(job);
    // libera para o proximo trabalho mantendo 'job' como historico (state Done/Error)
}

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
    s += "Impressora    : " + ip.toString() + "\r\n";
    if (pr) {
        if (pr->model.length())    s += "Modelo        : " + pr->model + "\r\n";
        if (pr->name.length())     s += "Nome          : " + pr->name + "\r\n";
        if (pr->location.length()) s += "Local         : " + pr->location + "\r\n";
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
    s += "\r\nSe voce consegue ler esta pagina, o caminho de impressao raw\r\n";
    s += "(porta 9100) entre o PrintService e a impressora esta funcionando.\r\n";
    return s;
}

String psEscape(const String& s) {
    String o;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '(' || c == ')' || c == '\\') o += '\\';
        o += c;
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
        String line = text.substring(start, e);
        ps += "(" + psEscape(line) + ") show nl\r\n";
        start = e + 2;
    }
    ps += "showpage\r\n%%EOF\r\n";
    return ps;
}

}  // namespace

// ---------- API ----------

void onDone(DoneCallback cb) { doneCb = cb; }
bool busy() { return job.state == State::Streaming || job.state == State::Finishing; }
const Info& info() { return job; }

bool start(const String& id, const IPAddress& ip, uint16_t port, size_t expected,
           const String& name, const String& source, String* err) {
    if (busy()) { if (err) *err = "busy"; return false; }
    if (!WiFi.isConnected()) { if (err) *err = "no_network"; return false; }

    job = Info();
    job.id = id; job.name = name; job.source = source;
    job.ip = ip; job.port = port ? port : DEFAULT_PORT;
    job.expected = expected;
    job.startedAt = millis();
    bufHead = bufTail = 0;
    finishRequested = false;

    client.stop();
    client.setTimeout(5000);   // ms (limite das escritas bloqueantes no socket)
    if (!client.connect(ip, job.port, CONNECT_TIMEOUT_MS)) {
        job.state = State::Error;
        job.error = "connect_failed";
        job.finishedAt = millis();
        if (err) *err = job.error;
        Serial.printf("[PRT] falha ao conectar em %s:%u\n", ip.toString().c_str(), job.port);
        return false;
    }
    client.setNoDelay(false);
    job.state = State::Streaming;
    lastProgress = millis();
    Serial.printf("[PRT] trabalho '%s' iniciado -> %s:%u (%s)\n", id.c_str(), ip.toString().c_str(), job.port, source.c_str());
    return true;
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

    if (!client.connected()) {
        // impressora fechou a conexao antes do fim
        if (bufTail > bufHead || !finishRequested) { end(State::Error, "connection_closed"); return; }
    }

    if (bufTail > bufHead) {
        size_t pending = bufTail - bufHead;
        size_t chunk = pending > 1460 ? 1460 : pending;   // ~1 segmento TCP por iteracao
        size_t n = client.write(buf + bufHead, chunk);
        if (n > 0) {
            bufHead += n;
            job.written += n;
            lastProgress = now;
            if (bufHead == bufTail) bufHead = bufTail = 0;
        } else if (now - lastProgress > STALL_TIMEOUT_MS) {
            end(State::Error, "write_timeout");
        }
        return;
    }

    // buffer vazio
    if (finishRequested) {
        if (job.state == State::Streaming) { job.state = State::Finishing; lastProgress = now; client.flush(); }
        else if (now - lastProgress > CLOSE_GRACE_MS) {
            Serial.printf("[PRT] trabalho '%s' concluido: %u bytes em %lu ms\n", job.id.c_str(),
                          (unsigned)job.written, (unsigned long)(now - job.startedAt));
            end(State::Done, nullptr);
        }
    } else if (now - lastProgress > STALL_TIMEOUT_MS) {
        end(State::Error, "data_timeout");   // solicitante parou de enviar blocos
    }
}

void statusJson(String& j) {
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
        j += ",\"expected\":" + String(job.expected) + ",\"received\":" + String(job.received) + ",\"written\":" + String(job.written);
        j += ",\"error\":\"" + jsonEscape(job.error) + "\"";
        uint32_t endT = job.finishedAt ? job.finishedAt : millis();
        j += ",\"duration_ms\":" + String(endT - job.startedAt) + "}";
    }
    j += "}";
}

bool printTest(const IPAddress& ip, uint16_t port, const String& format, const String& id,
               const String& source, String* err) {
    String text = testPageText(ip);
    String payload;
    String fmt = format; fmt.toLowerCase();
    if (fmt == "ps" || fmt == "postscript") payload = testPagePostScript(text);
    else if (fmt == "text" || fmt == "txt")  payload = text + "\f";
    else                                     payload = "\x1b" "E" + text + "\f" "\x1b" "E";   // PCL: reset, texto, FF, reset

    String jobId = id.length() ? id : "test-" + String(millis());
    if (!start(jobId, ip, port, payload.length(), "Pagina de teste (" + fmt + ")", source, err)) return false;
    if (!write((const uint8_t*)payload.c_str(), payload.length())) { cancel("overflow"); if (err) *err = "overflow"; return false; }
    finish();
    return true;
}

}  // namespace PrintJob
