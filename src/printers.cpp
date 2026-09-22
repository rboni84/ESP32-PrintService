#include "printers.h"
#include "portal.h"
#include "snmp.h"
#include "json_util.h"

#include <WiFi.h>
#include <Preferences.h>
#include <mdns.h>

namespace PrinterMonitor {

namespace {

DeviceConfig* g_cfg = nullptr;
Printer printers[MAX_PRINTERS];
uint8_t nPrinters = 0;

const char* NS = "printsrv";
const char* KEY_PRINTERS = "printers";

const uint32_t DISCOVERY_INTERVAL_MS = 120 * 1000;  // ciclo completo de descoberta mDNS
const uint32_t DISCOVERY_QUERY_MS = 3000;           // duracao de cada consulta PTR
const uint32_t POLL_INTERVAL_MS = 30 * 1000;        // sondagem SNMP por impressora
const uint32_t SNMP_TIMEOUT_MS = 2000;
const uint32_t MDNS_EXPIRE_MS = 60UL * 60 * 1000;   // remove mDNS-only offline sem anuncio ha 1 h
const uint16_t SNMP_PORT = 161;

// ---------- OIDs ----------
// Prefixos (sem indice) usados com GETNEXT para obter a primeira linha de cada coluna.
const char* OID_SYSDESCR      = "1.3.6.1.2.1.1.1";
const char* OID_SYSNAME       = "1.3.6.1.2.1.1.5";
const char* OID_SYSLOCATION   = "1.3.6.1.2.1.1.6";
const char* OID_HRPRT_STATUS  = "1.3.6.1.2.1.25.3.5.1.1";   // hrPrinterStatus
const char* OID_HRPRT_ERRORS  = "1.3.6.1.2.1.25.3.5.1.2";   // hrPrinterDetectedErrorState
const char* OID_LIFECOUNT     = "1.3.6.1.2.1.43.10.2.1.4";  // prtMarkerLifeCount
const char* OID_PRTNAME       = "1.3.6.1.2.1.43.5.1.1.16";  // prtGeneralPrinterName
const char* OID_HRDEV_STATUS  = "1.3.6.1.2.1.25.3.2.1.5";   // hrDeviceStatus.<idx>
const char* OID_SUP_DESC      = "1.3.6.1.2.1.43.11.1.1.6";
const char* OID_SUP_CLASS     = "1.3.6.1.2.1.43.11.1.1.4";
const char* OID_SUP_TYPE      = "1.3.6.1.2.1.43.11.1.1.5";
const char* OID_SUP_MAX       = "1.3.6.1.2.1.43.11.1.1.8";
const char* OID_SUP_LEVEL     = "1.3.6.1.2.1.43.11.1.1.9";

// ---------- estado da sondagem SNMP ----------
Snmp::Client snmp;
enum class PollStage { Idle, General, DeviceStatus, Supplies };
PollStage stage = PollStage::Idle;
int8_t pollIdx = -1;          // impressora sendo sondada
String walkOids[5];           // OIDs correntes da caminhada na tabela de suprimentos
int hrIndex = 1;              // indice do dispositivo impressora em hrDeviceTable
Printer scratch;              // resultados parciais; copiados ao final (evita estado inconsistente)

// ---------- estado da descoberta mDNS ----------
struct ServiceType { const char* service; const char* proto; };
const ServiceType SERVICES[] = { {"_ipp", "_tcp"}, {"_printer", "_tcp"}, {"_pdl-datastream", "_tcp"} };
const uint8_t N_SERVICES = sizeof(SERVICES) / sizeof(SERVICES[0]);
mdns_search_once_t* search = nullptr;
uint8_t serviceIdx = 0;
uint32_t lastDiscovery = 0;
bool discoveryPending = true;  // dispara logo apos o STA conectar

// ---------- persistencia ----------

void saveManual() {
    String list;
    for (uint8_t i = 0; i < nPrinters; i++) {
        if (!printers[i].manual) continue;
        if (list.length()) list += ',';
        list += printers[i].ip.toString();
    }
    Preferences p;
    p.begin(NS, false);
    p.putString(KEY_PRINTERS, list);
    p.end();
}

int findByIp(const IPAddress& ip) {
    for (uint8_t i = 0; i < nPrinters; i++) if (printers[i].ip == ip) return i;
    return -1;
}

Printer* addPrinter(const IPAddress& ip) {
    int i = findByIp(ip);
    if (i >= 0) return &printers[i];
    if (nPrinters >= MAX_PRINTERS) return nullptr;
    Printer& pr = printers[nPrinters++];
    pr = Printer();
    pr.ip = ip;
    return &pr;
}

void loadManual() {
    Preferences p;
    p.begin(NS, true);
    String list = p.getString(KEY_PRINTERS, "");
    p.end();
    int start = 0;
    while (start < (int)list.length()) {
        int comma = list.indexOf(',', start);
        if (comma < 0) comma = list.length();
        String item = list.substring(start, comma);
        item.trim();
        IPAddress ip;
        if (item.length() && ip.fromString(item)) {
            Printer* pr = addPrinter(ip);
            if (pr) pr->manual = true;
        }
        start = comma + 1;
    }
}

// ---------- descoberta mDNS ----------

void startDiscoveryQuery() {
    if (search) return;
    const ServiceType& st = SERVICES[serviceIdx];
    search = mdns_query_async_new(nullptr, st.service, st.proto, MDNS_TYPE_PTR, DISCOVERY_QUERY_MS, 20, nullptr);
    if (!search) {
        // mDNS ainda nao inicializado (Portal inicia ao obter IP): tenta no proximo ciclo
        discoveryPending = true;
        lastDiscovery = millis();
    }
}

String txtValue(const mdns_result_t* r, const char* key) {
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0 && r->txt[i].value) {
            return String(r->txt[i].value);
        }
    }
    return String();
}

