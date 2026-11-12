#include "orion/ollama.hpp"

#include "orion/util.hpp"

namespace orion {

Ollama::Ollama(const std::string& url, std::string modelo) : http_(url), modelo_(std::move(modelo)) {}

static std::string error_de(const RespuestaHttp& r) {
    if (!r.error.empty())
        return r.error + ". ¿Está Ollama en marcha? (abre la aplicación Ollama o ejecuta `ollama serve`)";
    json j = json::parse(r.cuerpo, nullptr, false);
    if (j.is_object() && j.contains("error")) return "Ollama: " + j["error"].get<std::string>();
    return "Ollama respondió " + std::to_string(r.estado) + ": " + recortar(r.cuerpo, 200);
}

bool Ollama::disponible(std::string* version) const {
    auto r = http_.get("/api/version", 5);
    if (r.estado != 200) return false;
    json j = json::parse(r.cuerpo, nullptr, false);
    if (version && j.is_object()) *version = j.value("version", std::string("?"));
    return true;
}

ResultadoGeneracion Ollama::chat(const json& mensajes, const OpcionesGeneracion& op, const std::function<void(const std::string&)>& por_trozo,
                                 const std::atomic<bool>* cancelar) const {
    json opciones = {{"temperature", op.temperatura}, {"num_ctx", op.num_ctx}, {"num_predict", op.num_predict}};
    if (op.semilla >= 0) opciones["seed"] = op.semilla;
    json cuerpo = {{"model", modelo_}, {"messages", mensajes}, {"stream", true}, {"keep_alive", "60m"}, {"options", opciones}};
    if (op.json) cuerpo["format"] = "json";
    ResultadoGeneracion res;
    std::string error_stream;
    auto r = http_.post_stream("/api/chat", cuerpo.dump(), [&](const std::string& linea) {
        json j = json::parse(linea, nullptr, false);
        if (!j.is_object()) return true;
        if (j.contains("error")) { error_stream = j["error"].get<std::string>(); return false; }
        if (j.contains("message") && j["message"].contains("content")) {
            std::string t = j["message"]["content"].get<std::string>();
            if (!t.empty()) {
                res.texto += t;
                if (por_trozo) por_trozo(t);
            }
        }
        if (j.value("done", false)) {
            res.tokens_prompt = j.value("prompt_eval_count", 0LL);
            res.tokens_generados = j.value("eval_count", 0LL);
            res.segundos_prompt = j.value("prompt_eval_duration", 0LL) / 1e9;
            res.segundos_generacion = j.value("eval_duration", 0LL) / 1e9;
        }
        return true;
    }, cancelar, 1800);
    res.cancelada = r.cancelada && error_stream.empty();
    if (!error_stream.empty()) res.error = "Ollama: " + error_stream;
    else if (r.estado != 200 && !res.cancelada) res.error = error_de(r);
    res.ok = res.error.empty() && !res.cancelada;
    return res;
}

InfoModelo Ollama::info() const {
    InfoModelo i;
    i.nombre = modelo_;
    auto r = http_.post("/api/show", json{{"model", modelo_}}.dump(), 30);
    if (r.estado != 200) {
        i.error = error_de(r);
        if (r.estado == 404) i.error += "\nDescárgalo con:  ollama pull " + modelo_;
        return i;
    }
    json j = json::parse(r.cuerpo, nullptr, false);
    if (!j.is_object()) { i.error = "respuesta no válida de /api/show"; return i; }
    i.ok = true;
    if (j.contains("details")) {
        auto& d = j["details"];
        i.familia = d.value("family", std::string());
        i.cuantizacion = d.value("quantization_level", std::string());
        i.tamano_parametros = d.value("parameter_size", std::string());
    }
    if (j.contains("model_info") && j["model_info"].is_object()) {
        for (auto& [k, v] : j["model_info"].items()) {
            if (k == "general.parameter_count" && v.is_number()) i.parametros = v.get<double>();
            if (k.size() > 15 && k.compare(k.size() - 15, 15, ".context_length") == 0 && v.is_number()) i.contexto_max = v.get<long long>();
        }
    }
    auto ps = http_.get("/api/ps", 10);
    json p = json::parse(ps.cuerpo, nullptr, false);
    if (ps.estado == 200 && p.is_object() && p.contains("models")) {
        for (auto& m : p["models"]) {
            std::string n = m.value("name", m.value("model", std::string()));
            if (n == modelo_ || n == modelo_ + ":latest") {
                i.cargado = true;
                i.bytes_en_memoria = m.value("size", 0.0);
                i.bytes_en_vram = m.value("size_vram", 0.0);
            }
        }
    }
    return i;
}

}  // namespace orion
