#include "orion/judge.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <regex>

#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
extern char** environ;
#endif

namespace orion {

std::optional<Autonomy> parse_autonomy(const std::string& s) {
    if (s == "strict") return Autonomy::Strict;
    if (s == "supervised") return Autonomy::Supervised;
    if (s == "full") return Autonomy::Full;
    return std::nullopt;
}

static const std::map<std::string, std::string>& risk_table() {
    static const std::map<std::string, std::string> t = {
        {"fs_list", "safe"}, {"fs_read", "safe"}, {"recall", "safe"}, {"remember", "safe"}, {"forget", "safe"},
        {"core_set", "safe"}, {"core_append", "safe"}, {"memory_stats", "safe"}, {"ask_user", "safe"}, {"done", "safe"},
        {"self_read", "safe"}, {"self_versions", "safe"},
        {"fs_write", "workspace_write"}, {"fs_delete", "workspace_write"}, {"download", "workspace_write"},
        {"web_get", "network_read"}, {"web_search", "network_read"},
        {"upload", "sensitive"}, {"shell", "sensitive"},
        {"self_patch", "gated"}, {"self_rollback", "gated"}};
    return t;
}
std::string risk_of(const std::string& tool) {
    auto it = risk_table().find(tool);
    return it == risk_table().end() ? "sensitive" : it->second;  // herramienta desconocida = sensible
}
bool is_taint_tool(const std::string& t) { return t == "web_get" || t == "web_search" || t == "download"; }

// ---------------------------------------------------------------- red
ParsedUrl parse_url(const std::string& url) {
    ParsedUrl u;
    size_t se = url.find("://");
    if (se == std::string::npos) return u;
    u.scheme = lower(url.substr(0, se));
    std::string rest = url.substr(se + 3);
    size_t slash = rest.find_first_of("/?#");
    std::string auth = slash == std::string::npos ? rest : rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    std::string port;
    if (!auth.empty() && auth[0] == '[') {
        size_t rb = auth.find(']');
        if (rb == std::string::npos) return u;
        u.host = auth.substr(1, rb - 1);
        if (rb + 1 < auth.size() && auth[rb + 1] == ':') port = auth.substr(rb + 2);
    } else {
        size_t colon = auth.rfind(':');
        if (colon != std::string::npos) { u.host = auth.substr(0, colon); port = auth.substr(colon + 1); } else { u.host = auth; }
    }
    u.port = u.scheme == "https" ? 443 : 80;
    if (!port.empty()) {
        if (!std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); })) return u;
        u.port = std::stoi(port);
    }
    u.ok = !u.host.empty();
    return u;
}

static void net_init() {
#ifdef _WIN32
    static bool done = false;
    if (!done) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); done = true; }
#endif
}

static bool v4_public(uint32_t a) {  // a en orden de host
    auto in = [&](uint32_t net, int bits) { uint32_t mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits); return (a & mask) == (net & mask); };
    static const struct { uint32_t net; int bits; } bad[] = {
        {0x00000000, 8}, {0x0A000000, 8}, {0x64400000, 10}, {0x7F000000, 8}, {0xA9FE0000, 16}, {0xAC100000, 12},
        {0xC0000000, 24}, {0xC0000200, 24}, {0xC0A80000, 16}, {0xC6120000, 15}, {0xC6336400, 24}, {0xCB007100, 24},
        {0xE0000000, 4}, {0xF0000000, 4}};
    for (auto& b : bad) if (in(b.net, b.bits)) return false;
    return true;
}

bool ip_is_public(const std::string& ip) {
    net_init();
    in_addr a4{};
    if (inet_pton(AF_INET, ip.c_str(), &a4) == 1) return v4_public(ntohl(a4.s_addr));
    in6_addr a6{};
    if (inet_pton(AF_INET6, ip.c_str(), &a6) == 1) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&a6);
        bool mapped = true;
        for (int i = 0; i < 10; ++i) if (b[i] != 0) mapped = false;
        if (mapped && b[10] == 0xff && b[11] == 0xff) return v4_public((uint32_t)b[12] << 24 | (uint32_t)b[13] << 16 | (uint32_t)b[14] << 8 | b[15]);
        bool nat64 = b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b;
        if (nat64) return v4_public((uint32_t)b[12] << 24 | (uint32_t)b[13] << 16 | (uint32_t)b[14] << 8 | b[15]);
        if ((b[0] & 0xE0) != 0x20) return false;                       // solo unicast global 2000::/3
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;  // documentación
        return true;
    }
    return false;  // no es una IP válida
}

