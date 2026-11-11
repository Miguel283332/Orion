#include "demo/util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace demo {

std::string ahora() {
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
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur.push_back(c); }
    out.push_back(cur);
    return out;
}
std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += sep; o += v[i]; }
    return o;
}
std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}
std::string recortar(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t keep = n > 15 ? n - 15 : n;
    while (keep > 0 && ((unsigned char)s[keep] & 0xC0) == 0x80) --keep;   // no cortar un carácter UTF-8
    return s.substr(0, keep) + "... [+" + std::to_string(s.size() - keep) + "]";
}
std::string dedent(const std::string& s) {
    auto lines = split_lines(s);
    size_t min_ind = std::string::npos;
    for (auto& l : lines) {
        if (trim(l).empty()) continue;
        size_t i = 0;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
        min_ind = std::min(min_ind, i);
    }
    if (min_ind == std::string::npos || min_ind == 0) {
        for (auto& l : lines) if (trim(l).empty()) l.clear();
        return join(lines, "\n");
    }
    for (auto& l : lines) l = trim(l).empty() ? "" : l.substr(min_ind);
    return join(lines, "\n");
}
std::string sangrar(const std::vector<std::string>& lineas, int n) {
    std::string pad(n, ' '), o;
    for (size_t i = 0; i < lineas.size(); ++i) {
        if (i) o += "\n";
        o += trim(lineas[i]).empty() ? lineas[i] : pad + lineas[i];
    }
    return o;
}
std::string ensamblar(const std::vector<std::string>& partes) {
    std::vector<std::string> v;
    for (auto& p : partes) { std::string t = trim(p); if (!t.empty()) v.push_back(t); }
    return join(v, "\n\n");
}

std::string normalizar(const std::string& texto) {
    std::string t = texto;
    for (auto& [a, b] : std::vector<std::pair<std::string, std::string>>{{"ℕ", "Nat"}, {"ℤ", "Int"}, {"ℝ", "Real"}, {"→", "->"}, {"≤", "<="}, {"≥", ">="}})
        t = replace_all(t, a, b);
    std::string o;
    bool sp = false;
    for (unsigned char c : t) {
        if (std::isspace(c)) sp = true;
        else { if (sp && !o.empty()) o.push_back(' '); sp = false; o.push_back((char)c); }
    }
    return o;
}
std::string huella(const std::string& s) { return sha256_hex(normalizar(s)).substr(0, 16); }

std::string leer_archivo(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("no se puede leer " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void escribir_archivo(const fs::path& p, const std::string& data, bool anexar) {
    if (p.has_parent_path()) { std::error_code ec; fs::create_directories(p.parent_path(), ec); }
    std::ofstream out(p, std::ios::binary | (anexar ? std::ios::app : std::ios::trunc));
    if (!out) throw std::runtime_error("no se puede escribir " + p.string());
    out << data;
}
void escribir_atomico(const fs::path& p, const std::string& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    escribir_archivo(tmp, data);
    std::error_code ec;
    fs::rename(tmp, p, ec);   // reemplaza el destino también en Windows
    if (ec) throw std::runtime_error("no se puede renombrar " + tmp.string() + ": " + ec.message());
}
static std::string aleatorio_hex(size_t n) {
    static std::mt19937_64 rng{std::random_device{}() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()};
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s.push_back(h[rng() & 15]);
    return s;
}
fs::path crear_temporal(const std::string& prefijo) {
    fs::path p = fs::temp_directory_path() / (prefijo + aleatorio_hex(10));
    fs::create_directories(p);
    return p;
}
std::string env_o(const char* nombre, const std::string& defecto) {
    const char* v = std::getenv(nombre);
    return v ? std::string(v) : defecto;
}
void dormir_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

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
        uint8_t pad = 0x80, z = 0;
        update(&pad, 1);
        while (blen != 56) update(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        static const char* hx = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; ++i) for (int j = 28; j >= 0; j -= 4) out.push_back(hx[(h[i] >> j) & 15]);
        return out;
    }
};
}  // namespace
std::string sha256_hex(const std::string& data) {
    Sha256 s;
    s.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return s.finish();
}