void handleDiscoveryResults(mdns_result_t* results) {
    uint32_t now = millis();
    for (mdns_result_t* r = results; r; r = r->next) {
        IPAddress ip;
        bool found = false;
        for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) { ip = IPAddress(a->addr.u_addr.ip4.addr); found = true; break; }
        }
        if (!found) continue;
        Printer* pr = addPrinter(ip);
        if (!pr) continue;
        bool isNew = !pr->viaMdns && !pr->manual;
        pr->viaMdns = true;
        pr->lastSeenMdns = now;
        if (r->hostname && pr->host.isEmpty()) pr->host = r->hostname;
        if (r->instance_name && pr->name.isEmpty()) pr->name = r->instance_name;
        String ty = txtValue(r, "ty");
        if (ty.length()) pr->model = ty;
        else { String prod = txtValue(r, "product"); if (prod.length() && pr->model.isEmpty()) pr->model = prod; }
        String note = txtValue(r, "note");
        if (note.length()) pr->location = note;
        if (isNew) Serial.printf("[PRN] descoberta via mDNS: %s (%s) %s\n", ip.toString().c_str(),
                                 pr->host.c_str(), pr->model.c_str());
    }
}

void pollDiscovery(uint32_t now) {
    if (search) {
        mdns_result_t* results = nullptr;
        uint8_t num = 0;
        if (mdns_query_async_get_results(search, 0, &results, &num)) {
            if (results) { handleDiscoveryResults(results); mdns_query_results_free(results); }
            mdns_query_async_delete(search);
            search = nullptr;
            serviceIdx++;
            if (serviceIdx >= N_SERVICES) { serviceIdx = 0; lastDiscovery = now; discoveryPending = false; }
            else startDiscoveryQuery();
        }
        return;
    }
    if (discoveryPending || now - lastDiscovery > DISCOVERY_INTERVAL_MS) {
        discoveryPending = false;
        serviceIdx = 0;
        startDiscoveryQuery();
    }
}

void expireStale(uint32_t now) {
    for (uint8_t i = 0; i < nPrinters;) {
        Printer& pr = printers[i];
        bool stale = !pr.manual && pr.viaMdns && !pr.online && now - pr.lastSeenMdns > MDNS_EXPIRE_MS;
        if (stale && (int8_t)i != pollIdx) {
            Serial.printf("[PRN] removida (sem anuncio mDNS e offline): %s\n", pr.ip.toString().c_str());
            for (uint8_t k = i; k + 1 < nPrinters; k++) printers[k] = printers[k + 1];
            nPrinters--;
            if (pollIdx > (int8_t)i) pollIdx--;
        } else i++;
    }
}

// ---------- sondagem SNMP ----------

bool oidUnder(const String& oid, const char* prefix) {
    size_t n = strlen(prefix);
    return oid.length() > n && oid.startsWith(prefix) && oid[n] == '.';
}

// Retorna o varbind i se ele estiver sob o prefixo pedido e nao for excecao; senao nullptr.
const Snmp::VarBind* vbUnder(uint8_t i, const char* prefix) {
    if (i >= snmp.resultCount() || snmp.errorStatus() != 0) return nullptr;
    const Snmp::VarBind& v = snmp.results()[i];
    if (v.isException() || !oidUnder(v.oid, prefix)) return nullptr;
    return &v;
}

