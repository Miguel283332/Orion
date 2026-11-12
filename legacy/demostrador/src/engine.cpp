#include "demo/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <cmath>
#include <ctime>
#include <regex>
#include <set>

namespace demo {

static const double TEMPERATURAS[] = {0.1, 0.25, 0.4, 0.55, 0.7, 0.85};

static std::string jd(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }
static std::string primera_linea(const std::string& s) { auto l = split_lines(s); return l.empty() ? "" : l[0]; }
static std::string ultima_linea(const std::string& s) {
    auto l = split_lines(s);
    while (!l.empty() && trim(l.back()).empty()) l.pop_back();
    return l.empty() ? "" : l.back();
}

Motor::Motor(const Config& cfg, Llm& llm, Verificador& ver, fs::path directorio, std::function<void(const std::string&)> log)
    : cfg_(cfg), llm_cli_(llm), ver_(ver), dir_(std::move(directorio)), log_(std::move(log)), t_inicio_(std::chrono::steady_clock::now()) {
    fs::create_directories(dir_);
    estado_ = json::object();
    if (!log_) log_ = [](const std::string& s) { std::cout << s << std::endl; };
}

// ---------------------------------------------------------------- persistencia
std::string Motor::huella_problema(const json& p) {
    return huella(p["enunciado"].get<std::string>() + "|" + p.value("backend", std::string("lean")) + "|" + ((cfg_.mathlib || !cfg_.lake_dir.empty()) ? "True" : "False"));
}

json Motor::nuevo_estado(const json& problema) {
    return {{"version", ESTADO_VERSION}, {"problema", problema}, {"huella", huella_problema(problema)}, {"creado", ahora()}, {"estado", "formalizando"},
            {"formalizado", false}, {"definiciones", ""}, {"preambulo", ""}, {"variables", json::array()}, {"criterios", json::array()}, {"raiz", nullptr},
            {"contador", 0}, {"llamadas", 0}, {"tiempo", 0.0}, {"intentos_formalizar", 0}, {"ultimo_error", ""},
            {"resultado", {{"codigo", ""}, {"archivo", ""}}}};
}

void Motor::cargar(const json& problema, bool reiniciar) {
    auto copia = [&](const char* sufijo) {
        fs::rename(ruta_estado(), dir_ / ("estado." + std::to_string(std::time(nullptr)) + "." + sufijo + ".json"));
    };
    if (reiniciar && fs::exists(ruta_estado())) copia("bak");
    if (fs::exists(ruta_estado())) {
        json datos = json::parse(leer_archivo(ruta_estado()), nullptr, false);
        bool espec_nueva = false;
        if (!datos.is_discarded() && datos.is_object() && problema.contains("especificacion") && problema["especificacion"].is_object()
            && datos.value("estado", std::string()) != "verificado" && datos.value("version", 0) == ESTADO_VERSION
            && datos.value("huella", std::string()) == huella_problema(problema)) {
            const json& e = problema["especificacion"];
            espec_nueva = datos.value("criterios", json::array()) != e.value("criterios", json::array())
                       || datos.value("preambulo", std::string()) != trim(e.value("preambulo", std::string()));
        }
        if (datos.is_discarded() || !datos.is_object()) {
            log_("Estado ilegible; se empieza de nuevo.");
            copia("corrupto");
        } else if (espec_nueva) {
            log_("Hay una especificación revisada para este problema y el estado guardado usa otra (escrita por el modelo): se guarda copia y se empieza con la revisada.");
            copia("bak");
        } else if (datos.value("version", 0) == ESTADO_VERSION && datos.value("huella", std::string()) == huella_problema(problema)) {
            estado_ = datos;
            t_base_ = datos.value("tiempo", 0.0);
            char b[64];
            std::snprintf(b, sizeof(b), "%.1f", t_base_ / 60);
            log_("Reanudando: " + datos.value("estado", std::string()) + ", " + std::to_string(datos.value("llamadas", 0)) + " llamadas y " + b + " min de trabajo previos.");
            if (estado_["estado"] == "fallido") reabrir_fallidos();
            return;
        } else {
            log_("El estado guardado es de otro problema o versión: se conserva como copia y se empieza de nuevo.");
            copia("bak");
        }
    }
    estado_ = nuevo_estado(problema);
    guardar();
}

// Un estado "fallido" significa que se agotaron los límites. Al volver a ejecutar se reintenta: los contadores vuelven a
// cero, pero se conservan los lemas demostrados, el historial de errores y las demostraciones ya descartadas (huellas).
void Motor::reabrir_fallidos() {
    estado_["estado"] = estado_.value("formalizado", false) ? "resolviendo" : "formalizando";
    estado_["intentos_formalizar"] = 0;
    estado_.erase("error_fatal");
    std::function<void(json&)> abrir = [&](json& n) {
        if (n["estado"] == "fallido") { n["estado"] = "pendiente"; n["directos"] = 0; n["finales"] = 0; n["replanes"] = 0; n["necesita_plan"] = true; }
        for (auto& h : n["hijos"]) abrir(h);
    };
    if (estado_.contains("raiz") && estado_["raiz"].is_object()) abrir(estado_["raiz"]);
    log_("La ejecución anterior terminó sin resolverse: se reintenta con los contadores a cero (se conservan los lemas demostrados y las demostraciones ya descartadas).");
    guardar();
}

void Motor::guardar() {
    estado_["tiempo"] = t_base_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - t_inicio_).count();
    escribir_atomico(ruta_estado(), estado_.dump(1, ' ', false, json::error_handler_t::replace));
}