std::optional<std::string> check_url(const std::string& url, bool allow_private, ParsedUrl* parsed, std::string* ip_out) {
    ParsedUrl u = parse_url(url);
    if (parsed) *parsed = u;
    if (u.scheme != "http" && u.scheme != "https") return std::string("solo se permiten http/https");
    if (!u.ok) return std::string("URL sin host o inválida");
    if (allow_private) return std::nullopt;
    net_init();
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res);
    if (rc != 0 || !res) return "no se puede resolver " + u.host;
    std::string first;
    std::optional<std::string> err;
    for (addrinfo* p = res; p; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        if (p->ai_family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, buf, sizeof(buf));
        else if (p->ai_family == AF_INET6) inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr, buf, sizeof(buf));
        else continue;
        if (!ip_is_public(buf)) { err = u.host + " apunta a una red privada/local (" + buf + "): bloqueado"; break; }
        if (first.empty()) first = buf;
    }
    freeaddrinfo(res);
    if (err) return err;
    if (ip_out) *ip_out = first;
    return std::nullopt;
}

// ---------------------------------------------------------------- secretos
static std::vector<std::pair<std::string, std::string>> secret_env() {
    std::vector<std::pair<std::string, std::string>> out;
#ifdef _WIN32
    char** env = _environ;
#else
    char** env = environ;
#endif
    for (char** e = env; e && *e; ++e) {
        std::string s = *e;
        if (starts_with(s, "ORION_SECRET_")) {
            size_t eq = s.find('=');
            if (eq != std::string::npos && s.size() - eq - 1 >= 6) out.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        }
    }
    return out;
}

std::string redact(const std::string& text) {
    static const std::regex re(
        "-----BEGIN [A-Z ]*PRIVATE KEY-----|\\bsk-[A-Za-z0-9_\\-]{20,}|\\bghp_[A-Za-z0-9]{30,}|\\bAKIA[0-9A-Z]{16}|\\bxox[baprs]-[A-Za-z0-9\\-]{10,}|\\bAIza[0-9A-Za-z_\\-]{30,}");
    std::string out = std::regex_replace(text, re, "[REDACTADO]");
    for (auto& kv : secret_env()) out = replace_all(out, kv.second, "[REDACTADO]");
    return out;
}

json resolve_secrets(const json& v) {
    if (v.is_string()) {
        static const std::regex re("\\{\\{secret:([A-Za-z0-9_]+)\\}\\}");
        std::string s = v.get<std::string>(), out;
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        size_t last = 0;
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            out += s.substr(last, it->position() - last);
            std::string name = (*it)[1];
            std::string up = name;
            for (auto& c : up) c = (char)std::toupper((unsigned char)c);
            const char* val = std::getenv(("ORION_SECRET_" + up).c_str());
            if (!val) throw std::runtime_error("secreto no definido: " + name);
            out += val;
            last = it->position() + it->length();
        }
        out += s.substr(last);
        return out;
    }
    if (v.is_object()) { json o = json::object(); for (auto& [k, x] : v.items()) o[k] = resolve_secrets(x); return o; }
    if (v.is_array()) { json a = json::array(); for (auto& x : v) a.push_back(resolve_secrets(x)); return a; }
    return v;
}

std::optional<std::string> looks_secret_file(const fs::path& p) {
    static const std::regex name_re(
        "(^|[\\\\/])(\\.env(\\..*)?|\\.netrc|\\.git-credentials|\\.aws|\\.ssh|id_(rsa|dsa|ecdsa|ed25519)(\\.pub)?|.*\\.(pem|key|p12|pfx|kdbx)|credentials?(\\..*)?|secrets?(\\..*)?|tokens?(\\..*)?)$",
        std::regex::icase);
    if (std::regex_search(p.string(), name_re)) return std::string("el nombre del archivo parece contener secretos");
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
        std::ifstream in(p, std::ios::binary);
        std::string head(200000, '\0');
        in.read(head.data(), (std::streamsize)head.size());
        head.resize((size_t)in.gcount());
        if (redact(head) != head) return std::string("el contenido parece contener una clave o token");
    }
    return std::nullopt;
}

fs::path safe_path(const fs::path& root, const std::string& rel, bool unconfined, bool write, const std::vector<fs::path>& prot) {
    fs::path root_c = fs::weakly_canonical(root);
    fs::path p(rel);
    fs::path full = p.is_absolute() ? p : root_c / p;
    full = fs::weakly_canonical(full);
    if (!unconfined) {
        fs::path r = full.lexically_relative(root_c);
        if (r.empty() || *r.begin() == "..") throw std::runtime_error("ruta fuera del espacio de trabajo: " + rel);
    }
    if (write) {
        for (const auto& pr : prot) {
            fs::path pc = fs::weakly_canonical(pr);
            fs::path r = full.lexically_relative(pc);
            if (full == pc || (!r.empty() && *r.begin() != "..")) throw std::runtime_error("ruta protegida (no se puede escribir): " + rel);
        }
    }
    return full;
}

