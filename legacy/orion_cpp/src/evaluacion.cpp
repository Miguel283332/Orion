#include "orion/evaluacion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <regex>

#include "orion/herramientas.hpp"
#include "orion/sistema.hpp"

namespace orion {

json ResultadoEvaluacion::a_json() const {
    return {{"ok", ok}, {"error", error}, {"aprobadas", aprobadas}, {"total", total}, {"puntuacion", puntuacion()},
            {"segundos", segundos}, {"tokens", tokens}, {"detalles", detalles}};
}

ResultadoEvaluacion ResultadoEvaluacion::de_json(const json& j) {
    ResultadoEvaluacion r;
    if (!j.is_object()) { r.error = "resultado no válido"; return r; }
    r.ok = j.value("ok", false);
    r.error = j.value("error", std::string());
    r.aprobadas = j.value("aprobadas", 0);
    r.total = j.value("total", 0);
    r.segundos = j.value("segundos", 0.0);
    r.tokens = j.value("tokens", 0LL);
    if (j.contains("detalles")) r.detalles = j["detalles"];
    return r;
}

std::vector<TareaEvaluacion> cargar_tareas(const fs::path& archivo, std::string* error) {
    std::vector<TareaEvaluacion> v;
    json j = json::parse(leer_archivo(archivo), nullptr, false);
    if (!j.is_object() || !j.contains("tareas") || !j["tareas"].is_array()) {
        if (error) *error = "no puedo leer las tareas de " + archivo.string();
        return v;
    }
    for (auto& t : j["tareas"]) {
        if (!t.is_object() || !t.contains("pregunta") || !t.contains("comprobar")) continue;
        TareaEvaluacion x;
        x.id = t.value("id", "t" + std::to_string(v.size() + 1));
        x.pregunta = t["pregunta"];
        if (t.contains("archivos") && t["archivos"].is_object()) x.archivos = t["archivos"];
        x.comprobar = t["comprobar"];
        v.push_back(x);
    }
    return v;
}

static std::vector<std::string> lista(const json& v) {
    std::vector<std::string> r;
    if (v.is_string()) r.push_back(v);
    if (v.is_array()) for (auto& x : v) if (x.is_string()) r.push_back(x);
    return r;
}

static bool comprobar(const json& c, const std::string& respuesta, const fs::path& dir, std::string& motivo) {
    std::string tipo = c.value("tipo", std::string("contiene"));
    std::string r = minusculas_sin_acentos(respuesta);
    if (tipo == "todas") {
        for (auto& x : c.value("comprobaciones", json::array()))
            if (!comprobar(x, respuesta, dir, motivo)) return false;
        return true;
    }
    if (tipo == "contiene" || tipo == "alguno" || tipo == "no_contiene") {
        auto vs = lista(c.value("valor", json()));
        int hay = 0;
        for (auto& v : vs) hay += contiene(r, minusculas_sin_acentos(v));
        bool ok = tipo == "contiene" ? hay == static_cast<int>(vs.size()) : tipo == "alguno" ? hay > 0 : hay == 0;
        if (!ok) motivo = (tipo == "no_contiene" ? "contiene algo que no debía: " : "falta: ") + join(vs, " | ");
        return ok;
    }
    if (tipo == "numero") {
        double objetivo = c.value("valor", 0.0), tol = c.value("tolerancia", 1e-6);
        static const std::regex num(R"([-+]?\d+(?:[.,]\d+)?)");
        std::string limpio = respuesta;
        for (size_t p; (p = limpio.find('\xC2')) != std::string::npos;) limpio.erase(p, 2);   // espacios duros, etc.
        for (auto it = std::sregex_iterator(limpio.begin(), limpio.end(), num); it != std::sregex_iterator(); ++it) {
            std::string n = it->str();
            for (auto& ch : n) if (ch == ',') ch = '.';
            if (std::fabs(std::atof(n.c_str()) - objetivo) <= tol * std::max(1.0, std::fabs(objetivo))) return true;
        }
        motivo = "no aparece el número " + c["valor"].dump();
        return false;
    }
    if (tipo == "archivo") {
        fs::path p = dir / c.value("ruta", std::string());
        std::error_code ec;
        if (!fs::exists(p, ec)) { motivo = "no existe " + c.value("ruta", std::string()); return false; }
        std::string cont = leer_archivo(p);
        for (auto& v : lista(c.value("contiene", json())))
            if (!contiene(minusculas_sin_acentos(cont), minusculas_sin_acentos(v))) { motivo = "el archivo no contiene " + v; return false; }
        return true;
    }
    if (tipo == "json") {
        json j = json::parse(extraer_json_objeto(respuesta), nullptr, false);
        std::string k = c.value("clave", std::string());
        bool ok = j.is_object() && j.contains(k) && (!c.contains("valor") || j[k] == c["valor"]);
        if (!ok) motivo = "no hay un JSON con " + k + (c.contains("valor") ? " = " + c["valor"].dump() : "");
        return ok;
    }
    motivo = "tipo de comprobación desconocido: " + tipo;
    return false;
}

bool comprobar_tarea(const TareaEvaluacion& t, const std::string& respuesta, const fs::path& dir, std::string* motivo) {
    std::string m;
    bool ok = comprobar(t.comprobar, respuesta, dir, m);
    if (motivo) *motivo = m;
    return ok;
}

ResultadoEvaluacion evaluar_en_proceso(const Config& cfg0, const fs::path& tareas, const fs::path& dir_prompts, const fs::path& dir_trabajo,
                                       const std::function<void(const std::string&)>& progreso) {
    ResultadoEvaluacion res;
    std::string err;
    auto ts = cargar_tareas(tareas, &err);
    if (ts.empty()) { res.error = err.empty() ? "no hay tareas" : err; return res; }
    Config cfg = cfg0;
    cfg.semilla = 42;   // misma semilla para todas las versiones: así las puntuaciones son comparables
    fs::path original = fs::current_path();
    double t0 = ahora_s();
    for (auto& t : ts) {
        fs::path dir = fs::absolute(dir_trabajo) / t.id;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / ".orion");
        for (auto& [nombre, contenido] : t.archivos.items()) escribir_atomico(dir / nombre, contenido.is_string() ? contenido.get<std::string>() : contenido.dump());
        Memoria mem(dir / ".orion");
        mem.cargar();
        Ollama llm(cfg.ollama, cfg.modelo);
        Mente mente(cfg, mem, llm);
        mente.fijar_dir_prompts(dir_prompts);
        mente.fijar_estado_actual("Estás en una evaluación automática de tus capacidades. Tu carpeta de trabajo actual es " + dir.string() + ".");
        ContextoHerramientas c;
        c.datos = dir / ".orion";
        c.memoria = &mem;
        c.origen = "evaluacion";
        Herramientas h(c);
        fs::current_path(dir);
        auto r = mente.responder(t.pregunta, nullptr, nullptr, &h);
        fs::current_path(original);
        res.tokens += r.tokens_generados;
        if (!r.ok && (contiene(r.error, "conectar") || contiene(r.error, "Ollama respondió 404") || contiene(r.error, "not found"))) {
            res.error = r.error;   // sin modelo no hay evaluación
            return res;
        }
        std::string motivo;
        bool ok = r.ok && comprobar_tarea(t, r.texto, dir, &motivo);
        if (!r.ok) motivo = r.error;
        res.total++;
        res.aprobadas += ok;
        res.detalles.push_back({{"id", t.id}, {"ok", ok}, {"motivo", motivo}, {"respuesta", recortar(r.texto, 300)}});
        if (progreso) progreso(std::string(ok ? "  ✓ " : "  ✗ ") + t.id + (ok ? "" : " (" + motivo + ")"));
    }
    res.segundos = ahora_s() - t0;
    res.ok = true;
    return res;
}