void Motor::evento(const std::string& nombre, json datos) {
    json rec = {{"ts", ahora()}, {"t", std::round(estado_.value("tiempo", 0.0) * 10) / 10}, {"evento", nombre}};
    for (auto& [k, v] : datos.items()) rec[k] = v;
    escribir_archivo(dir_ / "registro.jsonl", jd(rec) + "\n", true);
}

void Motor::say(const json* n, const std::string& msg) {
    double t = t_base_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - t_inicio_).count();
    char b[64];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d", (int)(t / 3600), (int)((long)t % 3600 / 60), (int)((long)t % 60));
    log_(std::string("[") + b + " | llamadas " + std::to_string(estado_.value("llamadas", 0)) + (n ? " | " + (*n)["id"].get<std::string>() : "") + "] " + msg);
}

// ---------------------------------------------------------------- presupuesto y modelo
void Motor::presupuesto() {
    if (bandera_parar()) throw Pausa("parada solicitada");
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_inicio_).count();
    if (cfg_.horas > 0 && el >= cfg_.horas * 3600) throw Pausa("límite de " + std::to_string(cfg_.horas) + " h de esta sesión");
    if (cfg_.max_intentos > 0 && llamadas_sesion_ >= cfg_.max_intentos) throw Pausa("límite de " + std::to_string(cfg_.max_intentos) + " llamadas al modelo en esta sesión");
}

std::string Motor::llm_(const std::string& prompt, double temperatura, int seed, bool json_mode, int num_predict) {
    presupuesto();
    estado_["llamadas"] = estado_.value("llamadas", 0) + 1;
    ++llamadas_sesion_;
    try {
        std::string r = llm_cli_.chat(ver_.sistema, prompt, temperatura, num_predict > 0 ? num_predict : cfg_.num_predict, seed, json_mode);
        guardar();
        return r;
    } catch (...) {
        guardar();
        throw;
    }
}

