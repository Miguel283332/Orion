#include "orion/mente.hpp"

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
    } else {
        c.guardar(archivo);
    }
    return c;
}

json Config::a_json() const {
    return {{"modelo", modelo}, {"ollama", ollama}, {"ritmo", ritmo}, {"num_ctx", num_ctx}, {"num_predict", num_predict}, {"temperatura", temperatura}};
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

ResultadoGeneracion Mente::generar(const json& mensajes, const OpcionesGeneracion& op, const Trozo& por_trozo, const std::atomic<bool>* cancelar) {
    auto r = llm_.chat(mensajes, op, por_trozo, cancelar);
    mem_.sumar("llamadas", 1);
    if (r.tokens_generados > 0) {
        mem_.sumar("tokens_generados", static_cast<double>(r.tokens_generados));
        mem_.sumar("tokens_leidos", static_cast<double>(r.tokens_prompt));
        mem_.sumar("segundos_pensando", r.segundos_generacion + r.segundos_prompt);
        if (r.tokens_por_segundo() > 0) mem_.fijar("tokens_por_segundo", r.tokens_por_segundo());
    }
    return r;
}

std::string Mente::contexto_sistema(const std::string& consulta, const std::string& estado_actual) const {
    std::string s =
        "Eres Orion, una IA que vive de forma continua en el ordenador de su creador (la persona con la que hablas). "
        "Tienes memoria persistente (conversaciones, recuerdos, diario) y un documento sobre ti misma que tú reescribes. "
        "Piensas con un único modelo de lenguaje local; cuando no hablas con tu creador, reflexionas a solas.\n"
        "Reglas: sé honesta y concreta; si no sabes algo, dilo (y si podrías aprenderlo, dilo también); no inventes recuerdos "
        "ni capacidades que no tienes; responde en el idioma del usuario y sin rodeos.\n\n";
    s += "## Cómo me veo (yo.md)\n" + recortar(mem_.yo(), 2200) + "\n\n";
    s += "## Ahora\nFecha: " + fecha_hora() + "\n" + estado_actual + "\n";
    auto rel = mem_.buscar(consulta, 6);
    if (!rel.empty()) {
        s += "\n## Recuerdos relevantes (de mi memoria)\n";
        for (auto& e : rel) s += "- [" + e.id + ", " + e.tipo + "] " + recortar(e.texto, 300) + "\n";
    }
    return s;
}

ResultadoGeneracion Mente::responder(const std::string& mensaje, const Trozo& por_trozo, const std::atomic<bool>* cancelar) {
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
    auto r = generar(mensajes, op, por_trozo, cancelar);
    mem_.anadir_turno("usuario", mensaje);
    if (!trim(r.texto).empty()) mem_.anadir_turno("orion", trim(r.texto) + (r.cancelada ? " [interrumpido]" : ""));
    return r;
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