String cleanStr(const String& s) {
    String o;
    for (size_t i = 0; i < s.length(); i++) { char c = s[i]; if ((uint8_t)c >= 0x20 || c == ' ') o += c; }
    o.trim();
    return o;
}

void sendGeneral(Printer& pr) {
    String oids[7] = { OID_SYSDESCR, OID_SYSNAME, OID_SYSLOCATION, OID_HRPRT_STATUS,
                       OID_HRPRT_ERRORS, OID_LIFECOUNT, OID_PRTNAME };
    snmp.request(pr.ip, SNMP_PORT, g_cfg->snmpCommunity, pr.snmpVersion, true, oids, 7, SNMP_TIMEOUT_MS);
    stage = PollStage::General;
}

void sendDeviceStatus(Printer& pr) {
    String oid = String(OID_HRDEV_STATUS) + "." + String(hrIndex);
    snmp.request(pr.ip, SNMP_PORT, g_cfg->snmpCommunity, pr.snmpVersion, false, &oid, 1, SNMP_TIMEOUT_MS);
    stage = PollStage::DeviceStatus;
}

void sendSuppliesNext(Printer& pr) {
    snmp.request(pr.ip, SNMP_PORT, g_cfg->snmpCommunity, pr.snmpVersion, true, walkOids, 5, SNMP_TIMEOUT_MS);
    stage = PollStage::Supplies;
}

void finishPoll(bool ok) {
    if (pollIdx >= 0 && pollIdx < (int8_t)nPrinters) {
        Printer& pr = printers[pollIdx];
        uint32_t now = millis();
        pr.lastPoll = now;
        if (ok) {
            // copia resultados coletados
            pr.name = scratch.name.length() ? scratch.name : pr.name;
            pr.host = scratch.host.length() ? scratch.host : pr.host;
            pr.model = scratch.model.length() ? scratch.model : pr.model;
            pr.location = scratch.location.length() ? scratch.location : pr.location;
            pr.printerStatus = scratch.printerStatus;
            pr.deviceStatus = scratch.deviceStatus;
            pr.errorState = scratch.errorState;
            pr.pageCount = scratch.pageCount;
            pr.hasPageCount = scratch.hasPageCount;
            pr.supplyCount = scratch.supplyCount;
            for (uint8_t i = 0; i < scratch.supplyCount; i++) pr.supplies[i] = scratch.supplies[i];
            if (!pr.online) Serial.printf("[PRN] online: %s %s\n", pr.ip.toString().c_str(), pr.model.c_str());
            pr.online = true;
            pr.everOnline = true;
            pr.fails = 0;
            pr.lastOk = now;
        } else {
            if (pr.fails < 255) pr.fails++;
            if (pr.fails == 2) pr.snmpVersion = pr.snmpVersion ? 0 : 1;  // tenta a outra versao
            if (pr.fails >= 3 && pr.online) {
                pr.online = false;
                Serial.printf("[PRN] offline (sem resposta SNMP): %s\n", pr.ip.toString().c_str());
            }
        }
    }
    stage = PollStage::Idle;
    pollIdx = -1;
}

void handleGeneral(Printer& pr) {
    const Snmp::VarBind* v;
    scratch = Printer();
    if ((v = vbUnder(0, OID_SYSDESCR)) && v->type == Snmp::VarType::String) scratch.model = cleanStr(v->str);
    if ((v = vbUnder(1, OID_SYSNAME)) && v->type == Snmp::VarType::String) scratch.host = cleanStr(v->str);
    if ((v = vbUnder(2, OID_SYSLOCATION)) && v->type == Snmp::VarType::String) scratch.location = cleanStr(v->str);
    hrIndex = 1;
    if ((v = vbUnder(3, OID_HRPRT_STATUS)) && v->isNumeric()) {
        scratch.printerStatus = (int)v->num;
        int dot = v->oid.lastIndexOf('.');
        if (dot > 0) hrIndex = v->oid.substring(dot + 1).toInt();
        if (hrIndex <= 0) hrIndex = 1;
    }
    if ((v = vbUnder(4, OID_HRPRT_ERRORS)) && v->type == Snmp::VarType::String) {
        uint16_t bits = 0;
        if (v->str.length() >= 1) bits |= (uint8_t)v->str[0] << 8;
        if (v->str.length() >= 2) bits |= (uint8_t)v->str[1];
        scratch.errorState = bits;
    }
    if ((v = vbUnder(5, OID_LIFECOUNT)) && v->isNumeric()) { scratch.pageCount = (uint32_t)v->num; scratch.hasPageCount = true; }
    if ((v = vbUnder(6, OID_PRTNAME)) && v->type == Snmp::VarType::String) scratch.name = cleanStr(v->str);
    // Se o modelo veio do mDNS (TXT ty), ele costuma ser mais limpo que sysDescr: mantem.
    if (pr.viaMdns && pr.model.length()) scratch.model = pr.model;
    sendDeviceStatus(pr);
}