// ---------------------------------------------------------------- FASE 0
void Motor::formalizar() {
    if (estado_["formalizado"].get<bool>()) return;
    say(nullptr, "FASE 0: formalización del enunciado");
    std::string problema = estado_["problema"]["enunciado"];
    std::string error = estado_.value("ultimo_error", std::string());
    if (cfg_.backend == "sympy" && estado_["problema"].contains("especificacion") && estado_["problema"]["especificacion"].is_object()) {
        try {   // especificación revisada por una persona: sin llamar al modelo
            auto [firma, desc] = validar_formalizacion(estado_["problema"]["especificacion"]);
            estado_["formalizado"] = true;
            estado_["estado"] = "resolviendo";
            estado_["raiz"] = nodo(firma, 0);
            estado_["ultimo_error"] = "";
            guardar();
            evento("formalizado", {{"firma", firma}, {"especificacion", "incluida"}});
            std::string crit;
            for (auto& c : estado_["criterios"]) crit += (crit.empty() ? "" : " ; ") + c.get<std::string>();
            say(nullptr, "Especificación incluida (revisada, sin usar el modelo):\n" + firma + "\nCriterios: " + crit);
            return;
        } catch (const ErrorValor& e) {
            say(nullptr, std::string("La especificación incluida no es válida (") + ultima_linea(e.what()) + "); se pide al modelo.");
        }
    }
    while (estado_["intentos_formalizar"].get<int>() < cfg_.formalizar) {
        estado_["intentos_formalizar"] = estado_["intentos_formalizar"].get<int>() + 1;
        int i = estado_["intentos_formalizar"];
        try {
            std::string resp = llm_(ver_.prompt_formalizar(problema, error), 0.1 + 0.03 * i, cfg_.semilla * 7919 + i, true);
            auto datos = extraer_json(resp);
            if (!datos) throw ErrorValor("el modelo no devolvió JSON");
            auto [firma, desc] = validar_formalizacion(*datos);
            if (cfg_.fidelidad) fidelidad(problema, firma);
            estado_["formalizado"] = true;
            estado_["estado"] = "resolviendo";
            estado_["raiz"] = nodo(firma, 0);
            estado_["ultimo_error"] = "";
            guardar();
            evento("formalizado", {{"firma", firma}});
            say(nullptr, "Formalización aceptada:\n" + firma);
            return;
        } catch (const ErrorValor& e) {
            error = e.what();
        } catch (const json::exception& e) {
            error = std::string("respuesta con formato inesperado: ") + e.what();
        }
        estado_["ultimo_error"] = recortar(error, 800);
        guardar();
        evento("formalizacion_fallo", {{"error", recortar(error, 400)}});
        say(nullptr, "formalización " + std::to_string(i) + "/" + std::to_string(cfg_.formalizar) + " rechazada: " + recortar(ultima_linea(error), 200));
    }
    throw std::runtime_error("No se pudo formalizar el problema: " + recortar(error, 300));
}