// ---------------------------------------------------------------- auditoría
void Audit::log(json ev) {
    if (path_.empty()) return;
    ev["ts"] = now_str();
    std::string line = redact(ev.dump(-1, ' ', false, json::error_handler_t::replace));
    std::lock_guard<std::mutex> lk(mu_);
    write_file(path_, line + "\n", true);
}

// ---------------------------------------------------------------- permisos
std::vector<fs::path> Policy::protected_paths() const {
    std::vector<fs::path> p = extra_protected;
    if (!home.empty()) { p.push_back(home / "audit.jsonl"); p.push_back(home / "versions"); p.push_back(home / "approvals"); p.push_back(home / "STOP"); }
    return p;
}

std::pair<bool, std::string> Policy::authorize(const std::string& tool, const json& args) {
    std::string risk = risk_of(tool);
    if (autonomy == Autonomy::Full) return {true, "autonomía total"};
    if (risk == "safe" || risk == "gated") return {true, risk};
    if (autonomy == Autonomy::Supervised && (risk == "workspace_write" || risk == "network_read")) return {true, risk};
    if (tool == "upload" && args.is_object() && args.contains("url") && args["url"].is_string()) {
        ParsedUrl u = parse_url(args["url"].get<std::string>());
        std::string h = lower(u.host);
        if (!h.empty() && std::find(upload_hosts.begin(), upload_hosts.end(), h) != upload_hosts.end()) return {true, "host de subida permitido"};
    }
    if (session_allow.count(tool)) return {true, "permitido para la sesión"};
    std::string summary = tool + "(" + clip(args.dump(-1, ' ', false, json::error_handler_t::replace), 500) + ")";
    if (tainted) summary += " [AVISO: se leyó contenido web en este ciclo; podría ser una inyección de instrucciones]";
    if (ask(tool, summary)) return {true, "aprobado por el humano"};
    return {false, "no aprobado por el humano"};
}

bool Policy::confirm(const std::string& tool, const std::string& summary) {
    if (autonomy == Autonomy::Full) return true;
    return ask(tool, summary);
}

bool Policy::ask(const std::string& tool, const std::string& summary) {
    if (approver) return approver(tool, summary);
    if (interactive) {
        say("\n[PERMISO] El agente quiere ejecutar:\n  " + summary);
        std::cout << "  ¿Permitir? [s=sí / N=no / a=siempre en esta sesión] " << std::flush;
        std::string ans;
        if (!std::getline(std::cin, ans)) ans = "n";
        ans = lower(trim(ans));
        if (ans == "a" && tool != "self_patch" && tool != "self_rollback") { session_allow.insert(tool); return true; }
        return ans == "s" || ans == "si" || ans == "sí" || ans == "y" || ans == "yes";
    }
    return file_approval(tool, summary);
}

bool Policy::file_approval(const std::string& tool, const std::string& summary) {
    if (home.empty()) return false;
    fs::path d = home / "approvals";
    fs::create_directories(d);
    std::string id = rand_hex(8);
    write_file(d / (id + ".json"), json{{"id", id}, {"tool", tool}, {"summary", summary}, {"ts", now_str()}}.dump());
    say("[PERMISO PENDIENTE " + id + "] " + summary + "\n  -> orion_agent approve " + id + "   (o deny " + id + ")");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(approval_timeout * 1000));
    fs::path ans = d / (id + ".answer");
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        if (fs::exists(ans, ec)) {
            std::string v = lower(trim(read_file(ans)));
            fs::remove(ans, ec);
            fs::remove(d / (id + ".json"), ec);
            return v == "yes" || v == "si" || v == "sí" || v == "y";
        }
        sleep_ms(300);
    }
    std::error_code ec;
    fs::remove(d / (id + ".json"), ec);
    return false;
}

