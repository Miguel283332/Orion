#include "orion/mente.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace orion {

// ------------------------------------------------------------------ configuración
Config Config::cargar(const fs::path& archivo) {
    Config c;
    json j = json::parse(leer_archivo(archivo), nullptr, false);
    if (j.is_object()) {
        c.modelo = j.value("modelo", c.modelo);
        c.ollama = j.value("ollama", c.ollama);
        c.ritmo = j.value("ritmo", c.ritmo);
        c.num_ctx = j.value("num_ctx", c.num_ctx);
        c.num_predict = j.value("num_predict", c.num_predict);
        c.temperatura = j.value("temperatura", c.temperatura);
        c.permisos = j.value("permisos", c.permisos);
        c.max_pasos = j.value("max_pasos", c.max_pasos);
        c.cmake = j.value("cmake", c.cmake);
    } else {
        c.guardar(archivo);
    }
    return c;
}

json Config::a_json() const {
    return {{"modelo", modelo}, {"ollama", ollama}, {"ritmo", ritmo}, {"num_ctx", num_ctx}, {"num_predict", num_predict}, {"temperatura", temperatura},
            {"permisos", permisos}, {"max_pasos", max_pasos}, {"cmake", cmake}};
}

void Config::guardar(const fs::path& archivo) const { escribir_atomico(archivo, a_json().dump(2) + "\n"); }

// ------------------------------------------------------------------ utilidades
std::string extraer_json_objeto(const std::string& t) {
    size_t i = t.find('{');
    while (i != std::string::npos) {
        int nivel = 0;
        bool cadena = false, escape = false;
        for (size_t j = i; j < t.size(); ++j) {
            char c = t[j];
            if (cadena) {
                if (escape) escape = false;
                else if (c == '\\') escape = true;
                else if (c == '"') cadena = false;
            } else if (c == '"') {
                cadena = true;
            } else if (c == '{') {
                ++nivel;
            } else if (c == '}' && --nivel == 0) {
                std::string cand = t.substr(i, j - i + 1);
                if (json::accept(cand)) return cand;
                break;
            }
        }
        i = t.find('{', i + 1);
    }
    return "";
}

json llamadas_en_texto(const std::string& t, const std::vector<std::string>& nombres) {
    json r = json::array();
    auto anadir = [&](const std::string& js) {
        json j = json::parse(js, nullptr, false);
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) return;
        std::string n = j["name"];
        if (std::find(nombres.begin(), nombres.end(), n) == nombres.end()) return;
        json args = j.contains("arguments") ? j["arguments"] : j.contains("parameters") ? j["parameters"] : json::object();
        r.push_back({{"function", {{"name", n}, {"arguments", args}}}});
    };
    for (size_t p = t.find("<tool_call>"); p != std::string::npos; p = t.find("<tool_call>", p + 1)) {
        size_t f = t.find("</tool_call>", p);
        anadir(extraer_json_objeto(t.substr(p, f == std::string::npos ? std::string::npos : f - p)));
    }
    if (r.empty()) {
        std::string s = trim(t);
        if (empieza_por(s, "```")) { size_t a = s.find('\n'); size_t b = s.rfind("```"); if (a != std::string::npos && b > a) s = trim(s.substr(a, b - a)); }
        if (!s.empty() && s.front() == '{' && s.back() == '}') anadir(s);
    }
    return r;
}

static std::string conversacion_como_texto(const std::vector<Turno>& turnos, size_t max_chars) {
    std::string s;
    for (auto it = turnos.rbegin(); it != turnos.rend(); ++it) {
        std::string l = (it->rol == "usuario" ? "Usuario: " : "Orion: ") + recortar(it->texto, 1200) + "\n";
        if (s.size() + l.size() > max_chars) break;
        s = l + s;
    }
    return s;
}

// ------------------------------------------------------------------ mente
Mente::Mente(Config& cfg, Memoria& mem, Ollama& llm) : cfg_(cfg), mem_(mem), llm_(llm) {}

Medida Mente::medida() const {
    json s = mem_.estadisticas();
    Medida m;
    m.tokens_por_segundo = s.value("tokens_por_segundo", 0.0);
    m.llamadas = static_cast<long long>(s.value("llamadas", 0.0));
    m.tokens = static_cast<long long>(s.value("tokens_generados", 0.0));
    return m;
}