void handleDeviceStatus(Printer& pr) {
    if (snmp.errorStatus() == 0 && snmp.resultCount() >= 1) {
        const Snmp::VarBind& v = snmp.results()[0];
        if (v.isNumeric()) scratch.deviceStatus = (int)v.num;
    }
    walkOids[0] = OID_SUP_DESC; walkOids[1] = OID_SUP_CLASS; walkOids[2] = OID_SUP_TYPE;
    walkOids[3] = OID_SUP_MAX;  walkOids[4] = OID_SUP_LEVEL;
    scratch.supplyCount = 0;
    sendSuppliesNext(pr);
}

void handleSupplies(Printer& pr) {
    const Snmp::VarBind* d = vbUnder(0, OID_SUP_DESC);
    const Snmp::VarBind* c = vbUnder(1, OID_SUP_CLASS);
    const Snmp::VarBind* t = vbUnder(2, OID_SUP_TYPE);
    const Snmp::VarBind* m = vbUnder(3, OID_SUP_MAX);
    const Snmp::VarBind* l = vbUnder(4, OID_SUP_LEVEL);
    // fim da tabela (ou impressora sem Printer-MIB)
    if (!l || !m || scratch.supplyCount >= MAX_SUPPLIES) { finishPoll(true); return; }
    Supply& s = scratch.supplies[scratch.supplyCount++];
    s.desc = (d && d->type == Snmp::VarType::String) ? cleanStr(d->str) : String("Suprimento ") + scratch.supplyCount;
    s.cls = (c && c->isNumeric()) ? (uint8_t)c->num : 0;
    s.type = (t && t->isNumeric()) ? (uint8_t)t->num : 0;
    s.max = (int32_t)m->num;
    s.level = (int32_t)l->num;
    // avanca a caminhada com os OIDs devolvidos
    for (uint8_t i = 0; i < 5; i++) walkOids[i] = snmp.results()[i].oid;
    sendSuppliesNext(pr);
}

void pollSnmp(uint32_t now) {
    if (stage != PollStage::Idle) {
        if (pollIdx < 0 || pollIdx >= (int8_t)nPrinters) { snmp.cancel(); stage = PollStage::Idle; pollIdx = -1; return; }
        Printer& pr = printers[pollIdx];
        Snmp::Status st = snmp.poll();
        if (st == Snmp::Status::Pending) return;
        if (st != Snmp::Status::Done) {
            // hrDeviceStatus e suprimentos sao opcionais: timeout neles nao invalida os dados gerais
            finishPoll(stage != PollStage::General);
            return;
        }
        switch (stage) {
            case PollStage::General:      handleGeneral(pr); break;
            case PollStage::DeviceStatus: handleDeviceStatus(pr); break;
            case PollStage::Supplies:     handleSupplies(pr); break;
            default: finishPoll(false);
        }
        return;
    }
    // escolhe a impressora ha mais tempo sem sondagem
    int8_t best = -1;
    uint32_t bestAge = 0;
    for (uint8_t i = 0; i < nPrinters; i++) {
        uint32_t age = printers[i].lastPoll ? now - printers[i].lastPoll : 0xFFFFFFFF;
        if (age >= POLL_INTERVAL_MS && age > bestAge) { best = i; bestAge = age; }
    }
    if (best < 0) return;
    pollIdx = best;
    sendGeneral(printers[best]);
}

}  // namespace

// ---------- Printer ----------

bool Printer::hasAlert() const {
    if (!online) return everOnline || manual;
    if (deviceStatus == 3 || deviceStatus == 5) return true;
    if (errorState) return true;
    for (uint8_t i = 0; i < supplyCount; i++) {
        int pct = supplies[i].percent();
        if (pct >= 0 && supplies[i].cls == 3 && pct <= 10) return true;
        if (pct >= 0 && supplies[i].cls == 4 && pct >= 90) return true;
        if (supplies[i].level == 0) return true;
    }
    return false;
}