std::pair<bool, std::string> guarded_call(Policy& pol, Audit& audit, const std::map<std::string, Tool>& tools,
                                          const std::string& name, const json& args) {
    if (!args.is_object()) return {false, "args debe ser un objeto JSON"};
    auto it = tools.find(name);
    if (it == tools.end()) {
        audit.log({{"event", "tool"}, {"tool", name}, {"decision", "desconocida"}});
        std::string names;
        for (auto& kv : tools) names += (names.empty() ? "" : ", ") + kv.first;
        return {false, "herramienta desconocida '" + name + "'. Disponibles: " + names};
    }
    auto [allowed, why] = pol.authorize(name, args);
    if (!allowed) {
        audit.log({{"event", "tool"}, {"tool", name}, {"args", args}, {"decision", "denegada"}, {"reason", why}});
        return {false, "DENEGADO: " + why + ". Prueba otra vía o usa ask_user."};
    }
    std::string out;
    bool ok = true;
    try {
        out = it->second.fn(args);
    } catch (const std::exception& e) {
        out = "error en " + name + ": " + e.what() + ". Uso: " + name + "(" + it->second.params + ")";
        ok = false;
    }
    if (is_taint_tool(name)) pol.tainted = true;
    out = redact(out);
    audit.log({{"event", "tool"}, {"tool", name}, {"args", args}, {"decision", "permitida"}, {"reason", why}, {"ok", ok}, {"out_chars", out.size()}});
    return {ok, out};
}

// ---------------------------------------------------------------- compuerta de automodificación
bool SelfGate::is_protected(const std::string& rel_in) {
    std::string rel = fs::path(rel_in).lexically_normal().generic_string();
    static const char* files[] = {"include/orion/judge.hpp", "src/judge.cpp", "CMakeLists.txt"};
    for (const char* f : files) if (rel == f) return true;
    return starts_with(rel, "tests/") || starts_with(rel, "tools/") || starts_with(rel, "legacy/");
}

std::string SelfGate::apply_edits(const json& edits, std::map<std::string, std::string>& out) const {
    if (!edits.is_array() || edits.empty() || edits.size() > 20) return "edits debe ser una lista de 1 a 20 objetos {file, old, new}";
    for (size_t i = 0; i < edits.size(); ++i) {
        const json& e = edits[i];
        if (!e.is_object() || !e.contains("file") || !e["file"].is_string() || !e.contains("old") || !e["old"].is_string() ||
            !e.contains("new") || !e["new"].is_string())
            return "edit " + std::to_string(i) + ": formato inválido (se espera {file, old, new})";
        std::string rel = fs::path(e["file"].get<std::string>()).lexically_normal().generic_string();
        if (rel.empty() || starts_with(rel, "..") || fs::path(rel).is_absolute()) return "edit " + std::to_string(i) + ": ruta no permitida";
        if (is_protected(rel)) return "edit " + std::to_string(i) + ": '" + rel + "' pertenece al JUEZ (protegido): rechazado";
        if (!(starts_with(rel, "src/") || starts_with(rel, "include/"))) return "edit " + std::to_string(i) + ": solo se puede editar src/ e include/";
        if (!out.count(rel)) {
            fs::path f = root_ / rel;
            if (!fs::is_regular_file(f)) return "edit " + std::to_string(i) + ": no existe " + rel;
            out[rel] = read_file(f);
        }
        std::string old = e["old"].get<std::string>();
        if (old.empty()) return "edit " + std::to_string(i) + ": 'old' vacío";
        size_t n = count_occurrences(out[rel], old);
        if (n != 1) return "edit " + std::to_string(i) + ": 'old' aparece " + std::to_string(n) + " veces en " + rel + " (debe ser exactamente 1)";
        size_t pos = out[rel].find(old);
        out[rel].replace(pos, old.size(), e["new"].get<std::string>());
    }
    return "";
}

std::string SelfGate::default_build_check(const fs::path& cand) {
    auto step = [&](const std::string& cmd, const char* what) -> std::string {
        ProcResult r = run_cmd(cmd + " 2>&1");
        if (r.code != 0) {
            auto lines = split(r.out, '\n');
            std::string tail;
            for (size_t i = lines.size() > 14 ? lines.size() - 14 : 0; i < lines.size(); ++i) tail += "  " + lines[i] + "\n";
            return std::string("falla ") + what + ":\n" + tail;
        }
        return "";
    };
    std::string b = shell_quote((cand / "build").string()), s = shell_quote(cand.string());
    std::string e;
    if (!(e = step("cmake -S " + s + " -B " + b + " -DCMAKE_BUILD_TYPE=Release", "la configuración")).empty()) return e;
    if (!(e = step("cmake --build " + b + " --config Release", "la compilación")).empty()) return e;
    if (!(e = step("cmake -E chdir " + b + " ctest -C Release --output-on-failure", "las pruebas (ctest)")).empty()) return e;
    return "";
}