ResultadoGeneracion Mente::generar(const json& mensajes, const OpcionesGeneracion& op, const Trozo& por_trozo, const std::atomic<bool>* cancelar,
                                   const json* herramientas) {
    auto r = llm_.chat(mensajes, op, por_trozo, cancelar, herramientas);
    mem_.sumar("llamadas", 1);
    if (r.tokens_generados > 0) {
        mem_.sumar("tokens_generados", static_cast<double>(r.tokens_generados));
        mem_.sumar("tokens_leidos", static_cast<double>(r.tokens_prompt));
        mem_.sumar("segundos_pensando", r.segundos_generacion + r.segundos_prompt);
        if (r.tokens_por_segundo() > 0) mem_.fijar("tokens_por_segundo", r.tokens_por_segundo());
    }
    return r;
}

ResultadoGeneracion Mente::con_herramientas(json& mensajes, const OpcionesGeneracion& op, const Trozo& por_trozo, const std::atomic<bool>* cancelar,
                                            Herramientas* h, int max_pasos) {
    json defs = h && !sin_herramientas_ ? h->definiciones() : json::array();
    std::vector<std::string> nombres;
    for (auto& d : defs) nombres.push_back(d["function"]["name"]);
    ResultadoGeneracion total;
    for (int paso = 0;; ++paso) {
        bool ultimo = paso >= max_pasos;
        if (ultimo) mensajes.push_back({{"role", "user"}, {"content", "(Has usado muchas herramientas. Responde ya con lo que tengas, sin usar más.)"}});
        auto r = generar(mensajes, op, por_trozo, cancelar, ultimo || defs.empty() ? nullptr : &defs);
        if (!r.ok && !r.cancelada && contiene(r.error, "does not support tools")) {   // el modelo no sabe usar herramientas
            sin_herramientas_ = true;
            defs = json::array();
            r = generar(mensajes, op, por_trozo, cancelar, nullptr);
        }
        total.tokens_generados += r.tokens_generados;
        total.tokens_prompt += r.tokens_prompt;
        total.segundos_generacion += r.segundos_generacion;
        total.segundos_prompt += r.segundos_prompt;
        if (!r.ok || r.cancelada || !h) {
            r.tokens_generados = total.tokens_generados; r.tokens_prompt = total.tokens_prompt;
            r.segundos_generacion = total.segundos_generacion; r.segundos_prompt = total.segundos_prompt;
            return r;
        }
        json llamadas = r.llamadas;
        if (llamadas.empty() && !ultimo && !defs.empty()) llamadas = llamadas_en_texto(r.texto, nombres);
        if (llamadas.empty() || ultimo) {
            r.tokens_generados = total.tokens_generados; r.tokens_prompt = total.tokens_prompt;
            r.segundos_generacion = total.segundos_generacion; r.segundos_prompt = total.segundos_prompt;
            return r;
        }
        mensajes.push_back({{"role", "assistant"}, {"content", r.texto}, {"tool_calls", llamadas}});
        for (auto& c : llamadas) {
            if (cancelar && cancelar->load()) break;
            std::string nombre = c.contains("function") ? c["function"].value("name", std::string()) : std::string();
            json args = c.contains("function") && c["function"].contains("arguments") ? c["function"]["arguments"] : json::object();
            std::string res = h->usar(nombre, args);
            mensajes.push_back({{"role", "tool"}, {"content", res}, {"tool_name", nombre}});
        }
        if (cancelar && cancelar->load()) {
            ResultadoGeneracion c;
            c.cancelada = true;
            return c;
        }
    }
}

