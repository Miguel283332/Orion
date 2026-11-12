#include "orion/util.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace orion {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            v.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == sep) { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    v.push_back(cur);
    return v;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) r += (i ? sep : "") + v[i];
    return r;
}

bool empieza_por(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool contiene(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

std::string recortar(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t corte = max;
    while (corte > 0 && (static_cast<unsigned char>(s[corte]) & 0xC0) == 0x80) --corte;   // no partir un carácter UTF-8
    return s.substr(0, corte) + "…";
}

std::string minusculas_sin_acentos(const std::string& s) {
    // Solo hace falta para el español (y las vocales con diéresis/acentos habituales).
    static const std::pair<const char*, char> mapa[] = {
        {"á", 'a'}, {"é", 'e'}, {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'}, {"ü", 'u'}, {"ñ", 'n'}, {"à", 'a'}, {"è", 'e'}, {"ò", 'o'},
        {"Á", 'a'}, {"É", 'e'}, {"Í", 'i'}, {"Ó", 'o'}, {"Ú", 'u'}, {"Ü", 'u'}, {"Ñ", 'n'}, {"ç", 'c'}, {"Ç", 'c'}};
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        bool hecho = false;
        if (static_cast<unsigned char>(s[i]) >= 0x80) {
            for (auto& [utf8, ascii] : mapa) {
                size_t n = std::char_traits<char>::length(utf8);
                if (s.compare(i, n, utf8) == 0) { r += ascii; i += n; hecho = true; break; }
            }
        }
        if (!hecho) {
            char c = s[i++];
            r += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    }
    return r;
}

std::vector<std::string> palabras(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    auto cerrar = [&] { if (cur.size() >= 2) v.push_back(cur); cur.clear(); };
    std::string t = minusculas_sin_acentos(s);
    for (const char* signo : {"¡", "¿", "«", "»", "—", "–", "“", "”", "‘", "’", "…", "·"}) {   // signos no ASCII: separan palabras
        for (size_t p = t.find(signo); p != std::string::npos; p = t.find(signo, p)) t.replace(p, std::char_traits<char>::length(signo), " ");
    }
    for (char c : t) {
        unsigned char u = static_cast<unsigned char>(c);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || u >= 0x80) cur += c;
        else cerrar();
    }
    cerrar();
    return v;
}

std::string formato_bytes(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; ++i; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), i == 0 ? "%.0f %s" : "%.1f %s", b, u[i]);
    return buf;
}

std::string formato_numero(double n) {
    char buf[64];
    if (n >= 1e9) std::snprintf(buf, sizeof(buf), "%.2f mil millones", n / 1e9);
    else if (n >= 1e6) std::snprintf(buf, sizeof(buf), "%.1f millones", n / 1e6);
    else if (n >= 1e3) std::snprintf(buf, sizeof(buf), "%.1f mil", n / 1e3);
    else std::snprintf(buf, sizeof(buf), "%.0f", n);
    return buf;
}

std::string formato_duracion(double s) {
    long t = static_cast<long>(s);
    char buf[64];
    if (t >= 86400) std::snprintf(buf, sizeof(buf), "%ld d %ld h", t / 86400, (t % 86400) / 3600);
    else if (t >= 3600) std::snprintf(buf, sizeof(buf), "%ld h %ld min", t / 3600, (t % 3600) / 60);
    else if (t >= 60) std::snprintf(buf, sizeof(buf), "%ld min %ld s", t / 60, t % 60);
    else std::snprintf(buf, sizeof(buf), "%ld s", t);
    return buf;
}

std::string sha256_hex(const std::string& datos) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m = datos;
    uint64_t bits = static_cast<uint64_t>(datos.size()) * 8;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56) m += '\0';
    for (int i = 7; i >= 0; --i) m += static_cast<char>((bits >> (i * 8)) & 0xff);
    auto rot = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(static_cast<unsigned char>(m[off + i * 4])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(m[off + i * 4 + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(m[off + i * 4 + 2])) << 8) | static_cast<uint32_t>(static_cast<unsigned char>(m[off + i * 4 + 3]));
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rot(e, 6) ^ rot(e, 11) ^ rot(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rot(a, 2) ^ rot(a, 13) ^ rot(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

double ahora_s() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string fecha_hora() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string leer_archivo(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void escribir_atomico(const fs::path& p, const std::string& contenido) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("no se puede escribir " + tmp.string());
        f << contenido;
        f.flush();
        if (!f) throw std::runtime_error("fallo al escribir " + tmp.string());
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);           // en Windows, rename sobre un archivo existente puede fallar
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p);
    }
}

void anadir_linea(const fs::path& p, const std::string& linea) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::app);
    if (!f) throw std::runtime_error("no se puede escribir " + p.string());
    f << linea << '\n';
}

double tamano_directorio(const fs::path& p) {
    double total = 0;
    std::error_code ec;
    if (!fs::exists(p, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) total += static_cast<double>(it->file_size(ec));
    return total;
}

void preparar_consola() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);   // colores ANSI en la consola de Windows
    DWORD modo = 0;
    if (GetConsoleMode(h, &modo)) SetConsoleMode(h, modo | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
#endif
}

bool leer_linea(std::string& linea) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD modo = 0;
    if (GetConsoleMode(h, &modo)) {   // consola real: ReadConsoleW evita el problema de las tildes con UTF-8
        std::wstring w;
        wchar_t buf[512];
        for (;;) {
            DWORD leidos = 0;
            if (!ReadConsoleW(h, buf, 512, &leidos, nullptr) || leidos == 0) return !w.empty();
            w.append(buf, leidos);
            if (!w.empty() && w.back() == L'\n') break;
        }
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        linea.assign(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), linea.data(), n, nullptr, nullptr);
        return true;
    }
#endif
    if (!std::getline(std::cin, linea)) return false;
    if (!linea.empty() && linea.back() == '\r') linea.pop_back();
    return true;
}

}  // namespace orion