std::pair<std::string, std::string> Motor::validar_formalizacion(const json& datos) {
    if (auto* lean = dynamic_cast<LeanVerificador*>(&ver_)) {
        std::string defs = datos.value("definiciones", std::string());
        {
            std::vector<std::string> keep;
            for (auto& l : split_lines(defs)) if (!starts_with(trim(l), "import ")) keep.push_back(l);
            defs = join(keep, "\n");
        }
        std::string firma = lean->extraer_firma(datos.contains("teorema") && datos["teorema"].is_string() ? datos["teorema"].get<std::string>() : "");
        if (firma.empty()) throw ErrorValor("No se obtuvo una firma `theorem ... := by` válida.");
        if (auto malo = lean->prohibido(defs + "\n" + firma)) throw ErrorValor("Construcción prohibida en la formalización: " + *malo);
        if (auto triv = lean->enunciado_trivial(firma)) throw ErrorValor(*triv);
        estado_["definiciones"] = trim(defs);
        auto [ok, salida] = lean->comprobar(lean->codigo(estado_, {}, firma, "  sorry"), "", true);
        if (!ok) throw ErrorValor(lean->resumir_error(salida));
        return {firma, firma};
    }
    std::string pre = datos.value("preambulo", std::string());
    std::vector<std::string> crit, vars;
    if (datos.contains("criterios") && datos["criterios"].is_array()) for (auto& c : datos["criterios"]) { std::string s = c.is_string() ? c.get<std::string>() : jd(c); if (!trim(s).empty()) crit.push_back(s); }
    if (datos.contains("variables") && datos["variables"].is_array()) for (auto& v : datos["variables"]) vars.push_back(v.is_string() ? v.get<std::string>() : jd(v));
    std::string desc = trim(datos.value("descripcion", std::string()));
    if (crit.empty() || vars.empty() || desc.empty()) throw ErrorValor("Faltan 'descripcion', 'variables' o 'criterios'.");
    auto malo = ver_.prohibido(pre);
    for (auto& c : crit) if (!malo) malo = ver_.prohibido(c);
    if (malo) throw ErrorValor("Construcción prohibida en la especificación: " + *malo);
    // la sintaxis la valida la propia ejecución del preámbulo y, para cada criterio, una comprobación con Python
    {
        std::string comprobacion = "import ast,sys\n";
        for (auto& c : crit) comprobacion += "ast.parse(" + jd(json(c)) + ", mode='eval')\n";
        comprobacion += "ast.parse(" + jd(json(pre)) + ", mode='exec')\n";
        auto [ok, salida] = ver_.comprobar(comprobacion, "");
        if (!ok) throw ErrorValor("Error de sintaxis en la especificación: " + recortar(ver_.resumir_error(salida), 300));
    }
    auto [ok, salida] = ver_.comprobar(ensamblar({PRELUDIO_PY, pre, "print(\"TODO_VERIFICADO\")"}), "x");
    if (!ok) throw ErrorValor("El preámbulo no se ejecuta: " + ver_.resumir_error(salida));
    estado_["preambulo"] = trim(pre);
    estado_["variables"] = vars;
    estado_["criterios"] = crit;
    return {desc, desc};
}

void Motor::fidelidad(const std::string& problema, const std::string& firma) {
    std::string prompt;
    if (dynamic_cast<LeanVerificador*>(&ver_)) prompt = ver_.prompt_fidelidad(problema, estado_.value("definiciones", std::string()), firma);
    else {
        std::string crit;
        for (auto& c : estado_["criterios"]) crit += c.get<std::string>() + "\n";
        prompt = ver_.prompt_fidelidad(problema, "PREÁMBULO:\n" + estado_.value("preambulo", std::string()) + "\nCRITERIOS:\n" + trim(crit), firma);
    }
    auto obj = extraer_json(llm_(prompt, 0.0, cfg_.semilla, true, 300));
    if (obj && obj->contains("equivalente") && (*obj)["equivalente"].is_boolean() && !(*obj)["equivalente"].get<bool>())
        throw ErrorValor("Fidelidad: el enunciado formal no coincide con el problema (" + recortar(obj->value("motivo", std::string()), 200) + ")");
}

// ---------------------------------------------------------------- árbol
json Motor::nodo(const std::string& firma, int prof) {
    int c = estado_["contador"].get<int>() + 1;
    estado_["contador"] = c;
    return {{"id", "n" + std::to_string(c)}, {"firma", firma}, {"prof", prof}, {"estado", "pendiente"}, {"cuerpo", ""}, {"auto", false}, {"directos", 0},
            {"finales", 0}, {"replanes", 0}, {"necesita_plan", true}, {"hijos", json::array()}, {"fallidos", json::array()}, {"errores", json::array()},
            {"ultimo_codigo", ""}, {"ultimo_error", ""}, {"huellas", json::array()}, {"cuerpos", json::array()}, {"repetidas", 0}};
}

std::string Motor::bloque_nodo(const json& n) {
    std::vector<std::string> partes;
    for (auto& h : n["hijos"]) if (h["estado"] == "probado") partes.push_back(bloque_nodo(h));
    partes.push_back(ver_.bloque(n["firma"], n["cuerpo"]));
    return ensamblar(partes);
}

