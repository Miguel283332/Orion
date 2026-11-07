// JUEZ (protegido): permisos, red, archivos, auditoría y compuerta de automodificación.
// Este archivo, judge.cpp, tests/ y CMakeLists.txt NO pueden modificarse con self_patch.
#pragma once
#include <mutex>
#include <set>

#include "orion/util.hpp"

namespace orion {

enum class Autonomy { Strict, Supervised, Full };
std::optional<Autonomy> parse_autonomy(const std::string& s);

std::string risk_of(const std::string& tool);  // safe | workspace_write | network_read | sensitive | gated
bool is_taint_tool(const std::string& tool);   // lee contenido no confiable (web, descargas)

// ---- red (SSRF)
struct ParsedUrl { std::string scheme, host, path; int port = 0; bool ok = false; };
ParsedUrl parse_url(const std::string& url);
bool ip_is_public(const std::string& ip);
// Devuelve el motivo si hay que bloquear. Si es válida y `ip` != nullptr, devuelve la IP verificada (para fijarla).
std::optional<std::string> check_url(const std::string& url, bool allow_private, ParsedUrl* parsed = nullptr,
                                     std::string* ip = nullptr);

// ---- secretos y rutas
std::string redact(const std::string& text);
json resolve_secrets(const json& v);  // {{secret:NOMBRE}} -> ORION_SECRET_NOMBRE; lanza si falta
std::optional<std::string> looks_secret_file(const fs::path& p);
fs::path safe_path(const fs::path& root, const std::string& rel, bool unconfined, bool write,
                   const std::vector<fs::path>& protected_paths);

class Audit {
public:
    explicit Audit(fs::path path = {}) : path_(std::move(path)) {}
    void log(json ev);
private:
    fs::path path_;
    std::mutex mu_;
};

struct Tool {
    std::string name, params, doc;
    std::function<std::string(const json&)> fn;
};

class Policy {
public:
    using Approver = std::function<bool(const std::string& tool, const std::string& summary)>;
    Autonomy autonomy = Autonomy::Supervised;
    fs::path workspace, home;
    Approver approver;
    bool allow_private = false;
    std::vector<std::string> upload_hosts;
    double approval_timeout = 600.0;
    bool interactive = false;
    bool tainted = false;
    std::set<std::string> session_allow;
    std::vector<fs::path> extra_protected;

    bool unconfined() const { return autonomy == Autonomy::Full; }
    std::vector<fs::path> protected_paths() const;
    void new_cycle() { tainted = false; }
    std::pair<bool, std::string> authorize(const std::string& tool, const json& args);
    bool confirm(const std::string& tool, const std::string& summary);  // para acciones con compuerta propia
    bool ask(const std::string& tool, const std::string& summary);
private:
    bool file_approval(const std::string& tool, const std::string& summary);
};

// ÚNICO punto por el que el agente ejecuta herramientas: autoriza, ejecuta, audita, redacta.
std::pair<bool, std::string> guarded_call(Policy& pol, Audit& audit, const std::map<std::string, Tool>& tools,
                                          const std::string& name, const json& args);

// ---- compuerta de automodificación del código fuente (C++)
class SelfGate {
public:
    SelfGate(fs::path src_root, fs::path versions_dir, Policy& pol, Audit& audit)
        : root_(std::move(src_root)), versions_(std::move(versions_dir)), pol_(pol), audit_(audit) {}
    // Devuelve "" si el candidato compila y pasa las pruebas; si no, el motivo. Sustituible en pruebas.
    std::function<std::string(const fs::path& candidate_dir)> build_check;
    bool restart_requested = false;

    std::string propose(const json& edits, const std::string& note);  // edits: [{file, old, new}]
    std::string rollback(const std::string& version);
    std::vector<std::string> versions() const;
    static bool is_protected(const std::string& rel);
    static std::string default_build_check(const fs::path& candidate_dir);
    // aplica las ediciones sobre el contenido actual; error != "" si no son válidas
    std::string apply_edits(const json& edits, std::map<std::string, std::string>& out) const;
    const fs::path& root() const { return root_; }

private:
    std::string validate_and_install(const std::map<std::string, std::string>& files, const std::string& what,
                                     const std::string& note, const std::string& tool);
    fs::path root_, versions_;
    Policy& pol_;
    Audit& audit_;
};

}  // namespace orion