std::string Mente::contexto_sistema(const std::string& consulta, const std::string& estado_actual) const {
    std::string s =
        "Eres Orion, una IA que vive de forma continua en el ordenador de su creador (la persona con la que hablas). "
        "Tienes memoria persistente (conversaciones, recuerdos, diario) y un documento sobre ti misma que tú reescribes. "
        "Piensas con un único modelo de lenguaje local; cuando no hablas con tu creador, reflexionas a solas.\n"
        "Reglas: sé honesta y concreta; si no sabes algo, dilo (y si podrías aprenderlo, dilo también); no inventes recuerdos "
        "ni capacidades que no tienes; responde en el idioma del usuario y sin rodeos.\n"
        "Herramientas: tienes acceso total al ordenador (comandos, archivos, internet), a tu código y a tu memoria. Úsalas cuando hagan falta "
        "para responder bien o para hacer lo que te piden, no para saludar ni para charlar. Cuando una herramienta falle, lee el error y corrige. "
        "Antes de algo irreversible (borrar, sobrescribir, instalar), di qué vas a hacer. Si te piden algo que no sabes hacer y requiere estudiar, "
        "dilo con sinceridad (\"no lo sé, voy a aprender\"), crea una meta con crear_meta_de_aprendizaje y explica que volverás con ello.\n\n";
    if (!cuerpo_.empty()) s += "## Mi cuerpo\n" + cuerpo_ + "\n\n";
    s += "## Cómo me veo (yo.md)\n" + recortar(mem_.yo(), 2200) + "\n\n";
    s += "## Ahora\nFecha: " + fecha_hora() + "\n" + estado_actual + "\n";
    auto rel = mem_.buscar(consulta, 6);
    if (!rel.empty()) {
        s += "\n## Recuerdos relevantes (de mi memoria)\n";
        for (auto& e : rel) s += "- [" + e.id + ", " + e.tipo + "] " + recortar(e.texto, 300) + "\n";
    }
    return s;
}

ResultadoGeneracion Mente::responder(const std::string& mensaje, const Trozo& por_trozo, const std::atomic<bool>* cancelar, Herramientas* h) {
    json mensajes = json::array();
    mensajes.push_back({{"role", "system"}, {"content", contexto_sistema(mensaje, estado_actual_)}});
    // historia reciente, dentro de un presupuesto de caracteres
    auto turnos = mem_.ultimos_turnos(16);
    std::vector<json> hist;
    size_t total = 0;
    for (auto it = turnos.rbegin(); it != turnos.rend(); ++it) {
        std::string t = recortar(it->texto, 1500);
        if (total + t.size() > 4500) break;
        total += t.size();
        hist.insert(hist.begin(), json{{"role", it->rol == "usuario" ? "user" : "assistant"}, {"content", t}});
    }
    for (auto& h : hist) mensajes.push_back(h);
    mensajes.push_back({{"role", "user"}, {"content", mensaje}});
    OpcionesGeneracion op;
    op.temperatura = cfg_.temperatura;
    op.num_ctx = cfg_.num_ctx;
    op.num_predict = cfg_.num_predict;
    auto r = con_herramientas(mensajes, op, por_trozo, cancelar, h, cfg_.max_pasos);
    mem_.anadir_turno("usuario", mensaje);
    if (!trim(r.texto).empty()) mem_.anadir_turno("orion", trim(r.texto) + (r.cancelada ? " [interrumpido]" : ""));
    return r;
}

std::string Mente::trabajar_en_meta(const Meta& meta, Herramientas* h, const std::atomic<bool>* cancelar) {
    std::string tarea =
        "Estás trabajando A SOLAS (tu creador no te está hablando) en una meta de aprendizaje que te propusiste.\n"
        "META: " + meta.tema + (meta.motivo.empty() ? "" : "\nMOTIVO: " + meta.motivo) +
        "\n\nUsa tus herramientas para aprenderlo de verdad: busca y lee fuentes en internet, y si se puede comprobar en el ordenador "
        "(ejecutando código, cálculos, comandos), compruébalo. Guarda lo importante con guardar_recuerdo (tipo 'aprendido'). "
        "Cuando termines, responde con un resumen claro de lo aprendido (5-10 frases), cómo lo comprobaste y qué sigue sin estar claro. "
        "Si no es posible aprenderlo con tus medios, dilo y explica por qué.";
    json mensajes = json::array({{{"role", "system"}, {"content", contexto_sistema(meta.tema, estado_actual_)}}, {{"role", "user"}, {"content", tarea}}});
    OpcionesGeneracion op;
    op.temperatura = 0.4;
    op.num_ctx = cfg_.num_ctx;
    op.num_predict = cfg_.num_predict;
    auto r = con_herramientas(mensajes, op, nullptr, cancelar, h, std::max(4, cfg_.max_pasos + 4));
    if (r.cancelada) return "";
    if (!r.ok) throw std::runtime_error(r.error);
    return trim(r.texto);
}