bool Motor::fallo(json& n, const std::string& cuerpo, const std::string& salida, const std::string& tipo) {
    std::string resumen = ver_.resumir_error(salida);
    if (contains(resumen, "repetida") && !n.value("ultimo_error", std::string()).empty()) {   // conservar el error REAL
        std::string previo = n["ultimo_error"];
        if (previo.rfind("(Repetiste", 0) == 0) { auto p = previo.find('\n'); if (p != std::string::npos) previo = previo.substr(p + 1); }
        n["ultimo_codigo"] = cuerpo;
        n["ultimo_error"] = "(Repetiste una respuesta idéntica a un intento anterior: CAMBIA de enfoque.)\n" + previo;
    } else {
        n["ultimo_codigo"] = cuerpo;
        n["ultimo_error"] = resumen;
    }
    n["errores"].push_back(resumen);
    while (n["errores"].size() > 6) n["errores"].erase(n["errores"].begin());
    if (!cuerpo.empty() && !contains(resumen, "repetida")) {
        n["cuerpos"].push_back(recortar(cuerpo, 500));
        while (n["cuerpos"].size() > 4) n["cuerpos"].erase(n["cuerpos"].begin());
    }
    guardar();
    evento("intento_fallido", {{"nodo", n["id"]}, {"tipo", tipo}, {"error", recortar(resumen, 400)}});
    say(&n, tipo + " " + std::to_string(tipo == "directo" ? n["directos"].get<int>() : n["finales"].get<int>()) + ": FALLA — " + recortar(ver_.titulo_error(resumen), 130));
    return false;
}

bool Motor::aceptar(json& n, const std::string& cuerpo, const std::string& tipo) {
    n["cuerpo"] = cuerpo;
    n["estado"] = "probado";
    n["ultimo_error"] = "";
    guardar();
    evento("probado", {{"nodo", n["id"]}, {"tipo", tipo}, {"firma", recortar(n["firma"], 200)}});
    say(&n, tipo + ": VERIFICADO — " + recortar(primera_linea(n["firma"]), 110));
    return true;
}

bool Motor::intento(json& n, const std::vector<std::string>& ctx, const std::string& tipo) {
    presupuesto();
    const char* clave = tipo == "directo" ? "directos" : "finales";
    n[clave] = n[clave].get<int>() + 1;
    int idx = n["directos"].get<int>() + n["finales"].get<int>() + 6 * n["replanes"].get<int>();
    double temp = std::min(1.0, TEMPERATURAS[std::min(idx - 1, 5)] + 0.15 * n.value("repetidas", 0));   // si repite, más variedad
    long long idn = std::stoll(n["id"].get<std::string>().substr(1));
    int seed = (int)(cfg_.semilla * 1000003LL + idn * 1009 + n["directos"].get<int>() * 31 + n["finales"].get<int>() * 17 + n["replanes"].get<int>() * 5);
    std::string resp = llm_(ver_.prompt_prueba(n["firma"], ctx, estado_, n, tipo), temp, seed);
    std::string cuerpo = ver_.extraer_cuerpo(resp);
    if (cuerpo.empty()) return fallo(n, "", "No se pudo extraer una demostración de la respuesta.", tipo);
    if (auto malo = ver_.prohibido(cuerpo)) return fallo(n, cuerpo, "Construcción prohibida: " + *malo, tipo);
    std::string h = huella(cuerpo);
    for (auto& x : n["huellas"]) if (x == h) { n["repetidas"] = n.value("repetidas", 0) + 1; return fallo(n, cuerpo, "Demostración repetida: ya se probó y falló.", tipo); }
    n["huellas"].push_back(h);
    while (n["huellas"].size() > 200) n["huellas"].erase(n["huellas"].begin());
    auto [ok, salida] = ver_.comprobar(ver_.codigo(estado_, ctx, n["firma"], cuerpo), ver_.nombre == "sympy" ? "x" : "");
    return ok ? aceptar(n, cuerpo, tipo) : fallo(n, cuerpo, salida, tipo);
}