ResultadoEvaluacion evaluar_con_proceso(const fs::path& exe, const fs::path& tareas, const fs::path& dir_prompts, const Config& cfg,
                                        const fs::path& dir_trabajo, int timeout_s, const std::atomic<bool>* cancelar) {
    char temp[32];
    std::snprintf(temp, sizeof(temp), "%.3f", cfg.temperatura);
    std::vector<std::string> args = {exe.string(), "--evaluar", tareas.string(), "--datos", dir_trabajo.string(), "--ollama", cfg.ollama,
                                     "--modelo", cfg.modelo, "--temperatura", temp, "--max-pasos", std::to_string(cfg.max_pasos),
                                     "--num-ctx", std::to_string(cfg.num_ctx)};
    if (!dir_prompts.empty()) { args.push_back("--prompts"); args.push_back(dir_prompts.string()); }
    auto p = ejecutar_proceso(args, {}, timeout_s, "", cancelar);
    if (p.cancelado) {
        ResultadoEvaluacion r;
        r.error = "cancelada";
        return r;
    }
    for (auto& l : split_lines(p.salida))
        if (empieza_por(l, "RESULTADO_EVALUACION ")) return ResultadoEvaluacion::de_json(json::parse(l.substr(21), nullptr, false));
    ResultadoEvaluacion r;
    r.error = p.tiempo_agotado ? "la evaluación superó el tiempo máximo" : "la evaluación no terminó (código " + std::to_string(p.codigo) + "): " + recortar(p.salida, 400);
    return r;
}

}  // namespace orion