// ---------------------------------------------------------------- extracción
std::optional<json> extraer_json(const std::string& texto) {
    std::string t = trim(texto);
    if (starts_with(t, "```")) {
        size_t nl = t.find('\n');
        if (nl != std::string::npos) t = t.substr(nl + 1);
        size_t end = t.rfind("```");
        if (end != std::string::npos) t = t.substr(0, end);
        t = trim(t);
    }
    json j = json::parse(t, nullptr, false);
    if (!j.is_discarded() && j.is_object()) return j;
    size_t start = t.find('{');
    if (start == std::string::npos) return std::nullopt;
    int prof = 0;
    bool instr = false, esc = false;
    for (size_t i = start; i < t.size(); ++i) {
        char c = t[i];
        if (instr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') instr = false; }
        else if (c == '"') instr = true;
        else if (c == '{') ++prof;
        else if (c == '}' && --prof == 0) {
            json k = json::parse(t.substr(start, i - start + 1), nullptr, false);
            if (k.is_discarded() || !k.is_object()) return std::nullopt;
            return k;
        }
    }
    return std::nullopt;
}

std::vector<std::string> bloques_codigo(const std::string& texto, const std::vector<std::string>& lenguajes) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = texto.find("```", pos)) != std::string::npos) {
        size_t p = pos + 3;
        // lenguaje permitido (sin distinguir mayúsculas) o ninguno; después, espacios y salto de línea
        size_t fin_len = p;
        while (fin_len < texto.size() && (std::isalnum((unsigned char)texto[fin_len]) || texto[fin_len] == '_')) ++fin_len;
        std::string lang = lower(texto.substr(p, fin_len - p));
        size_t q = p;
        if (!lang.empty() && std::find(lenguajes.begin(), lenguajes.end(), lang) != lenguajes.end()) q = fin_len;
        while (q < texto.size() && (texto[q] == ' ' || texto[q] == '\t')) ++q;
        if (q < texto.size() && texto[q] == '\n') {
            size_t cierre = texto.find("```", q + 1);
            if (cierre == std::string::npos) break;
            std::string cont = texto.substr(q + 1, cierre - q - 1);
            while (!cont.empty() && cont.front() == '\n') cont.erase(cont.begin());
            while (!cont.empty() && cont.back() == '\n') cont.pop_back();
            out.push_back(cont);
            pos = cierre + 3;
        } else {
            pos += 1;
        }
    }
    return out;
}

std::string elegir_bloque(const std::string& texto, const std::vector<std::string>& lenguajes,
                          const std::function<bool(const std::string&)>& pista) {
    auto bs = bloques_codigo(texto, lenguajes);
    if (bs.empty()) {
        std::string t = replace_all(texto, "```", "");
        while (!t.empty() && t.front() == '\n') t.erase(t.begin());
        while (!t.empty() && t.back() == '\n') t.pop_back();
        return t;
    }
    for (auto it = bs.rbegin(); it != bs.rend(); ++it) if (pista(*it)) return *it;
    return *std::max_element(bs.begin(), bs.end(), [](const std::string& a, const std::string& b) { return a.size() < b.size(); });
}

std::string quitar_comentarios_lean(const std::string& c) {
    std::string out;
    size_t i = 0;
    while (i < c.size()) {
        if (c.compare(i, 2, "/-") == 0) {
            size_t e = c.find("-/", i + 2);
            if (e == std::string::npos) { out += c.substr(i); break; }
            i = e + 2;
        } else if (c.compare(i, 2, "--") == 0) {
            while (i < c.size() && c[i] != '\n') ++i;
        } else out.push_back(c[i++]);
    }
    return out;
}
std::string quitar_comentarios_py(const std::string& c) {
    std::string out;
    bool in = false;
    for (char ch : c) {
        if (ch == '#') in = true;
        if (ch == '\n') in = false;
        if (!in) out.push_back(ch);
    }
    return out;
}

// ---------------------------------------------------------------- procesos
std::atomic<bool>& bandera_parar() { static std::atomic<bool> b{false}; return b; }