// ---------- API publica ----------

void begin(DeviceConfig& cfg) {
    g_cfg = &cfg;
    loadManual();
    Serial.printf("[PRN] monitor iniciado: %u impressora(s) manual(is); community='%s'\n", nPrinters,
                  cfg.snmpCommunity.c_str());
}

void loop() {
    uint32_t now = millis();
    if (!Portal::isStaConnected()) {
        // sem rede: aborta consultas em andamento e reagenda descoberta para quando conectar
        if (stage != PollStage::Idle) { snmp.cancel(); stage = PollStage::Idle; pollIdx = -1; }
        if (search) { mdns_query_async_delete(search); search = nullptr; serviceIdx = 0; }
        discoveryPending = true;
        return;
    }
    pollDiscovery(now);
    pollSnmp(now);
    static uint32_t lastExpire = 0;
    if (now - lastExpire > 60000) { lastExpire = now; expireStale(now); }
}

uint8_t count() { return nPrinters; }
uint8_t onlineCount() { uint8_t n = 0; for (uint8_t i = 0; i < nPrinters; i++) if (printers[i].online) n++; return n; }
uint8_t alertCount() { uint8_t n = 0; for (uint8_t i = 0; i < nPrinters; i++) if (printers[i].hasAlert()) n++; return n; }
const Printer* get(uint8_t i) { return i < nPrinters ? &printers[i] : nullptr; }

const char* deviceStatusText(int v) {
    switch (v) { case 1: return "desconhecido"; case 2: return "operando"; case 3: return "alerta";
                 case 4: return "em teste"; case 5: return "parada"; default: return "-"; }
}

const char* printerStatusText(int v) {
    switch (v) { case 1: return "outro"; case 2: return "desconhecido"; case 3: return "ociosa";
                 case 4: return "imprimindo"; case 5: return "aquecendo"; default: return "-"; }
}

String errorStateText(uint16_t b) {
    struct { uint16_t bit; const char* txt; } names[] = {
        {0x8000, "pouco papel"}, {0x4000, "sem papel"}, {0x2000, "pouco toner"}, {0x1000, "sem toner"},
        {0x0800, "tampa aberta"}, {0x0400, "papel atolado"}, {0x0200, "offline"}, {0x0100, "requer manutencao"},
        {0x0080, "bandeja de entrada ausente"}, {0x0040, "bandeja de saida ausente"}, {0x0020, "suprimento ausente"},
        {0x0010, "saida quase cheia"}, {0x0008, "saida cheia"}, {0x0004, "bandeja vazia"}, {0x0002, "manutencao preventiva atrasada"},
    };
    String s;
    for (auto& n : names) if (b & n.bit) { if (s.length()) s += ", "; s += n.txt; }
    return s;
}

void toJson(String& j) {
    uint32_t now = millis();
    j += "{\"count\":" + String(nPrinters) + ",\"online\":" + String(onlineCount()) +
         ",\"alerts\":" + String(alertCount()) + ",\"sta\":" + String(Portal::isStaConnected() ? "true" : "false") +
         ",\"printers\":[";
    for (uint8_t i = 0; i < nPrinters; i++) {
        const Printer& p = printers[i];
        if (i) j += ",";
        j += "{\"ip\":\"" + p.ip.toString() + "\"";
        j += ",\"host\":\"" + jsonEscape(p.host) + "\"";
        j += ",\"name\":\"" + jsonEscape(p.name) + "\"";
        j += ",\"model\":\"" + jsonEscape(p.model) + "\"";
        j += ",\"location\":\"" + jsonEscape(p.location) + "\"";
        j += ",\"manual\":" + String(p.manual ? "true" : "false");
        j += ",\"mdns\":" + String(p.viaMdns ? "true" : "false");
        j += ",\"online\":" + String(p.online ? "true" : "false");
        j += ",\"alert\":" + String(p.hasAlert() ? "true" : "false");
        j += ",\"dev_status\":" + String(p.deviceStatus) + ",\"dev_status_text\":\"" + deviceStatusText(p.deviceStatus) + "\"";
        j += ",\"prn_status\":" + String(p.printerStatus) + ",\"prn_status_text\":\"" + printerStatusText(p.printerStatus) + "\"";
        j += ",\"errors\":\"" + jsonEscape(errorStateText(p.errorState)) + "\"";
        j += ",\"pages\":" + String(p.hasPageCount ? String(p.pageCount) : String("null"));
        j += ",\"last_ok\":" + String(p.lastOk ? String((now - p.lastOk) / 1000) : String("null"));
        j += ",\"snmp_version\":\"" + String(p.snmpVersion ? "v2c" : "v1") + "\"";
        j += ",\"supplies\":[";
        for (uint8_t k = 0; k < p.supplyCount; k++) {
            const Supply& s = p.supplies[k];
            if (k) j += ",";
            j += "{\"desc\":\"" + jsonEscape(s.desc) + "\",\"level\":" + String(s.level) + ",\"max\":" + String(s.max) +
                 ",\"pct\":" + String(s.percent()) + ",\"class\":" + String(s.cls) + ",\"type\":" + String(s.type) + "}";
        }
        j += "]}";
    }
    j += "]}";
}

