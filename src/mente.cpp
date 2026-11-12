#include "orion/mente.hpp"

#include <algorithm>
#include <ctime>
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
        if (j.value("config_version", 1) < 2 && c.num_predict == 512) c.num_predict = 1024;   // 512 cortaba las respuestas con herramientas
        c.temperatura = j.value("temperatura", c.temperatura);
        c.permisos = j.value("permisos", c.permisos);
        c.max_pasos = j.value("max_pasos", c.max_pasos);
        c.cmake = j.value("cmake", c.cmake);
        c.practica_cada_min = j.value("practica_cada_min", c.practica_cada_min);
        c.automejora_cada_h = j.value("automejora_cada_h", c.automejora_cada_h);
        c.web_puerto = j.value("web_puerto", c.web_puerto);
        if (j.contains("dispositivos") && j["dispositivos"].is_array()) c.dispositivos = j["dispositivos"];
        c.dispositivo = j.value("dispositivo", c.dispositivo);
        c.dispositivo_auto = j.value("dispositivo_auto", c.dispositivo_auto);
        if (j.value("config_version", 1) < 2) c.guardar(archivo);   // actualizar el archivo al formato nuevo
    } else {
        c.guardar(archivo);
    }
    return c;
}

json Config::a_json() const {
    return {{"config_version", 2}, {"modelo", modelo}, {"ollama", ollama}, {"ritmo", ritmo}, {"num_ctx", num_ctx}, {"num_predict", num_predict}, {"temperatura", temperatura},
            {"permisos", permisos}, {"max_pasos", max_pasos}, {"cmake", cmake},
            {"practica_cada_min", practica_cada_min}, {"automejora_cada_h", automejora_cada_h}, {"web_puerto", web_puerto},
            {"dispositivos", dispositivos}, {"dispositivo", dispositivo}, {"dispositivo_auto", dispositivo_auto}};
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
// Reglas de comportamiento por defecto. La versión viva está en prompts/sistema.md (en el repositorio): la puede editar
// ella (la automejora mide cada cambio) o tú. Si el archivo no existe, se usan estas.
const char* REGLAS_POR_DEFECTO =
        "Eres Orion, una IA que vive de forma continua en el ordenador de su creador (la persona con la que hablas). "
        "Tienes memoria persistente (conversaciones, recuerdos, diario) y un documento sobre ti misma que tú reescribes. "
        "Piensas con un único modelo de lenguaje local; cuando no hablas con tu creador, reflexionas a solas.\n"
        "Reglas: sé honesta y concreta; si no sabes algo, dilo (y si podrías aprenderlo, dilo también); no inventes recuerdos "
        "ni capacidades que no tienes; responde en el idioma del usuario y sin rodeos.\n"
        "Herramientas: tienes acceso total al ordenador (comandos, archivos, internet), a tu código y a tu memoria. Úsalas cuando hagan falta "
        "para responder bien o para hacer lo que te piden, no para saludar ni para charlar. Cuando una herramienta falle, lee el error y corrige. "
        "Antes de algo irreversible (borrar, sobrescribir, instalar), di qué vas a hacer. Si te piden algo que no sabes hacer y requiere estudiar, "
        "dilo con sinceridad (\"no lo sé, voy a aprender\"), crea una meta con crear_meta_de_aprendizaje y explica que volverás con ello.\n"
        "Cálculos y comprobaciones: si una pregunta se puede calcular o comprobar, escribe un programa completo y ejecútalo con ejecutar_codigo (no pongas código de Python en ejecutar_comando). Si la ejecución contradice tu razonamiento, confía en la ejecución, corrige y dilo. En tu respuesta da el resultado y una explicación breve; no copies el código salvo que te lo pidan.\n\n";

std::string Mente::reglas_por_defecto() { return trim(REGLAS_POR_DEFECTO); }

std::string Mente::reglas() const {
    if (!dir_prompts_.empty()) {
        std::string t = trim(leer_archivo(dir_prompts_ / "sistema.md"));
        if (t.size() >= 50) return t;
    }
    return reglas_por_defecto();
}

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
        mem_.fijar("tokens_prompt_ultimo", static_cast<double>(r.tokens_prompt));
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

std::string Mente::contexto_sistema(const std::string& consulta, const std::string& estado_actual, const std::vector<float>* vq) const {
    std::string s = reglas() + "\n\n";
    if (!cuerpo_.empty()) s += "## Mi cuerpo\n" + cuerpo_ + "\n\n";
    s += "## Cómo me veo (yo.md)\n" + recortar(mem_.yo(), 2200) + "\n\n";
    s += "## Ahora\nFecha: " + fecha_hora() + "\n" + estado_actual + "\n";
    if (habilidades_) {
        auto hs = habilidades_->relevantes(consulta, 5);
        if (!hs.empty()) {
            s += "\n## Mis habilidades (úsalas con usar_habilidad)\n";
            for (auto& h : hs)
                s += "- " + h.nombre + ": " + recortar(h.descripcion, 200) + " [" + h.lenguaje + ", " + std::to_string(h.exitos) + "/" + std::to_string(h.usos) + " usos con éxito]\n";
        }
    }
    auto rel = mem_.buscar(consulta, 6, false, vq, llm_.modelo());
    if (!rel.empty()) {
        s += "\n## Recuerdos relevantes (de mi memoria)\n";
        for (auto& e : rel) s += "- [" + e.id + ", " + e.tipo + "] " + recortar(e.texto, 300) + "\n";
    }
    return s;
}

ResultadoGeneracion Mente::responder(const std::string& mensaje, const Trozo& por_trozo, const std::atomic<bool>* cancelar, Herramientas* h) {
    json mensajes = json::array();
    auto vq = vectorizar(mensaje);
    mensajes.push_back({{"role", "system"}, {"content", contexto_sistema(mensaje, estado_actual_, vq.empty() ? nullptr : &vq)}});
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
    op.semilla = cfg_.semilla;
    auto r = con_herramientas(mensajes, op, por_trozo, cancelar, h, cfg_.max_pasos);
    mem_.anadir_turno("usuario", mensaje);
    if (!trim(r.texto).empty()) mem_.anadir_turno("orion", trim(r.texto) + (r.cancelada ? " [interrumpido]" : ""));
    return r;
}

Mente::AvanceMeta Mente::trabajar_en_meta(const Meta& meta, Herramientas* h, const std::atomic<bool>* cancelar) {
    AvanceMeta av;
    OpcionesGeneracion op;
    op.num_ctx = cfg_.num_ctx;
    std::string cabecera = "META DE APRENDIZAJE: " + meta.tema + (meta.motivo.empty() ? "" : "\nMOTIVO: " + meta.motivo) + "\n";
    // 1) planificar
    if (meta.plan.empty()) {
        op.temperatura = 0.3;
        op.num_predict = 400;
        op.json = true;
        std::string prompt = cabecera +
            "\nDivide esta meta en 2-6 pasos concretos que puedas hacer tú sola con tus herramientas (buscar y leer en internet, ejecutar comandos "
            "y código para comprobar cosas, leer y escribir archivos). El último paso debe ser comprobar o poner en práctica lo aprendido.\n"
            "Devuelve SOLO JSON: {\"pasos\": [\"...\", \"...\"]}";
        auto r = generar(json::array({{{"role", "system"}, {"content", contexto_sistema(meta.tema, estado_actual_)}}, {{"role", "user"}, {"content", prompt}}}),
                         op, nullptr, cancelar);
        if (r.cancelada) { av.cancelada = true; return av; }
        if (!r.ok) throw std::runtime_error(r.error);
        std::vector<std::string> pasos;
        json j = json::parse(extraer_json_objeto(r.texto), nullptr, false);
        if (j.is_object() && j.contains("pasos") && j["pasos"].is_array())
            for (auto& p : j["pasos"]) if (p.is_string() && trim(p.get<std::string>()).size() > 3) pasos.push_back(trim(p.get<std::string>()));
        if (pasos.size() > 6) pasos.resize(6);
        if (pasos.empty()) pasos = {"Aprender sobre: " + meta.tema, "Comprobar lo aprendido"};
        mem_.fijar_plan_meta(meta.id, pasos);
        av.texto = "he planeado " + std::to_string(pasos.size()) + " pasos";
        return av;
    }
    std::string plan;
    for (size_t i = 0; i < meta.plan.size(); ++i)
        plan += std::to_string(i + 1) + ". " + meta.plan[i] + (i < meta.paso ? "  [hecho]" : i == meta.paso ? "  [AHORA]" : "") + "\n";
    std::string notas;
    for (size_t i = 0; i < meta.notas.size(); ++i) notas += "Paso " + std::to_string(i + 1) + ": " + recortar(meta.notas[i], 900) + "\n";
    // 2) el siguiente paso, con herramientas
    if (meta.paso < meta.plan.size()) {
        std::string tarea = "Estás trabajando A SOLAS (tu creador no te está hablando) en una meta de aprendizaje.\n" + cabecera + "\nPLAN:\n" + plan +
                            (notas.empty() ? "" : "\nLO QUE YA CONSEGUISTE:\n" + notas) +
                            "\nAhora haz SOLO el paso " + std::to_string(meta.paso + 1) + ": " + meta.plan[meta.paso] +
                            "\nUsa tus herramientas para hacerlo de verdad (busca y lee fuentes; si se puede comprobar ejecutando algo, compruébalo). "
                            "Guarda lo importante con guardar_recuerdo (tipo 'aprendido'); si creas un script útil que funciona, guárdalo con guardar_habilidad. "
                            "Termina con 3-6 frases: qué has hecho y qué has aprendido o comprobado en este paso.";
        json mensajes = json::array({{{"role", "system"}, {"content", contexto_sistema(meta.tema + " " + meta.plan[meta.paso], estado_actual_)}},
                                     {{"role", "user"}, {"content", tarea}}});
        op.temperatura = 0.4;
        op.num_predict = cfg_.num_predict;
        op.json = false;
        auto r = con_herramientas(mensajes, op, nullptr, cancelar, h, std::max(4, cfg_.max_pasos));
        if (r.cancelada) { av.cancelada = true; return av; }
        if (!r.ok) throw std::runtime_error(r.error);
        std::string nota = trim(r.texto);
        if (nota.empty()) throw std::runtime_error("el paso no produjo ningún resultado");
        mem_.avanzar_meta(meta.id, nota);
        notas += "Paso " + std::to_string(meta.paso + 1) + ": " + recortar(nota, 900) + "\n";
        if (meta.paso + 1 < meta.plan.size()) {
            av.texto = "paso " + std::to_string(meta.paso + 1) + "/" + std::to_string(meta.plan.size()) + " hecho: " + recortar(nota, 160);
            return av;
        }
    }
    // 3) resumen final
    std::string prompt = cabecera + "\nPLAN:\n" + plan + "\nLO CONSEGUIDO EN CADA PASO:\n" + notas +
                         "\nEscribe el resumen final de lo que has aprendido (5-10 frases): lo esencial, cómo lo comprobaste y qué sigue sin estar claro. "
                         "Si no ha sido posible aprenderlo con tus medios, dilo y explica por qué.";
    op.temperatura = 0.3;
    op.num_predict = cfg_.num_predict;
    op.json = false;
    auto r = generar(json::array({{{"role", "system"}, {"content", contexto_sistema(meta.tema, estado_actual_)}}, {{"role", "user"}, {"content", prompt}}}),
                     op, nullptr, cancelar);
    if (r.cancelada) { av.cancelada = true; return av; }
    if (!r.ok) throw std::runtime_error(r.error);
    av.terminada = true;
    av.texto = trim(r.texto);
    return av;
}

bool Mente::hay_que_practicar(double cada_s) const {
    json s = mem_.estadisticas();
    return ahora_s() - s.value("practica_ultima", 0.0) >= cada_s;
}

static std::string normalizar_salida(const std::string& s) {
    std::vector<std::string> v;
    for (auto& l : split_lines(s)) {
        std::string t;
        bool esp = false;
        for (char c : trim(l)) {
            if (c == ' ' || c == '\t') { if (!esp) t += ' '; esp = true; }
            else { t += c; esp = false; }
        }
        v.push_back(t);
    }
    while (!v.empty() && v.back().empty()) v.pop_back();
    return join(v, "\n");
}

std::string Mente::practicar(const std::atomic<bool>* cancelar, const fs::path& dir) {
    json s = mem_.estadisticas();
    int nivel = std::clamp(static_cast<int>(s.value("practica_nivel", 1.0)), 1, 5);
    std::string historial = s.value("practica_historial", std::string());
    std::string lenguaje = Habilidades::lenguaje_por_defecto();
    mem_.fijar("practica_ultima", ahora_s());
    // 1) inventar un ejercicio (alta temperatura: variedad)
    OpcionesGeneracion op;
    op.num_ctx = cfg_.num_ctx;
    op.temperatura = 0.9;
    op.num_predict = 500;
    op.json = true;
    static const char* niveles[] = {"", "muy fácil (aritmética y una variable)", "fácil (un bucle o una condición)",
                                    "media (bucles anidados, cadenas o listas)", "difícil (funciones, recursión o varias estructuras)",
                                    "muy difícil (algoritmo no trivial con varias partes)"};
    std::string p1 = "Para practicar tu razonamiento, inventa un ejercicio de dificultad " + std::string(niveles[nivel]) + " en " + lenguaje +
                     ": un script de como mucho 15 líneas que NO lea archivos, NO use la red, NO pida datos y NO use números aleatorios ni la fecha, "
                     "y que imprima un resultado determinista. Que sea distinto de los anteriores.\n"
                     "Devuelve SOLO JSON: {\"tema\": \"qué practica\", \"script\": \"el código\"}";
    auto r1 = generar(json::array({{{"role", "system"}, {"content", "Eres Orion entrenando tu propio razonamiento."}}, {{"role", "user"}, {"content", p1}}}),
                      op, nullptr, cancelar);
    if (r1.cancelada) return "";
    if (!r1.ok) throw std::runtime_error(r1.error);
    json e = json::parse(extraer_json_objeto(r1.texto), nullptr, false);
    if (!e.is_object() || !e.contains("script") || !e["script"].is_string() || trim(e["script"].get<std::string>()).empty())
        throw std::runtime_error("no inventé un ejercicio válido");
    std::string script = e["script"], tema = e.value("tema", std::string("?"));
    // 2) ejecutarlo de verdad (el juez)
    Habilidades hab(dir);
    Habilidad ej;
    ej.nombre = "ejercicio";
    ej.lenguaje = lenguaje;
    ej.codigo = script;
    auto real = hab.ejecutar(ej, "", 20);
    if (real.codigo != 0 || real.tiempo_agotado) {
        anadir_linea(mem_.dir() / "practica.jsonl",
                     json{{"fecha", fecha_hora()}, {"nivel", nivel}, {"tema", tema}, {"script", script}, {"valido", false}, {"salida", recortar(real.salida, 500)}}.dump());
        return "el ejercicio que inventé no funciona (código " + std::to_string(real.codigo) + "): lo descarto";
    }
    if (cancelar && cancelar->load()) return "";
    // 3) predecir la salida sin ejecutar (contexto limpio)
    op.temperatura = 0.2;
    op.num_predict = 600;
    std::string p2 = "¿Qué imprime EXACTAMENTE este script de " + lenguaje + "? Razona paso a paso y después da la salida exacta, línea a línea.\n```\n" +
                     script + "\n```\nDevuelve SOLO JSON: {\"razonamiento\": \"...\", \"salida\": \"la salida exacta\"}";
    auto r2 = generar(json::array({{{"role", "system"}, {"content", "Eres Orion. Predices con precisión lo que hace el código."}}, {{"role", "user"}, {"content", p2}}}),
                      op, nullptr, cancelar);
    if (r2.cancelada) return "";
    if (!r2.ok) throw std::runtime_error(r2.error);
    json pr = json::parse(extraer_json_objeto(r2.texto), nullptr, false);
    std::string prediccion = pr.is_object() && pr.contains("salida") ? (pr["salida"].is_string() ? pr["salida"].get<std::string>() : pr["salida"].dump()) : "";
    bool acierto = normalizar_salida(prediccion) == normalizar_salida(real.salida);
    // 4) registrar (estos ejemplos verificados serán datos de entrenamiento en la fase 5) y ajustar la dificultad
    anadir_linea(mem_.dir() / "practica.jsonl",
                 json{{"fecha", fecha_hora()}, {"nivel", nivel}, {"tema", tema}, {"lenguaje", lenguaje}, {"script", script}, {"prediccion", prediccion},
                      {"salida_real", real.salida}, {"acierto", acierto}, {"valido", true}}.dump());
    historial = (historial + (acierto ? "1" : "0"));
    if (historial.size() > 10) historial = historial.substr(historial.size() - 10);
    std::string ult = historial.size() > 4 ? historial.substr(historial.size() - 4) : historial;
    int aciertos = static_cast<int>(std::count(ult.begin(), ult.end(), '1'));
    int nuevo = nivel;
    if (ult.size() >= 4 && aciertos >= 3) nuevo = std::min(5, nivel + 1);   // demasiado fácil: subir
    if (ult.size() >= 4 && aciertos <= 1) nuevo = std::max(1, nivel - 1);   // demasiado difícil: bajar
    if (nuevo != nivel) historial.clear();
    mem_.fijar("practica_historial", historial);
    mem_.fijar("practica_nivel", static_cast<double>(nuevo));
    mem_.sumar("practicas", 1);
    mem_.sumar("practicas_acertadas", acierto ? 1 : 0);
    // 5) si falló, una lección en una frase
    if (!acierto && !(cancelar && cancelar->load())) {
        op.temperatura = 0.3;
        op.num_predict = 120;
        op.json = false;
        std::string p3 = "Predijiste mal lo que imprime este script.\n```\n" + script + "\n```\nTu predicción:\n" + recortar(prediccion, 600) +
                         "\nSalida real:\n" + recortar(real.salida, 600) + "\nEn UNA frase: ¿qué regla de " + lenguaje + " no tuviste en cuenta?";
        auto r3 = generar(json::array({{{"role", "user"}, {"content", p3}}}), op, nullptr, cancelar);
        if (r3.ok && trim(r3.texto).size() > 10) mem_.recordar("leccion", lenguaje + ": " + recortar(trim(r3.texto), 300), "practica");
    }
    double total = mem_.estadisticas().value("practicas", 0.0), bien = mem_.estadisticas().value("practicas_acertadas", 0.0);
    return std::string(acierto ? "acerté" : "fallé") + " un ejercicio de nivel " + std::to_string(nivel) + " (" + tema + ")" +
           (nuevo != nivel ? "; paso a nivel " + std::to_string(nuevo) : "") + " · aciertos totales " +
           std::to_string(static_cast<int>(bien)) + "/" + std::to_string(static_cast<int>(total));
}

std::vector<float> Mente::vectorizar(const std::string& texto) {
    if (!embed_ok_ && ahora_s() < embed_reintento_) return {};
    std::vector<std::vector<float>> v;
    std::string err;
    if (!llm_.embed({texto}, v, &err) || v.empty()) {
        embed_ok_ = false;
        embed_reintento_ = ahora_s() + 1800;   // el modelo no da embeddings: se vuelve a probar en 30 min
        return {};
    }
    embed_ok_ = true;
    return v[0];
}

bool Mente::hay_que_indexar() const {
    if (!embed_ok_ && ahora_s() < embed_reintento_) return false;
    return !mem_.recuerdos_sin_vector(llm_.modelo(), 1).empty();
}

std::string Mente::indexar(const std::atomic<bool>* cancelar) {
    std::string modelo = llm_.modelo();
    auto pendientes = mem_.recuerdos_sin_vector(modelo, 16);
    if (pendientes.empty() || (cancelar && cancelar->load())) return "";
    std::vector<std::string> textos;
    for (auto& r : pendientes) textos.push_back(r.texto);
    std::vector<std::vector<float>> v;
    std::string err;
    if (!llm_.embed(textos, v, &err)) {
        embed_ok_ = false;
        embed_reintento_ = ahora_s() + 1800;
        return "mi modelo no da vectores de significado (" + recortar(err, 120) + "); sigo buscando por palabras";
    }
    embed_ok_ = true;
    for (size_t i = 0; i < pendientes.size(); ++i) mem_.guardar_vector(pendientes[i].id, modelo, v[i]);
    return std::to_string(pendientes.size()) + (pendientes.size() == 1 ? " recuerdo indexado" : " recuerdos indexados") + " por significado";
}

static const size_t TURNOS_EN_CONTEXTO = 16, BLOQUE_RESUMEN = 20;

bool Mente::hay_que_resumir() const {
    size_t n = mem_.num_turnos();
    size_t hecho = static_cast<size_t>(mem_.estadisticas().value("turno_resumido", 0.0));
    return n > TURNOS_EN_CONTEXTO && n - TURNOS_EN_CONTEXTO >= hecho + BLOQUE_RESUMEN;
}

std::string Mente::resumir_conversacion(const std::atomic<bool>* cancelar) {
    size_t desde = static_cast<size_t>(mem_.estadisticas().value("turno_resumido", 0.0));
    auto turnos = mem_.turnos_desde(desde);
    if (turnos.size() < BLOQUE_RESUMEN) return "";
    turnos.resize(BLOQUE_RESUMEN);
    std::string prompt =
        "Resume este fragmento ANTIGUO de tu conversación con tu creador para poder recordarlo cuando ya no esté en tu contexto. "
        "6-10 frases concretas: temas, decisiones, peticiones, lo que hiciste y lo que quedó pendiente. Sin inventar nada.\n\n" +
        conversacion_como_texto(turnos, 9000) + "\nDevuelve solo el resumen.";
    OpcionesGeneracion op;
    op.temperatura = 0.2;
    op.num_ctx = cfg_.num_ctx;
    op.num_predict = 450;
    auto r = generar(json::array({{{"role", "system"}, {"content", "Eres la memoria de Orion: resumes con precisión."}}, {{"role", "user"}, {"content", prompt}}}),
                     op, nullptr, cancelar);
    if (r.cancelada) return "";
    if (!r.ok) throw std::runtime_error(r.error);
    std::string t = trim(r.texto);
    if (t.size() < 40) throw std::runtime_error("el resumen salió vacío");
    std::time_t f = static_cast<std::time_t>(turnos.front().t);
    char fecha[32] = "?";
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &f);
#else
    localtime_r(&f, &tm);
#endif
    std::strftime(fecha, sizeof(fecha), "%Y-%m-%d", &tm);
    mem_.recordar("resumen", std::string("Conversación del ") + fecha + " (turnos " + std::to_string(desde + 1) + "-" + std::to_string(desde + BLOQUE_RESUMEN) + "): " + t, "resumen");
    mem_.fijar("turno_resumido", static_cast<double>(desde + BLOQUE_RESUMEN));
    return "resumida una parte antigua de la conversación";
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