#ifdef _WIN32
static std::wstring ancho(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
ResultadoProceso ejecutar(const std::vector<std::string>& argv, const OpcionesProceso& op) {
    ResultadoProceso r;
    fs::path tmp = fs::temp_directory_path() / ("demo_out_" + aleatorio_hex(10));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(tmp.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { r.no_ejecutado = true; r.codigo = -1; r.salida = "no se pudo crear el archivo de salida"; return r; }
    std::wstring cmd;
    for (auto& a : argv) {
        std::wstring w = ancho(a);
        bool q = w.find_first_of(L" \t\"") != std::wstring::npos || w.empty();
        if (!cmd.empty()) cmd += L" ";
        if (q) { cmd += L"\""; for (wchar_t c : w) { if (c == L'"') cmd += L"\\"; cmd += c; } cmd += L"\""; } else cmd += w;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h; si.hStdError = h; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = op.cwd.empty() ? L"" : op.cwd.wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(h);
    if (!ok) { r.no_ejecutado = true; r.codigo = 127; std::error_code ec; fs::remove(tmp, ec); r.salida = "no se pudo ejecutar " + argv[0]; return r; }
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > op.timeout_s) { TerminateProcess(pi.hProcess, 1); r.tiempo_agotado = true; break; }
        if (bandera_parar()) { TerminateProcess(pi.hProcess, 1); r.interrumpido = true; break; }
    }
    DWORD code = 0;
    WaitForSingleObject(pi.hProcess, 2000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    r.codigo = (int)code;
    try { r.salida = leer_archivo(tmp); } catch (...) {}
    std::error_code ec; fs::remove(tmp, ec);
    return r;
}
#else
ResultadoProceso ejecutar(const std::vector<std::string>& argv, const OpcionesProceso& op) {
    ResultadoProceso r;
    fs::path tmp = fs::temp_directory_path() / ("demo_out_" + aleatorio_hex(10));
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::vector<std::string> quitar;
    if (op.limpiar_secretos) {
        static const std::regex mal("(KEY|TOKEN|SECRET|PASS|CRED|AUTH)", std::regex::icase);
        for (char** e = environ; e && *e; ++e) {
            std::string s = *e, nombre = s.substr(0, s.find('='));
            if (std::regex_search(nombre, mal)) quitar.push_back(nombre);
        }
    }
    pid_t pid = fork();
    if (pid < 0) { r.no_ejecutado = true; r.codigo = -1; r.salida = "fork falló"; return r; }
    if (pid == 0) {
        setsid();
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        if (!op.cwd.empty() && chdir(op.cwd.c_str()) != 0) _exit(126);
        if (op.limites_py) {
            rlimit cpu{(rlim_t)(op.cpu_s > 0 ? op.cpu_s : op.timeout_s + 5), (rlim_t)((op.cpu_s > 0 ? op.cpu_s : op.timeout_s + 5) + 5)};
            setrlimit(RLIMIT_CPU, &cpu);
            rlimit as{(rlim_t)6 << 30, (rlim_t)6 << 30};
            setrlimit(RLIMIT_AS, &as);
        }
        for (auto& n : quitar) unsetenv(n.c_str());
        execvp(args[0], args.data());
        _exit(127);
    }
    auto t0 = std::chrono::steady_clock::now();
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) { status = 0; break; }
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > op.timeout_s) { kill(-pid, SIGKILL); waitpid(pid, &status, 0); r.tiempo_agotado = true; break; }
        if (bandera_parar()) { kill(-pid, SIGKILL); waitpid(pid, &status, 0); r.interrumpido = true; break; }
        dormir_ms(20);
    }
    if (WIFEXITED(status)) r.codigo = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) { r.senal = WTERMSIG(status); r.codigo = -r.senal; }
    if (r.codigo == 127 && !r.tiempo_agotado) r.no_ejecutado = true;
    try { r.salida = leer_archivo(tmp); } catch (...) {}
    std::error_code ec;
    fs::remove(tmp, ec);
    return r;
}
#endif

}  // namespace demo
