#pragma once
#include <Arduino.h>

// SSID e uma sequencia de ate 32 bytes sem codificacao definida (802.11). Roteadores configurados
// por paineis antigos anunciam acentos em Latin-1 (1 byte: 'A' com acento = C1), enquanto o
// navegador e o terminal enviam UTF-8 (C3 81). O radio compara byte a byte, por isso um SSID com
// acento salvo em UTF-8 da NO_AP_FOUND numa rede anunciada em Latin-1. Estas funcoes convertem
// entre as duas formas e produzem um texto exibivel (UTF-8 valido) a partir dos bytes do radio.
namespace SsidUtil {

inline bool isAscii(const String& s) {
    for (size_t i = 0; i < s.length(); i++) if ((uint8_t)s[i] >= 0x80) return false;
    return true;
}

// Estrutura UTF-8 valida (cabecalho + continuacoes). Suficiente para decidir como exibir.
inline bool isValidUtf8(const String& s) {
    size_t i = 0, n = s.length();
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > n) return false;
        for (int k = 1; k < len; k++) if (((uint8_t)s[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

inline String latin1ToUtf8(const String& s) {
    String o;
    o.reserve(s.length() * 2);
    for (size_t i = 0; i < s.length(); i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) o += (char)c;
        else { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}

// UTF-8 -> Latin-1. Falso se houver caractere fora de U+0000..U+00FF ou UTF-8 invalido.
inline bool utf8ToLatin1(const String& s, String& out) {
    out = "";
    size_t i = 0, n = s.length();
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { out += (char)c; i++; continue; }
        if ((c & 0xE0) == 0xC0 && i + 1 < n && ((uint8_t)s[i + 1] & 0xC0) == 0x80) {
            uint32_t cp = ((c & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F);
            if (cp > 0xFF) return false;
            out += (char)cp;
            i += 2;
            continue;
        }
        return false;
    }
    return true;
}

// Texto exibivel (UTF-8 valido) para um SSID vindo do radio ou da NVS.
inline String display(const String& raw) { return isValidUtf8(raw) ? raw : latin1ToUtf8(raw); }

// Caracteres (code points) de uma string UTF-8; bytes soltos contam 1.
inline size_t charCount(const String& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.length(); i++) if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
    return n;
}

// Mesmo nome de rede: iguais byte a byte, ou um e a forma Latin-1 do outro (UTF-8).
inline bool sameName(const String& a, const String& b) {
    if (a == b) return true;
    String t;
    if (utf8ToLatin1(a, t) && t == b) return true;
    if (utf8ToLatin1(b, t) && t == a) return true;
    return false;
}

// Remove espacos finais: ' ' e o espaco nao separavel (A0 em Latin-1, C2 A0 em UTF-8). O portal
// aplica trim() ao SSID digitado, entao um nome anunciado com espaco no fim nunca bateria.
inline String stripTrailingSpace(const String& s) {
    String o = s;
    for (;;) {
        size_t n = o.length();
        if (n && ((uint8_t)o[n - 1] == 0x20 || (uint8_t)o[n - 1] == 0xA0)) { o.remove(n - 1); continue; }
        if (n >= 2 && (uint8_t)o[n - 2] == 0xC2 && (uint8_t)o[n - 1] == 0xA0) { o.remove(n - 2); continue; }
        break;
    }
    return o;
}

// Bytes em hexadecimal, para diagnostico no log serial.
inline String hex(const String& s) {
    String o;
    o.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char b[4];
        snprintf(b, sizeof(b), "%02X ", (uint8_t)s[i]);
        o += b;
    }
    o.trim();
    return o;
}

}  // namespace SsidUtil
