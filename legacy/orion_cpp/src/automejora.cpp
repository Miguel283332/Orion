#include "orion/automejora.hpp"

#include "orion/sistema.hpp"

#include <algorithm>
#include <cstdio>

namespace orion {

Automejora::Automejora(ContextoAutomejora c, Ollama& llm) : c_(std::move(c)), llm_(llm) {}

std::string Automejora::disponible() const {
    std::error_code ec;
    if (c_.repo.empty() || !fs::exists(c_.repo / "CMakeLists.txt", ec)) return "no sé dónde está mi repositorio (arráncame desde él o con --repo)";
    if (!fs::exists(banco(), ec)) return "no encuentro mi banco de pruebas (" + banco().string() + ")";
    if (!fs::exists(examen(), ec)) return "no encuentro el examen de referencia (" + examen().string() + ")";
    if (c_.ejecutable.empty() || !fs::exists(c_.ejecutable, ec)) return "no encuentro mi propio ejecutable para medirme";
    return "";
}

std::string Automejora::aviso_examen() const {
    std::string actual = sha256_hex(leer_archivo(examen()));
    fs::path f = c_.datos / "examen_huella.txt";
    std::string guardada = trim(leer_archivo(f));
    if (guardada.empty()) {
        escribir_atomico(f, actual + "\n");
        return "";
    }
    return guardada == actual ? "" : "⚠ El examen de referencia ha cambiado desde que se anotó su huella (" + guardada.substr(0, 12) + "… → " +
                                         actual.substr(0, 12) + "…). Revisa quién lo cambió (git log evaluacion/).";
}

std::string Automejora::clave_medicion(const Config& cfg, const fs::path& tareas, const fs::path& prompts) const {
    std::error_code ec;
    auto tam = fs::exists(c_.ejecutable, ec) ? fs::file_size(c_.ejecutable, ec) : 0;
    auto t = fs::exists(c_.ejecutable, ec) ? fs::last_write_time(c_.ejecutable, ec).time_since_epoch().count() : 0;
    char p[64];
    std::snprintf(p, sizeof(p), "%.3f|%d|%d", cfg.temperatura, cfg.max_pasos, cfg.num_ctx);
    return sha256_hex(leer_archivo(prompts / "sistema.md") + "|" + leer_archivo(tareas) + "|" + cfg.modelo + "|" + p + "|" + std::to_string(tam) + "|" +
                      std::to_string(static_cast<long long>(t)));
}

ResultadoEvaluacion Automejora::medir(const Config& cfg, const fs::path& tareas, const fs::path& prompts, const std::atomic<bool>* cancelar) {
    fs::path cache = c_.datos / "automejora" / "mediciones.json";
    json c = json::parse(leer_archivo(cache), nullptr, false);
    if (!c.is_object()) c = json::object();
    std::string k = clave_medicion(cfg, tareas, prompts);
    if (c.contains(k)) return ResultadoEvaluacion::de_json(c[k]);
    auto r = evaluar_con_proceso(c_.ejecutable, tareas, prompts, cfg, c_.datos / "automejora" / "trabajo", 7200, cancelar);
    if (r.ok) {
        c[k] = r.a_json();
        escribir_atomico(cache, c.dump());
    }
    return r;
}

json Automejora::historial() const {
    json h = json::array();
    for (auto& l : split_lines(leer_archivo(c_.datos / "mejoras.jsonl"))) {
        json j = json::parse(l, nullptr, false);
        if (j.is_object()) h.push_back(j);
    }
    return h;
}

void Automejora::registrar(json e) const {
    e["n"] = static_cast<int>(historial().size()) + 1;
    e["fecha"] = fecha_hora();
    anadir_linea(c_.datos / "mejoras.jsonl", e.dump());
}

std::string Automejora::commit(const std::string& mensaje) const {
    std::error_code ec;
    if (!fs::exists(c_.repo / ".git", ec)) return "";
    auto a = ejecutar_proceso({"git", "-C", c_.repo.string(), "add", "prompts/sistema.md"}, {}, 30);
    if (a.codigo != 0) return "";
    auto c = ejecutar_proceso({"git", "-C", c_.repo.string(), "-c", "user.name=Orion", "-c", "user.email=orion@localhost", "commit", "-m", mensaje,
                               "--", "prompts/sistema.md"}, {}, 30);
    if (c.codigo != 0) return "";
    auto h = ejecutar_proceso({"git", "-C", c_.repo.string(), "rev-parse", "--short", "HEAD"}, {}, 15);
    return h.codigo == 0 ? trim(h.salida) : "";
}

static std::string porcentaje(const ResultadoEvaluacion& r) {
    return std::to_string(r.aprobadas) + "/" + std::to_string(r.total);
}

ResultadoMejora Automejora::ciclo(const Config& cfg, const std::atomic<bool>* cancelar, const std::function<void(const std::string&)>& aviso) {
    ResultadoMejora res;
    std::string no = disponible();
    if (!no.empty()) { res.texto = "no puedo mejorarme ahora: " + no; return res; }
    auto decir = [&](const std::string& t) { if (aviso) aviso(t); };
    std::string av_examen = aviso_examen();
    if (!av_examen.empty()) decir(av_examen);
    if (!fs::exists(prompts() / "sistema.md")) escribir_atomico(prompts() / "sistema.md", Mente::reglas_por_defecto() + "\n");
    // 1) medir la versión actual
    decir("mido mi versión actual con el banco de pruebas…");
    auto base = medir(cfg, banco(), prompts(), cancelar);
    if (cancelar && cancelar->load()) { res.cancelada = true; return res; }
    if (!base.ok) { res.texto = "no pude medirme: " + base.error; return res; }
    decir("versión actual: " + porcentaje(base) + " en el banco");
    // 2) proponer un cambio
    std::string actual = trim(leer_archivo(prompts() / "sistema.md"));
    std::string fallos;
    for (auto& d : base.detalles)
        if (!d.value("ok", false))
            fallos += "- " + d.value("id", std::string()) + ": " + d.value("motivo", std::string()) + ". Respondí: «" + recortar(d.value("respuesta", std::string()), 200) + "»\n";
    std::string intentos;
    auto h = historial();
    for (size_t i = h.size() > 6 ? h.size() - 6 : 0; i < h.size(); ++i)
        if (h[i].contains("motivo"))
            intentos += "- " + std::string(h[i].value("aceptada", false) ? "ACEPTADA" : "RECHAZADA") + ": " + h[i].value("motivo", std::string()) + " (" +
                        h[i].value("razon", std::string()) + ")\n";
    char params[128];
    std::snprintf(params, sizeof(params), "temperatura %.2f, herramientas por turno %d", cfg.temperatura, cfg.max_pasos);
    std::string prompt =
        "Propón UNA mejora concreta de tus propias instrucciones de comportamiento para hacerlo mejor en tu banco de pruebas.\n\n"
        "TUS INSTRUCCIONES ACTUALES (prompts/sistema.md):\n<<<\n" + actual + "\n>>>\n\nTUS PARÁMETROS: " + params + "\n\n"
        "RESULTADO EN EL BANCO: " + porcentaje(base) + "\n" + (fallos.empty() ? "(Lo aprobaste todo: busca ser más precisa o más breve.)\n" : "TAREAS QUE FALLASTE:\n" + fallos) +
        (intentos.empty() ? "" : "\nINTENTOS ANTERIORES (no repitas los rechazados):\n" + intentos) +
        "\nReglas: cambia lo mínimo necesario; conserva lo que funciona; reglas generales (no menciones las tareas concretas del banco); "
        "entre 200 y 3000 caracteres; en español.\n"
        "Devuelve SOLO JSON: {\"motivo\": \"qué cambias y por qué, en una frase\", \"sistema\": \"las instrucciones completas nuevas\", "
        "\"temperatura\": número entre 0.1 y 1.0, \"max_pasos\": entero entre 3 y 15}";
    OpcionesGeneracion op;
    op.temperatura = 0.7;
    op.num_ctx = cfg.num_ctx;
    op.num_predict = 1500;
    op.json = true;
    decir("pienso un cambio en mis instrucciones…");
    auto r = llm_.chat(json::array({{{"role", "system"}, {"content", "Eres Orion mejorándote a ti misma con cuidado: cada cambio se medirá."}},
                                    {{"role", "user"}, {"content", prompt}}}),
                       op, nullptr, cancelar);
    if (r.cancelada) { res.cancelada = true; return res; }
    if (!r.ok) { res.texto = "no pude proponer un cambio: " + r.error; return res; }
    json p = json::parse(extraer_json_objeto(r.texto), nullptr, false);
    if (!p.is_object() || !p.contains("sistema") || !p["sistema"].is_string()) { res.texto = "la propuesta no tenía el formato pedido"; return res; }
    std::string nuevo = trim(p["sistema"].get<std::string>()), motivo = p.value("motivo", std::string("sin motivo"));
    double temp = cfg.temperatura;
    int pasos = cfg.max_pasos;
    if (p.contains("temperatura") && p["temperatura"].is_number()) temp = std::clamp(p["temperatura"].get<double>(), 0.1, 1.0);
    if (p.contains("max_pasos") && p["max_pasos"].is_number()) pasos = std::clamp(p["max_pasos"].get<int>(), 3, 15);
    json entrada = {{"motivo", motivo}, {"sistema_antes", actual}, {"sistema_despues", nuevo}, {"antes", {{"temperatura", cfg.temperatura}, {"max_pasos", cfg.max_pasos}}},
                    {"despues", {{"temperatura", temp}, {"max_pasos", pasos}}}, {"banco_antes", porcentaje(base)}, {"examen_modificado", !av_examen.empty()}};
    auto rechazar = [&](const std::string& razon) {
        entrada["aceptada"] = false;
        entrada["razon"] = razon;
        registrar(entrada);
        res.hecho = true;
        res.texto = "propuesta rechazada (" + razon + "): " + motivo;
        return res;
    };
    if (nuevo.size() < 200 || nuevo.size() > 6000) return rechazar("longitud no válida: " + std::to_string(nuevo.size()) + " caracteres");
    if (nuevo == actual && temp == cfg.temperatura && pasos == cfg.max_pasos) return rechazar("no cambiaba nada");
    // 3) medir la propuesta
    fs::path cand = c_.datos / "automejora" / "candidato";
    escribir_atomico(cand / "sistema.md", nuevo + "\n");
    Config cc = cfg;
    cc.temperatura = temp;
    cc.max_pasos = pasos;
    decir("mido la propuesta: " + motivo);
    auto rb = medir(cc, banco(), cand, cancelar);
    if (cancelar && cancelar->load()) { res.cancelada = true; return res; }
    if (!rb.ok) return rechazar("no se pudo medir: " + rb.error);
    entrada["banco_despues"] = porcentaje(rb);
    if (rb.aprobadas <= base.aprobadas) return rechazar("no mejora el banco (" + porcentaje(base) + " → " + porcentaje(rb) + ")");
    // 4) confirmar con el examen de referencia que no empeora
    decir("mejora el banco (" + porcentaje(base) + " → " + porcentaje(rb) + "); lo confirmo con el examen de referencia…");
    auto eb = medir(cfg, examen(), prompts(), cancelar);
    if (cancelar && cancelar->load()) { res.cancelada = true; return res; }
    auto ec = medir(cc, examen(), cand, cancelar);
    if (cancelar && cancelar->load()) { res.cancelada = true; return res; }
    if (!eb.ok || !ec.ok) return rechazar("no se pudo medir el examen: " + (eb.ok ? ec.error : eb.error));
    entrada["examen_antes"] = porcentaje(eb);
    entrada["examen_despues"] = porcentaje(ec);
    if (ec.aprobadas < eb.aprobadas) return rechazar("empeora el examen de referencia (" + porcentaje(eb) + " → " + porcentaje(ec) + ")");
    // 5) aceptar
    escribir_atomico(prompts() / "sistema.md", nuevo + "\n");
    std::string hash = commit("Orion: mejora automática: " + motivo + " (banco " + porcentaje(base) + " → " + porcentaje(rb) + ", examen " + porcentaje(eb) +
                              " → " + porcentaje(ec) + ")");
    entrada["aceptada"] = true;
    entrada["razon"] = "mejora el banco y no empeora el examen";
    entrada["commit"] = hash;
    registrar(entrada);
    res.hecho = res.aceptada = true;
    res.temperatura = temp;
    res.max_pasos = pasos;
    res.texto = "mejora aceptada: " + motivo + " (banco " + porcentaje(base) + " → " + porcentaje(rb) + ", examen " + porcentaje(eb) + " → " + porcentaje(ec) + ")" +
                (hash.empty() ? "" : " · commit " + hash);
    return res;
}

ResultadoMejora Automejora::volver(int n, const Config& cfg) {
    ResultadoMejora res;
    for (auto& e : historial()) {
        if (e.value("n", 0) != n) continue;
        if (!e.value("aceptada", false) || !e.contains("sistema_antes")) { res.texto = "la entrada " + std::to_string(n) + " no es una mejora aceptada"; return res; }
        escribir_atomico(prompts() / "sistema.md", e["sistema_antes"].get<std::string>() + "\n");
        std::string hash = commit("Orion: vuelvo atrás la mejora " + std::to_string(n) + ": " + e.value("motivo", std::string()));
        json r = {{"tipo", "vuelta"}, {"deshace", n}, {"commit", hash}, {"aceptada", false}, {"razon", "vuelta atrás pedida"},
                  {"motivo", "deshacer la mejora " + std::to_string(n)}};
        registrar(r);
        res.hecho = res.aceptada = true;
        if (e.contains("antes")) {
            res.temperatura = e["antes"].value("temperatura", cfg.temperatura);
            res.max_pasos = e["antes"].value("max_pasos", cfg.max_pasos);
        }
        res.texto = "He vuelto a las instrucciones de antes de la mejora " + std::to_string(n) + (hash.empty() ? "" : " (commit " + hash + ")") + ".";
        return res;
    }
    res.texto = "No hay ninguna mejora con el número " + std::to_string(n) + ".";
    return res;
}

}  // namespace orion