void printList(Print& out) {
    out.println();
    out.println("------------- IMPRESSORAS -------------");
    if (!nPrinters) out.println("(nenhuma; use mDNS automatico ou adicione por IP)");
    uint32_t now = millis();
    for (uint8_t i = 0; i < nPrinters; i++) {
        const Printer& p = printers[i];
        out.printf("%2d) %-15s %s%s\n", i + 1, p.ip.toString().c_str(), p.online ? "ONLINE " : "offline",
                   p.manual ? " [manual]" : (p.viaMdns ? " [mDNS]" : ""));
        if (p.model.length())    out.printf("    modelo : %s\n", p.model.c_str());
        if (p.name.length() || p.host.length())
            out.printf("    nome   : %s%s%s\n", p.name.c_str(), (p.name.length() && p.host.length()) ? " / " : "", p.host.c_str());
        if (p.location.length()) out.printf("    local  : %s\n", p.location.c_str());
        if (p.online) {
            out.printf("    estado : %s / %s%s%s\n", deviceStatusText(p.deviceStatus), printerStatusText(p.printerStatus),
                       p.errorState ? " - " : "", p.errorState ? errorStateText(p.errorState).c_str() : "");
            if (p.hasPageCount) out.printf("    paginas: %lu\n", (unsigned long)p.pageCount);
            for (uint8_t k = 0; k < p.supplyCount; k++) {
                const Supply& s = p.supplies[k];
                int pct = s.percent();
                if (pct >= 0) out.printf("    %-28s %3d%%%s\n", s.desc.c_str(), pct, s.cls == 4 ? " (cheio)" : "");
                else out.printf("    %-28s %s\n", s.desc.c_str(), s.level == -3 ? "ok" : "n/d");
            }
            out.printf("    ultima resposta ha %lus (SNMP %s)\n", (unsigned long)((now - p.lastOk) / 1000), p.snmpVersion ? "v2c" : "v1");
        } else if (p.fails) {
            out.printf("    sem resposta SNMP (%u falhas)\n", p.fails);
        }
    }
    out.println("---------------------------------------");
}

bool addManual(const String& ipText, String* err) {
    IPAddress ip;
    String t = ipText; t.trim();
    if (!ip.fromString(t) || ip == IPAddress((uint32_t)0)) { if (err) *err = "IP invalido"; return false; }
    int i = findByIp(ip);
    if (i >= 0 && printers[i].manual) { if (err) *err = "ja cadastrada"; return false; }
    Printer* pr = addPrinter(ip);
    if (!pr) { if (err) *err = "limite de impressoras atingido"; return false; }
    pr->manual = true;
    pr->lastPoll = 0;  // sonda em seguida
    saveManual();
    Serial.printf("[PRN] adicionada manualmente: %s\n", ip.toString().c_str());
    return true;
}

bool remove(const String& ipText) {
    IPAddress ip;
    String t = ipText; t.trim();
    if (!ip.fromString(t)) return false;
    int i = findByIp(ip);
    if (i < 0) return false;
    if ((int8_t)i == pollIdx) { snmp.cancel(); stage = PollStage::Idle; pollIdx = -1; }
    else if (pollIdx > (int8_t)i) pollIdx--;
    bool wasManual = printers[i].manual;
    for (uint8_t k = i; k + 1 < nPrinters; k++) printers[k] = printers[k + 1];
    nPrinters--;
    if (wasManual) saveManual();
    Serial.printf("[PRN] removida: %s\n", ip.toString().c_str());
    return true;
}

void refreshNow() {
    discoveryPending = true;
    for (uint8_t i = 0; i < nPrinters; i++) printers[i].lastPoll = 0;
}

}  // namespace PrinterMonitor