std::vector<json> Motor::planificar(json& n, const std::vector<std::string>& ctx) {
    auto* lean = dynamic_cast<LeanVerificador*>(&ver_);
    std::vector<json> vacio;
    if (!lean) return vacio;
    std::set<std::string> existentes;
    for (auto& h : n["hijos"]) existentes.insert(lean->clave(h["firma"]));
    std::vector<std::string> fallidos;
    for (auto& f : n["fallidos"]) { existentes.insert(lean->clave(f)); fallidos.push_back(f); }
    existentes.insert(lean->clave(n["firma"]));
    std::vector<std::string> probados = ctx;
    for (auto& h : n["hijos"]) if (h["estado"] == "probado") probados.push_back(bloque_nodo(h));
    std::string error;
    long long idn = std::stoll(n["id"].get<std::string>().substr(1));
    for (int k = 1; k <= cfg_.planes; ++k) {
        int seed = (int)(cfg_.semilla * 2000003LL + idn * 1013 + n["replanes"].get<int>() * 101 + k);
        try {
            auto obj = extraer_json(llm_(lean->prompt_plan(n["firma"], probados, estado_, n, cfg_.max_lemas, fallidos), 0.25 + 0.05 * (k + n["replanes"].get<int>()), seed, true, 2000));
            if (!obj) throw ErrorValor("el modelo no devolvió JSON");
            if (!obj->contains("lemas") || !(*obj)["lemas"].is_array()) throw ErrorValor("'lemas' debe ser una lista");
            std::vector<std::string> firmas;
            std::set<std::string> claves = existentes;
            int visto = 0;
            for (auto& raw : (*obj)["lemas"]) {
                if (visto++ >= cfg_.max_lemas) break;
                std::string f = lean->extraer_firma(raw.is_string() ? raw.get<std::string>() : jd(raw));
                if (f.empty() || lean->prohibido(f) || lean->enunciado_trivial(f) || claves.count(lean->clave(f))) continue;
                claves.insert(lean->clave(f));
                int c = estado_["contador"].get<int>() + 1;
                estado_["contador"] = c;
                std::string slug;
                for (char ch : lean->nombre_de(f)) slug.push_back(std::isalnum((unsigned char)ch) || ch == '_' ? ch : '_');
                if (slug.size() > 30) slug.resize(30);
                if (slug.empty()) slug = "lema";
                firmas.push_back(lean->renombrar(f, "l" + std::to_string(c) + "_" + slug));
            }
            if (firmas.empty()) throw ErrorValor("no se obtuvo ningún lema útil y nuevo");
            std::vector<std::string> partes = {lean->codigo(estado_, probados)};
            for (auto& f : firmas) partes.push_back(f + "\n  sorry");
            partes.push_back(n["firma"].get<std::string>() + "\n  sorry");
            auto [ok, salida] = lean->comprobar(ensamblar(partes), "", true);
            if (!ok) throw ErrorValor(lean->resumir_error(salida));
            evento("plan", {{"nodo", n["id"]}, {"lemas", firmas}});
            std::string resumen;
            for (size_t i = 0; i < firmas.size(); ++i) resumen += (i ? "; " : "") + recortar(primera_linea(firmas[i]), 60);
            say(&n, "plan con " + std::to_string(firmas.size()) + " lemas: " + resumen);
            std::vector<json> nuevos;
            for (auto& f : firmas) nuevos.push_back(nodo(f, n["prof"].get<int>() + 1));
            return nuevos;
        } catch (const ErrorValor& e) {
            error = e.what();
        } catch (const json::exception& e) {
            error = std::string("respuesta con formato inesperado: ") + e.what();
        }
        evento("plan_fallo", {{"nodo", n["id"]}, {"error", recortar(error, 300)}});
        say(&n, "plan " + std::to_string(k) + "/" + std::to_string(cfg_.planes) + " rechazado: " + recortar(primera_linea(error), 120));
    }
    return vacio;
}

