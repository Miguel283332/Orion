#include "demo/llm.hpp"

#include <algorithm>
#include <sstream>

namespace demo {

std::string normalizar_url_ollama(const std::string& raw_in) {
    std::string raw = trim(raw_in.empty() ? env_o("OLLAMA_HOST", "http://127.0.0.1:11434") : raw_in);
    if (!contains(raw, "://")) raw = "http://" + raw;
    raw = replace_all(raw, "://0.0.0.0", "://127.0.0.1");
    while (!raw.empty() && raw.back() == '/') raw.pop_back();
    return raw;
}

OllamaLlm::OllamaLlm(const Config& cfg, double retardo) : base_(normalizar_url_ollama(cfg.ollama_url)), modelo_(cfg.modelo), num_ctx_(cfg.num_ctx), retardo_(retardo) {}

static std::string comillas(const std::string& s) {  // valor entre comillas para un archivo de configuración de curl
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"': o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o.push_back(c);
        }
    }
    return o + "\"";
}

json OllamaLlm::post(const std::string& ruta, const json& payload, int reintentos) {
    std::string ultimo;
    for (int i = 0; i <= reintentos; ++i) {
        if (bandera_parar()) throw Pausa("parada solicitada");
        fs::path td = crear_temporal("demo_http_");
        struct Limpia { fs::path p; ~Limpia() { std::error_code ec; fs::remove_all(p, ec); } } limpia{td};
        escribir_archivo(td / "req.json", payload.dump(-1, ' ', false, json::error_handler_t::replace));
        std::ostringstream c;
        c << "silent\nshow-error\nurl = " << comillas(base_ + ruta) << "\nrequest = \"POST\"\nnoproxy = \"*\"\nmax-time = 1800\n";
        c << "header = \"Content-Type: application/json\"\ndata-binary = " << comillas("@" + (td / "req.json").string()) << "\n";
        c << "output = " << comillas((td / "resp.json").string()) << "\nwrite-out = \"%{http_code}\"\n";
        escribir_archivo(td / "cfg", c.str());
        OpcionesProceso op;
        op.timeout_s = 1900;
        ResultadoProceso r = ejecutar({"curl", "-q", "-K", (td / "cfg").string()}, op);
        if (r.interrumpido || bandera_parar()) throw Pausa("parada solicitada");
        std::string codigo = trim(r.salida);
        if (r.codigo == 0 && !codigo.empty() && std::all_of(codigo.begin(), codigo.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
            std::string cuerpo = fs::exists(td / "resp.json") ? leer_archivo(td / "resp.json") : "";
            if (codigo != "200") throw ErrorModelo("Ollama respondió HTTP " + codigo + ": " + recortar(cuerpo, 300));
            json j = json::parse(cuerpo, nullptr, false);
            if (j.is_discarded()) throw ErrorModelo("Ollama devolvió una respuesta que no es JSON");
            return j;
        }
        ultimo = r.no_ejecutado ? "no se encuentra `curl` en el PATH" : recortar(trim(r.salida), 200);
        if (i < reintentos) dormir_ms((int)(std::min(30.0, retardo_ * (i + 1)) * 1000));
    }
    throw ErrorModelo("No se puede hablar con Ollama en " + base_ + " (" + ultimo + "). ¿Está en marcha? (ollama serve)");
}

std::string OllamaLlm::chat(const std::string& sistema, const std::string& usuario, double temperatura, int num_predict, int seed, bool json_mode) {
    json payload = {{"model", modelo_}, {"stream", false}, {"keep_alive", "60m"},
                    {"messages", json::array({{{"role", "system"}, {"content", sistema}}, {{"role", "user"}, {"content", usuario}}})},
                    {"options", {{"temperature", temperatura}, {"num_predict", num_predict}, {"num_ctx", num_ctx_}, {"seed", seed}}}};
    if (json_mode) payload["format"] = "json";
    json out = post("/api/chat", payload);
    if (out.contains("message") && out["message"].is_object()) return out["message"].value("content", std::string());
    return "";
}

}  // namespace demo