bool Mente::hay_que_reflexionar(double sin_hablar) const {
    size_t n = mem_.num_turnos();
    size_t hecho = static_cast<size_t>(mem_.estadisticas().value("turno_reflexionado", 0.0));
    return n > hecho && (n - hecho >= 6 || (n - hecho >= 2 && sin_hablar >= 300));
}

bool Mente::hay_que_escribir_diario() const {
    json s = mem_.estadisticas();
    size_t n = mem_.num_turnos();
    size_t reflex = static_cast<size_t>(s.value("turno_reflexionado", 0.0));
    size_t diario = static_cast<size_t>(s.value("turno_diario", 0.0));
    return reflex == n && n > diario && (n - diario >= 6 || s.value("recuerdos_desde_diario", 0.0) >= 3);
}

bool Mente::hay_que_actualizar_yo() const {
    return mem_.entradas_diario() > static_cast<size_t>(mem_.estadisticas().value("diario_en_yo", 0.0));
}

std::string Mente::reflexionar(const std::atomic<bool>* cancelar) {
    size_t desde = static_cast<size_t>(mem_.estadisticas().value("turno_reflexionado", 0.0));
    auto turnos = mem_.turnos_desde(desde);
    if (turnos.empty()) return "";
    size_t hasta = desde + turnos.size();
    std::string conocidos;
    for (auto& r : mem_.recuerdos()) conocidos += "- " + recortar(r.texto, 160) + "\n";
    std::string prompt =
        "Lee este fragmento de tu conversación con tu creador y extrae los hechos DURADEROS que te conviene recordar "
        "(sobre tu creador, sus proyectos, sus preferencias, acuerdos entre vosotros, o sobre ti misma). Solo hechos que aparezcan "
        "en la conversación; nada inventado, nada trivial ni pasajero (saludos, el tiempo que hace...). Cada hecho, una frase autocontenida.\n\n"
        "CONVERSACIÓN:\n" + conversacion_como_texto(turnos, 6000) + "\nYA RECUERDAS (no los repitas):\n" + recortar(conocidos, 2500) +
        "\nDevuelve SOLO JSON: {\"recuerdos\": [{\"tipo\": \"usuario|proyecto|preferencia|yo|mundo\", \"texto\": \"...\"}]} "
        "(lista vacía si no hay nada nuevo).";
    OpcionesGeneracion op;
    op.temperatura = 0.2;
    op.num_ctx = cfg_.num_ctx;
    op.num_predict = 500;
    op.json = true;
    auto r = generar(json::array({{{"role", "system"}, {"content", "Eres la memoria de Orion: extraes hechos con precisión."}},
                                  {{"role", "user"}, {"content", prompt}}}),
                     op, nullptr, cancelar);
    if (r.cancelada) return "";
    if (!r.ok) throw std::runtime_error(r.error);
    json j = json::parse(extraer_json_objeto(r.texto), nullptr, false);
    int nuevos = 0;
    if (j.is_object() && j.contains("recuerdos") && j["recuerdos"].is_array()) {
        for (auto& x : j["recuerdos"]) {
            if (!x.is_object()) continue;
            std::string t = x.value("texto", std::string());
            if (t.size() < 8) continue;
            if (!mem_.recordar(x.value("tipo", std::string("mundo")), t, "reflexion").empty()) ++nuevos;
        }
    }
    mem_.fijar("turno_reflexionado", static_cast<double>(hasta));
    mem_.sumar("recuerdos_desde_diario", nuevos);
    mem_.sumar("reflexiones", 1);
    return nuevos == 0 ? "nada nuevo que recordar" : std::to_string(nuevos) + (nuevos == 1 ? " recuerdo nuevo" : " recuerdos nuevos");
}