bool Motor::resolver_nodo(json& n, const std::vector<std::string>& ctx) {
    if (n["estado"] == "probado") return true;
    if (!n["auto"].get<bool>()) {
        presupuesto();
        auto cuerpo = ver_.auto_(estado_, n["firma"], ctx);
        n["auto"] = true;
        guardar();
        if (cuerpo) return aceptar(n, *cuerpo, "auto");
    }
    while (n["directos"].get<int>() < cfg_.directos) if (intento(n, ctx, "directo")) return true;
    if (!ver_.admite_plan || n["prof"].get<int>() >= cfg_.profundidad) return fallar(n);
    while (n["replanes"].get<int>() <= cfg_.replanes) {
        if (n["necesita_plan"].get<bool>()) {
            if (n["replanes"].get<int>() >= cfg_.replanes) break;
            n["replanes"] = n["replanes"].get<int>() + 1;
            auto nuevos = planificar(n, ctx);
            for (auto& h : nuevos) n["hijos"].push_back(h);
            n["necesita_plan"] = nuevos.empty();
            n["finales"] = 0;
            guardar();
            if (nuevos.empty()) continue;
        }
        bool hubo_fallo = false;
        for (size_t i = 0; i < n["hijos"].size(); ++i) {
            if (n["hijos"][i]["estado"] == "probado") continue;
            std::vector<std::string> ctx_h = ctx;
            for (size_t j = 0; j < i; ++j) if (n["hijos"][j]["estado"] == "probado") ctx_h.push_back(bloque_nodo(n["hijos"][j]));
            if (!resolver_nodo(n["hijos"][i], ctx_h)) {
                n["fallidos"].push_back(n["hijos"][i]["firma"]);
                json conservados = json::array();
                for (auto& x : n["hijos"]) if (x["estado"] == "probado") conservados.push_back(x);   // se conservan los lemas ya demostrados
                n["hijos"] = conservados;
                n["necesita_plan"] = true;
                hubo_fallo = true;
                guardar();
                break;
            }
        }
        if (hubo_fallo) continue;
        std::vector<std::string> ctx_f = ctx;
        for (auto& x : n["hijos"]) ctx_f.push_back(bloque_nodo(x));
        while (n["finales"].get<int>() < cfg_.finales) if (intento(n, ctx_f, "final")) return true;
        n["necesita_plan"] = true;   // los lemas no bastaron: pedir más
        guardar();
    }
    return fallar(n);
}

bool Motor::fallar(json& n) {
    n["estado"] = "fallido";
    guardar();
    evento("fallido", {{"nodo", n["id"]}, {"firma", recortar(n["firma"], 200)}});
    say(&n, "no se pudo demostrar: " + recortar(primera_linea(n["firma"]), 100));
    return false;
}

// ---------------------------------------------------------------- cierre
bool Motor::finalizar() {
    json& raiz = estado_["raiz"];
    std::string codigo = ver_.codigo(estado_, {bloque_nodo(raiz)});
    std::string nombre = "x";
    if (auto* lean = dynamic_cast<LeanVerificador*>(&ver_)) nombre = lean->nombre_de(raiz["firma"]);
    auto [ok, salida] = ver_.comprobar(codigo, nombre);
    if (!ok) {
        estado_["ultimo_error"] = recortar(salida, 800);
        say(nullptr, "La verificación final FALLÓ: " + recortar(salida, 300));
        raiz["estado"] = "pendiente";   // se volverá a intentar en la próxima ejecución
        raiz["directos"] = 0;
        raiz["finales"] = 0;
        guardar();
        return false;
    }
    fs::path archivo = dir_ / ("resultado." + ver_.extension);
    escribir_atomico(archivo, codigo + "\n");
    estado_["resultado"] = {{"codigo", codigo}, {"archivo", archivo.string()}};
    estado_["estado"] = "verificado";
    guardar();
    evento("verificado", {{"archivo", archivo.string()}});
    return true;
}

