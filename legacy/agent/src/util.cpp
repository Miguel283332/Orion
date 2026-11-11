#include "orion/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace orion {

static bool g_quiet = false;
void set_quiet(bool q) { g_quiet = q; }
bool is_quiet() { return g_quiet; }
void say(const std::string& msg) {
    if (!g_quiet) { std::cout << msg << std::endl; }
}
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string now_str() {
    std::time_t t = std::time(nullptr);
    char b[32];
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); } else { cur.push_back(c); }
    }
    out.push_back(cur);
    return out;
}
std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}
std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t keep = n > 20 ? n - 20 : n;
    // no cortar a mitad de un carácter UTF-8
    while (keep > 0 && ((unsigned char)s[keep] & 0xC0) == 0x80) --keep;
    return s.substr(0, keep) + "... [+" + std::to_string(s.size() - keep) + " bytes]";
}
size_t count_occurrences(const std::string& s, const std::string& sub) {
    if (sub.empty()) return 0;
    size_t n = 0, pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) { ++n; pos += sub.size(); }
    return n;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("no se puede leer " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void write_file(const fs::path& p, const std::string& data, bool append) {
    if (p.has_parent_path()) { std::error_code ec; fs::create_directories(p.parent_path(), ec); }
    std::ofstream out(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) throw std::runtime_error("no se puede escribir " + p.string());
    out << data;
}
void write_file_atomic(const fs::path& p, const std::string& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    write_file(tmp, data);
    std::error_code ec;
    fs::rename(tmp, p, ec);  // reemplaza el destino también en Windows
    if (ec) throw std::runtime_error("no se puede renombrar " + tmp.string() + ": " + ec.message());
}
std::string rand_hex(size_t n) {
    static std::mt19937_64 rng{std::random_device{}() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()};
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s.push_back(h[rng() & 15]);
    return s;
}
std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : def;
}
fs::path make_temp_dir(const std::string& prefix) {
    fs::path p = fs::temp_directory_path() / (prefix + rand_hex(10));
    fs::create_directories(p);
    return p;
}
std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + replace_all(s, "\"", "\\\"") + "\"";
#else
    return "'" + replace_all(s, "'", "'\\''") + "'";
#endif
}
ProcResult run_cmd(const std::string& cmdline) {
    ProcResult r;
    FILE* f = popen(cmdline.c_str(), "r");
    if (!f) { r.code = -1; r.out = "no se pudo ejecutar el comando"; return r; }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) r.out.append(buf, n);
    int st = pclose(f);
#ifdef _WIN32
    r.code = st;
#else
    r.code = (st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : st;
#endif
    return r;
}

// ---------------------------------------------------------------- SHA-256
namespace {
const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t blen = 0;
    uint64_t total = 0;
    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n > 0) {
            size_t take = std::min(n, 64 - blen);
            std::copy(p, p + take, buf + blen);
            blen += take; p += take; n -= take;
            if (blen == 64) { block(buf); blen = 0; }
        }
    }
    std::string finish() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (blen != 56) update(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        static const char* hx = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; ++i)
            for (int j = 28; j >= 0; j -= 4) out.push_back(hx[(h[i] >> j) & 15]);
        return out;
    }
};
}  // namespace

std::string sha256_hex(const std::string& data) {
    Sha256 s;
    s.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return s.finish();
}
std::string sha256_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("no se puede leer " + p.string());
    Sha256 s;
    char buf[65536];
    while (in) {
        in.read(buf, sizeof(buf));
        s.update(reinterpret_cast<const uint8_t*>(buf), (size_t)in.gcount());
    }
    return s.finish();
}

// ---------------------------------------------------------------- JSON tolerante
std::optional<json> parse_json_loose(const std::string& text) {
    std::string t = trim(text);
    if (starts_with(t, "```")) {
        size_t nl = t.find('\n');
        if (nl != std::string::npos) t = t.substr(nl + 1);
        size_t end = t.rfind("```");
        if (end != std::string::npos) t = t.substr(0, end);
        t = trim(t);
    }
    json j = json::parse(t, nullptr, false);
    if (!j.is_discarded()) return j;
    size_t start = t.find('{');
    if (start == std::string::npos) return std::nullopt;
    int depth = 0;
    bool instr = false, esc = false;
    for (size_t i = start; i < t.size(); ++i) {
        char c = t[i];
        if (instr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') instr = false;
        } else if (c == '"') {
            instr = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                json k = json::parse(t.substr(start, i - start + 1), nullptr, false);
                if (k.is_discarded()) return std::nullopt;
                return k;
            }
        }
    }
    return std::nullopt;
}

}  // namespace orion