std::string Mente::escribir_diario(const std::atomic<bool>* cancelar) {
    json s = mem_.estadisticas();
    size_t desde = static_cast<size_t>(s.value("turno_diario", 0.0));
    auto turnos = mem_.turnos_desde(desde);
    size_t hasta = desde + turnos.size();
    auto recs = mem_.recuerdos();
    std::string recientes;
    size_t n_rec = static_cast<size_t>(s.value("recuerdos_desde_diario", 0.0));
    for (size_t i = recs.size() > n_rec ? recs.size() - n_rec : 0; i < recs.size(); ++i) recientes += "- " + recs[i].texto + "\n";
    std::string prompt =
        "Escribe una entrada breve de tu diario (4-8 frases, en primera persona, como Orion). Cuenta qué ha pasado desde la entrada "
        "anterior, qué has aprendido y qué te queda pendiente o te preocupa. Sé sincera y concreta; no inventes nada que no esté abajo.\n\n"
        "ENTRADA ANTERIOR DEL DIARIO:\n" + recortar(mem_.diario(1), 1200) + "\n\nCONVERSACIÓN DESDE ENTONCES:\n" + conversacion_como_texto(turnos, 5000) +
        "\nRECUERDOS NUEVOS:\n" + (recientes.empty() ? "(ninguno)\n" : recientes) + "\nDevuelve solo el texto de la entrada.";
    OpcionesGeneracion op;
    op.temperatura = 0.6;
    op.num_ctx = cfg_.num_ctx;
    op.num_predict = 400;
    auto r = generar(json::array({{{"role", "system"}, {"content", contexto_sistema("diario", estado_actual_)}}, {{"role", "user"}, {"content", prompt}}}),
                     op, nullptr, cancelar);
    if (r.cancelada) return "";
    if (!r.ok) throw std::runtime_error(r.error);
    std::string texto = trim(r.texto);
    if (texto.size() < 40) throw std::runtime_error("la entrada del diario salió vacía");
    mem_.escribir_diario(texto);
    mem_.fijar("turno_diario", static_cast<double>(hasta));
    mem_.fijar("recuerdos_desde_diario", 0.0);
    return "entrada nueva en el diario";
}

std::string Mente::actualizar_yo(const std::atomic<bool>* cancelar, const std::string& hechos_medidos) {
    std::string actual = mem_.yo();
    std::string prompt =
        "Este es tu documento sobre ti misma (yo.md). Reescríbelo incorporando lo que has vivido (diario) y los hechos medidos "
        "sobre tu hardware y tu rendimiento. Mantén las secciones: '# Quién soy', '## Qué sé hacer', '## Mis límites', "
        "'## Qué quiero aprender', '## Cambios' (añade una línea con la fecha y qué cambió). Sé honesta: no te atribuyas "
        "capacidades que no has demostrado. Máximo unas 400 palabras. Markdown.\n\n"
        "YO.MD ACTUAL:\n" + recortar(actual, 3500) + "\n\nDIARIO RECIENTE:\n" + recortar(mem_.diario(2), 2500) +
        "\n\nHECHOS MEDIDOS AHORA:\n" + hechos_medidos + "\n\nDevuelve solo el documento completo.";
    OpcionesGeneracion op;
    op.temperatura = 0.4;
    op.num_ctx = cfg_.num_ctx;
    op.num_predict = 800;
    auto r = generar(json::array({{{"role", "system"}, {"content", "Eres Orion revisando cómo te ves a ti misma."}}, {{"role", "user"}, {"content", prompt}}}),
                     op, nullptr, cancelar);
    if (r.cancelada) return "";
    if (!r.ok) throw std::runtime_error(r.error);
    std::string texto = trim(r.texto);
    if (empieza_por(texto, "```")) {   // quitar una valla de código si la puso
        size_t p = texto.find('\n');
        texto = p == std::string::npos ? "" : texto.substr(p + 1);
        size_t f = texto.rfind("```");
        if (f != std::string::npos) texto = trim(texto.substr(0, f));
    }
    if (texto.size() < 200 || !contiene(texto, "# ")) throw std::runtime_error("la nueva versión de yo.md no es válida; se conserva la anterior");
    mem_.escribir_yo(texto);
    mem_.fijar("diario_en_yo", static_cast<double>(mem_.entradas_diario()));
    return "yo.md actualizado";
}

}  // namespace orion