int Motor::ejecutar(const json& problema, bool reiniciar) {
    try {
        say(nullptr, "Verificador: " + ver_.entorno() + " | modelo " + cfg_.modelo);
    } catch (const std::exception& e) {
        log_(std::string("ERROR: ") + e.what());
        return 2;
    }
    cargar(problema, reiniciar);
    t_inicio_ = std::chrono::steady_clock::now();
    try {
        if (estado_["estado"] == "verificado") {
            say(nullptr, "Este problema ya estaba verificado: " + estado_["resultado"]["archivo"].get<std::string>());
            return 0;
        }
        formalizar();
        if (!resolver_nodo(estado_["raiz"], {})) {
            estado_["estado"] = "fallido";
            guardar();
            say(nullptr, "No se encontró una demostración con los límites actuales. Sube --directos/--replanes/--profundidad y vuelve a ejecutar (con --reiniciar si quieres empezar de cero).");
            return 1;
        }
        if (!finalizar()) return 1;
        say(nullptr, "PROBLEMA RESUELTO Y VERIFICADO: " + estado_["resultado"]["archivo"].get<std::string>());
        return 0;
    } catch (const Pausa& p) {
        estado_["estado"] = "pausado";
        guardar();
        if (bandera_parar()) { say(nullptr, "INTERRUMPIDO. El progreso está guardado: ejecuta el mismo comando para continuar."); return 130; }
        say(nullptr, std::string("PAUSADO (") + p.what() + "). El progreso está guardado: ejecuta el mismo comando para continuar.");
        return 3;
    } catch (const ErrorModelo& e) {
        estado_["estado"] = "pausado";
        guardar();
        say(nullptr, std::string("ERROR DEL MODELO: ") + e.what() + "\nEl progreso está guardado: arranca Ollama y vuelve a ejecutar el mismo comando.");
        return 4;
    } catch (const std::exception& e) {
        estado_["estado"] = "fallido";
        estado_["error_fatal"] = e.what();   // ultimo_error conserva el error bruto del verificador
        guardar();
        say(nullptr, std::string("ERROR: ") + e.what());
        return 1;
    }
}

// ---------------------------------------------------------------- informe
std::vector<std::string> arbol(const json& n, int nivel) {
    std::string e = n["estado"];
    std::string marca = e == "probado" ? "✔" : e == "fallido" ? "✘" : e == "pendiente" ? "…" : "?";
    std::vector<std::string> out = {std::string(nivel * 4, ' ') + marca + " " + n["id"].get<std::string>() + " [" + e + "] directos=" + std::to_string(n["directos"].get<int>()) +
                                    " finales=" + std::to_string(n["finales"].get<int>()) + " replanes=" + std::to_string(n["replanes"].get<int>()) + "  " + recortar(primera_linea(n["firma"]), 90)};
    for (auto& h : n["hijos"]) for (auto& l : arbol(h, nivel + 1)) out.push_back(l);
    return out;
}

std::string informe(const fs::path& directorio) {
    fs::path ruta = directorio / "estado.json";
    if (!fs::exists(ruta)) return "No hay estado en " + directorio.string();
    json e = json::parse(leer_archivo(ruta), nullptr, false);
    if (e.is_discarded()) return "El estado de " + directorio.string() + " no se puede leer";
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", e.value("tiempo", 0.0) / 3600);
    std::vector<std::string> out = {"Problema: " + recortar(trim(e["problema"]["enunciado"].get<std::string>()), 150),
                                    "Estado: " + e["estado"].get<std::string>() + " | backend " + e["problema"].value("backend", std::string()) + " | llamadas " + std::to_string(e.value("llamadas", 0)) + " | tiempo " + b + " h"};
    if (e.contains("raiz") && e["raiz"].is_object()) { out.push_back(""); for (auto& l : arbol(e["raiz"])) out.push_back(l); }
    if (!e.value("error_fatal", std::string()).empty()) { out.push_back(""); out.push_back("Error: " + recortar(e["error_fatal"].get<std::string>(), 300)); }
    else if (!e.value("ultimo_error", std::string()).empty()) { out.push_back(""); out.push_back("Último error: " + recortar(e["ultimo_error"].get<std::string>(), 300)); }
    if (e["estado"] == "verificado") { out.push_back(""); out.push_back("Resultado: " + e["resultado"]["archivo"].get<std::string>()); }
    return join(out, "\n");
}

}  // namespace demo