std::string SelfGate::validate_and_install(const std::map<std::string, std::string>& files, const std::string& what,
                                           const std::string& note, const std::string& tool) {
    // 1) copiar el árbol de código a un directorio temporal y aplicar los cambios
    fs::path cand = make_temp_dir("orion_gate_");
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{cand};
    for (const char* top : {"include", "src", "tests", "tools", "cmake"}) {
        if (fs::exists(root_ / top)) fs::copy(root_ / top, cand / top, fs::copy_options::recursive);
    }
    if (fs::exists(root_ / "CMakeLists.txt")) fs::copy_file(root_ / "CMakeLists.txt", cand / "CMakeLists.txt");
    for (auto& [rel, content] : files) {
        if (is_protected(rel)) return "RECHAZADO: toca el juez (" + rel + ")";
        write_file(cand / rel, content);
    }
    // 2) el juez del candidato debe ser idéntico al actual
    for (const char* rel : {"include/orion/judge.hpp", "src/judge.cpp", "CMakeLists.txt"}) {
        if (fs::exists(root_ / rel) && read_file(root_ / rel) != read_file(cand / rel)) return std::string("RECHAZADO: el juez (") + rel + ") cambió";
    }
    // 3) compilar y pasar las pruebas del candidato
    std::string problem = build_check ? build_check(cand) : default_build_check(cand);
    if (!problem.empty()) {
        audit_.log({{"event", tool}, {"decision", "rechazado"}, {"problems", problem}, {"note", note}});
        return std::string(tool == "self_patch" ? "PARCHE" : "ROLLBACK") + " RECHAZADO por la compuerta:\n" + problem;
    }
    // 4) aprobación humana
    if (!pol_.confirm(tool, what + ". Motivo: " + note)) {
        audit_.log({{"event", tool}, {"decision", "no aprobado"}, {"note", note}});
        return "Válido (compila y pasa las pruebas) pero no aprobado por el humano.";
    }
    // 5) instalar con copia de seguridad
    fs::create_directories(versions_);
    int n = 1;
    for (auto& e : fs::directory_iterator(versions_)) { (void)e; ++n; }
    char tsb[32];
    std::time_t t = std::time(nullptr);
    std::strftime(tsb, sizeof(tsb), "%Y%m%d_%H%M%S", std::localtime(&t));
    char nb[16];
    std::snprintf(nb, sizeof(nb), "v%03d_", n);
    fs::path vdir = versions_ / (std::string(nb) + tsb);
    for (auto& [rel, content] : files) {
        fs::path old = root_ / rel;
        if (fs::exists(old)) { fs::create_directories((vdir / rel).parent_path()); fs::copy_file(old, vdir / rel); }
        write_file(root_ / rel, content);
    }
    restart_requested = true;
    audit_.log({{"event", tool}, {"decision", "aplicado"}, {"backup", vdir.filename().string()}, {"note", note}});
    return "APLICADO al código fuente. Copia anterior: " + vdir.filename().string() +
           ". Recompila (cmake --build) y reinicia para usar el código nuevo.";
}

std::string SelfGate::propose(const json& edits, const std::string& note) {
    std::map<std::string, std::string> files;
    std::string err = apply_edits(edits, files);
    if (!err.empty()) {
        audit_.log({{"event", "self_patch"}, {"decision", "rechazado"}, {"problems", err}, {"note", note}});
        return "PARCHE RECHAZADO: " + err;
    }
    bool changed = false;
    for (auto& [rel, c] : files) if (read_file(root_ / rel) != c) changed = true;
    if (!changed) return "PARCHE RECHAZADO: no cambia nada";
    return validate_and_install(files, "aplicar parche al propio código (" + std::to_string(files.size()) + " archivos)", note, "self_patch");
}

std::vector<std::string> SelfGate::versions() const {
    std::vector<std::string> v;
    std::error_code ec;
    if (fs::exists(versions_, ec)) for (auto& e : fs::directory_iterator(versions_)) v.push_back(e.path().filename().string());
    std::sort(v.begin(), v.end());
    return v;
}

std::string SelfGate::rollback(const std::string& version) {
    fs::path vdir = versions_ / fs::path(version).filename();
    if (!fs::is_directory(vdir)) return "no existe la versión " + version;
    std::map<std::string, std::string> files;
    for (auto& e : fs::recursive_directory_iterator(vdir)) {
        if (!e.is_regular_file()) continue;
        std::string rel = e.path().lexically_relative(vdir).generic_string();
        if (is_protected(rel)) return "ROLLBACK RECHAZADO: la copia contiene archivos del juez (" + rel + ")";
        files[rel] = read_file(e.path());
    }
    if (files.empty()) return "la versión está vacía";
    return validate_and_install(files, "volver a la versión " + version, "rollback", "self_rollback");
}

}  // namespace orion
