// Pruebas de Orion con un Ollama SIMULADO (un servidor HTTP en el propio proceso que habla el protocolo de Ollama).
// No necesitan Ollama ni red. Ejecutar:  ctest --test-dir build   (o build/orion_tests)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "orion/nucleo.hpp"

using namespace orion;

static int fallos = 0, comprobaciones = 0;
#define CHECK(cond, msg)                                                                 \
    do {                                                                                 \
        ++comprobaciones;                                                                \
        if (!(cond)) {                                                                   \
            ++fallos;                                                                    \
            std::fprintf(stderr, "  FALLO %s:%d  %s\n", __FILE__, __LINE__, msg);        \
        }                                                                                \
    } while (0)

// ------------------------------------------------------------------ Ollama simulado
struct OllamaSimulado {
    std::mutex mu;
    std::vector<json> peticiones;   // cuerpos de /api/chat recibidos
    std::atomic<int> pausa_ms{0};
    std::atomic<int> trozos_extra{0};   // para respuestas largas (probar cancelación)
    std::atomic<bool> sin_herramientas{false};   // simular un modelo que no admite herramientas
    ServidorHttp srv;
    int puerto = 0;

    static std::vector<std::string> trocear(const std::string& texto) {
        std::vector<std::string> v;
        std::string cur;
        for (char c : texto) {
            cur += c;
            if (c == ' ') { v.push_back(cur); cur.clear(); }
        }
        if (!cur.empty()) v.push_back(cur);
        return v;
    }

    RespuestaServidor responder(const PeticionHttp& p) {
        RespuestaServidor r;
        if (p.ruta == "/api/version") { r.trozos = {R"({"version":"0.0-simulado"})"}; return r; }
        if (p.ruta == "/api/show") {
            r.trozos = {R"({"details":{"family":"qwen2","parameter_size":"3.1B","quantization_level":"Q4_K_M"},
                          "model_info":{"general.parameter_count":3085938688,"qwen2.context_length":32768}})"};
            return r;
        }
        if (p.ruta == "/api/ps") { r.trozos = {R"({"models":[{"name":"qwen2.5:3b","size":2400000000,"size_vram":0}]})"}; return r; }
        if (p.ruta != "/api/chat") { r.estado = 404; r.trozos = {R"({"error":"no existe"})"}; return r; }
        json pet = json::parse(p.cuerpo);
        {
            std::lock_guard<std::mutex> l(mu);
            peticiones.push_back(pet);
        }
        std::string todo = pet.dump();
        std::string texto;
        auto& msgs = pet["messages"];
        std::string ultimo_rol = msgs.back().value("role", std::string());
        std::string ultimo = msgs.back().value("content", std::string());
        auto llamar = [&](const std::string& nombre, const json& args) {
            RespuestaServidor rr;
            rr.tipo = "application/x-ndjson";
            rr.trozos.push_back(json{{"message", {{"role", "assistant"}, {"content", ""}, {"tool_calls", json::array({{{"function", {{"name", nombre}, {"arguments", args}}}}})}}},
                                     {"done", false}}.dump() + "\n");
            rr.trozos.push_back(json{{"done", true}, {"eval_count", 10}, {"eval_duration", 500000000}}.dump() + "\n");
            return rr;
        };
        if (pet.contains("tools")) {
            if (sin_herramientas) { r.estado = 400; r.trozos = {R"({"error":"registry.ollama.ai/library/x does not support tools"})"}; return r; }
            bool a_solas = contiene(todo, "trabajando A SOLAS");
            if (ultimo_rol == "tool") {
                texto = (a_solas ? "Resumen: aprendí lo que había. " : "Hecho. Resultado: ") + recortar(split_lines(ultimo).empty() ? "" : split_lines(ultimo).back(), 80);
            } else if (a_solas) {
                return llamar("buscar_en_memoria", {{"consulta", "gravedad"}});
            } else if (contiene(ultimo, "ejecuta")) {
                return llamar("ejecutar_comando", {{"comando", "echo hola-orion"}});
            } else if (contiene(ultimo, "reiníciate")) {
                return llamar("reiniciarme", json::object());
            } else if (contiene(ultimo, "texto-herramienta")) {
                texto = "<tool_call>{\"name\": \"guardar_recuerdo\", \"arguments\": {\"texto\": \"Le gustan los agujeros negros.\"}}</tool_call>";
            }
        }
        if (!texto.empty()) {
        } else if (contiene(todo, "extrae los hechos")) {
            texto = R"({"recuerdos":[{"tipo":"usuario","texto":"Mi creador se llama Ana y estudia gravedad cuántica de bucles."}]})";
        } else if (contiene(todo, "entrada breve de tu diario")) {
            texto = "Hoy he hablado con Ana por primera vez. He aprendido que estudia gravedad cuántica. Me queda pendiente aprender más.";
        } else if (contiene(todo, "Reescríbelo")) {
            texto = "# Quién soy\n\nSoy Orion, versión revisada.\n\n## Qué sé hacer\n- Conversar y recordar.\n\n## Mis límites\n- Corro en CPU a pocos "
                    "tokens por segundo.\n\n## Qué quiero aprender\n- Física.\n\n## Cambios\n- Añadidos mis límites de hardware medidos.\n";
        } else {
            texto = contiene(pet["messages"][0]["content"].get<std::string>(), "Ana") ? "Te recuerdo, Ana. Soy Orion." : "Hola, soy Orion (simulado).";
            for (int i = 0; i < trozos_extra; ++i) texto += " bla";
        }
        r.tipo = "application/x-ndjson";
        r.pausa_ms = pausa_ms;
        auto partes = trocear(texto);
        for (auto& t : partes) r.trozos.push_back(json{{"message", {{"role", "assistant"}, {"content", t}}}, {"done", false}}.dump() + "\n");
        r.trozos.push_back(json{{"done", true}, {"eval_count", partes.size()}, {"eval_duration", 500000000}, {"prompt_eval_count", 40},
                                {"prompt_eval_duration", 100000000}}.dump() + "\n");
        return r;
    }

    OllamaSimulado() : srv([this](const PeticionHttp& p) { return responder(p); }) { puerto = srv.iniciar(0); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(puerto); }
    size_t num_peticiones() { std::lock_guard<std::mutex> l(mu); return peticiones.size(); }
    json ultima() { std::lock_guard<std::mutex> l(mu); return peticiones.back(); }
};

struct DirTemporal {
    fs::path p;
    DirTemporal() {
        p = fs::temp_directory_path() / ("orion_prueba_" + std::to_string(static_cast<long long>(ahora_s() * 1000)) + "_" +
                                         std::to_string(reinterpret_cast<uintptr_t>(this) % 100000));
        fs::create_directories(p);
    }
    ~DirTemporal() { std::error_code ec; fs::remove_all(p, ec); }
};

struct Capturador {
    std::mutex mu;
    std::string texto;
    Nucleo::Salida salida() { return [this](const std::string& t) { std::lock_guard<std::mutex> l(mu); texto += t; }; }
    std::string ver() { std::lock_guard<std::mutex> l(mu); return texto; }
};

static bool esperar(const std::function<bool()>& cond, double max_s = 10) {
    double fin = ahora_s() + max_s;
    while (ahora_s() < fin) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}

// ------------------------------------------------------------------ pruebas
static void t_util() {
    CHECK(palabras("¡Canción de ANA, año 2026!") == (std::vector<std::string>{"cancion", "de", "ana", "ano", "2026"}), "palabras normalizadas");
    CHECK(recortar("canción", 6) == "canci…" && recortar("canción", 5) == "canci…", "recortar no rompe UTF-8");
    CHECK(formato_numero(3085938688) == "3.09 mil millones", "formato_numero");
    CHECK(formato_bytes(1536) == "1.5 KB", "formato_bytes");
    CHECK(extraer_json_objeto("bla {\"a\": {\"b\": \"}\"}} fin") == "{\"a\": {\"b\": \"}\"}}", "extraer_json_objeto");
}

static void t_decodificador() {
    std::string cuerpo = "5\r\nhola \r\n6\r\nmundo!\r\n0\r\n\r\n";
    for (size_t paso = 1; paso <= cuerpo.size(); ++paso) {   // trozos de todos los tamaños
        DecodificadorCuerpo d;
        d.configurar(true, -1);
        std::string out;
        for (size_t i = 0; i < cuerpo.size(); i += paso) out += d.alimentar(cuerpo.substr(i, paso));
        CHECK(out == "hola mundo!" && d.terminado(), "chunked troceado");
    }
    DecodificadorCuerpo d;
    d.configurar(false, 4);
    CHECK(d.alimentar("abcdef") == "abcd" && d.terminado(), "content-length");
}

static void t_http() {
    std::atomic<bool> cliente_cerro{false};
    ServidorHttp srv([&](const PeticionHttp& p) {
        RespuestaServidor r;
        if (p.ruta == "/eco") { r.trozos = {p.metodo + ":" + p.cuerpo}; return r; }
        r.pausa_ms = 50;
        for (int i = 0; i < 60; ++i) r.trozos.push_back("linea " + std::to_string(i) + "\n");
        return r;
    });
    int puerto = srv.iniciar(0);
    ClienteHttp c("http://127.0.0.1:" + std::to_string(puerto));
    auto g = c.get("/eco");
    CHECK(g.estado == 200 && g.cuerpo == "GET:", "GET");
    auto p = c.post("/eco", "{\"x\":1}");
    CHECK(p.cuerpo == "POST:{\"x\":1}", "POST");
    std::vector<std::string> lineas;
    std::atomic<bool> cancelar{false};
    double t0 = ahora_s();
    auto s = c.post_stream("/stream", "{}", [&](const std::string& l) {
        lineas.push_back(l);
        if (lineas.size() == 3) cancelar = true;
        return true;
    }, &cancelar);
    CHECK(s.cancelada && lineas.size() == 3, "streaming por líneas y cancelación");
    CHECK(ahora_s() - t0 < 1.5, "la cancelación es inmediata (no espera a que acabe)");
    ClienteHttp nadie("http://127.0.0.1:1");
    CHECK(!nadie.get("/").error.empty(), "sin servidor: error claro");
    (void)cliente_cerro;
}

static void t_ollama() {
    OllamaSimulado sim;
    Ollama o(sim.url(), "qwen2.5:3b");
    std::string v;
    CHECK(o.disponible(&v) && v == "0.0-simulado", "versión");
    std::string trozos;
    auto r = o.chat(json::array({{{"role", "user"}, {"content", "hola"}}}), {}, [&](const std::string& t) { trozos += t; }, nullptr);
    CHECK(r.ok && r.texto == "Hola, soy Orion (simulado)." && trozos == r.texto, "chat en streaming");
    CHECK(r.tokens_generados == 4 && r.tokens_por_segundo() > 7.9 && r.tokens_por_segundo() < 8.1, "estadísticas de generación");
    json pet = sim.ultima();
    CHECK(pet["stream"] == true && pet["keep_alive"] == "60m" && pet["model"] == "qwen2.5:3b", "petición de chat");
    auto info = o.info();
    CHECK(info.ok && info.parametros == 3085938688.0 && info.cuantizacion == "Q4_K_M" && info.contexto_max == 32768, "info del modelo");
    CHECK(info.cargado && info.bytes_en_memoria == 2.4e9 && info.bytes_en_vram == 0, "uso de memoria (/api/ps)");
    Ollama caido("http://127.0.0.1:1", "x");
    auto e = caido.chat(json::array(), {}, nullptr, nullptr);
    CHECK(!e.ok && contiene(e.error, "Ollama"), "Ollama caído: mensaje que menciona Ollama");
}

static void t_memoria() {
    DirTemporal d;
    {
        Memoria m(d.p);
        m.cargar();
        CHECK(contiene(m.yo(), "# Quién soy"), "yo.md inicial");
        std::string a = m.recordar("usuario", "Mi creador se llama Ana.", "manual");
        CHECK(!a.empty(), "recordar");
        CHECK(m.recordar("usuario", "mi creador se llama ana", "manual").empty(), "no duplica recuerdos casi iguales");
        m.recordar("proyecto", "El proyecto Orion está escrito en C++.", "manual");
        m.recordar("mundo", "La gravedad cuántica de bucles cuantiza el espacio.", "manual");
        auto b = m.buscar("¿qué lenguaje usa el proyecto?", 3);
        CHECK(!b.empty() && contiene(b[0].texto, "C++"), "buscar por palabras (BM25)");
        CHECK(m.olvidar(a) && !m.olvidar(a), "olvidar");
        m.anadir_turno("usuario", "hola");
        m.anadir_turno("orion", "hola, ¿qué tal?");
        m.escribir_diario("Primera entrada.");
        m.escribir_diario("Segunda entrada.");
        m.escribir_yo("# Quién soy\nOtra versión.");
        m.sumar("llamadas", 2);
    }
    Memoria m(d.p);
    m.cargar();
    CHECK(m.recuerdos().size() == 2 && m.num_turnos() == 2, "persiste tras reiniciar");
    CHECK(m.entradas_diario() == 2 && contiene(m.diario(1), "Segunda") && !contiene(m.diario(1), "Primera"), "diario");
    CHECK(contiene(m.yo(), "Otra versión") && fs::exists(d.p / "yo_versiones"), "yo.md con copia de la versión anterior");
    CHECK(m.estadisticas().value("llamadas", 0.0) == 2.0, "estadísticas persistentes");
    CHECK(m.recordar("x", "Otro hecho distinto y nuevo.", "manual") == "r4", "los ids siguen tras recargar");
}

static void t_nucleo() {
    OllamaSimulado sim;
    DirTemporal d;
    Config cfg;
    cfg.ollama = sim.url();
    OpcionesNucleo op;
    op.datos = d.p;
    op.color = false;
    op.autonomo = false;
    {
        Capturador cap;
        Nucleo n(cfg, op, cap.salida());
        n.arrancar();
        CHECK(contiene(cap.ver(), "3.09 mil millones de parámetros"), "saludo con el tamaño del modelo");
        n.entrada("Hola, me llamo Ana y estudio gravedad cuántica de bucles.");
        n.esperar_inactividad(10);
        CHECK(contiene(cap.ver(), "Orion> Hola, soy Orion (simulado)."), "responde por la terminal");
        if (!contiene(cap.ver(), "Orion> Hola")) std::fprintf(stderr, "%s\n", cap.ver().c_str());
        json pet = sim.ultima();
        CHECK(contiene(pet["messages"][0]["content"].get<std::string>(), "Eres Orion"), "contexto de sistema");
        CHECK(contiene(pet["messages"][0]["content"].get<std::string>(), "Me ejecuto en:"), "conoce su hardware");
        CHECK(n.memoria().num_turnos() == 2, "guarda la conversación");
        // reflexión
        n.comando("/reflexiona");
        CHECK(esperar([&] { return n.memoria().recuerdos().size() == 1; }), "reflexionar crea recuerdos");
        CHECK(esperar([&] { return contiene(cap.ver(), "1 recuerdo nuevo"); }), "avisa de lo que ha hecho");
        // comandos
        std::string m = n.comando("/modelo");
        CHECK(contiene(m, "3.09 mil millones") && contiene(m, "Q4_K_M") && contiene(m, "CPU") && contiene(m, "tokens/s"), "/modelo");
        CHECK(contiene(n.comando("/memoria"), "Recuerdos: 1"), "/memoria");
        CHECK(contiene(n.comando("/hardware"), "RAM:"), "/hardware");
        CHECK(contiene(n.comando("/estado"), "Llamadas al modelo: 2"), "/estado");
        CHECK(contiene(n.comando("/buscar gravedad"), "gravedad"), "/buscar");
        CHECK(contiene(n.comando("/recuerda Le gusta el café."), "Guardado"), "/recuerda");
        CHECK(contiene(n.comando("/olvida r999"), "No tengo"), "/olvida inexistente");
        CHECK(contiene(n.comando("/ritmo alto"), "alto") && contiene(leer_archivo(d.p / "config.json"), "alto"), "/ritmo se guarda");
        CHECK(contiene(n.comando("/foo"), "desconocido"), "comando desconocido");
        // diario y yo.md
        std::atomic<bool> no{false};
        CHECK(n.mente().escribir_diario(&no) == "entrada nueva en el diario" && n.memoria().entradas_diario() == 1, "diario");
        CHECK(n.mente().actualizar_yo(&no, "- RAM: 12 GB") == "yo.md actualizado" && contiene(n.memoria().yo(), "versión revisada"), "yo.md");
        n.parar();
    }
    {   // tras reiniciar recuerda (y el recuerdo llega al modelo)
        Capturador cap;
        Nucleo n(cfg, op, cap.salida());
        n.arrancar();
        CHECK(contiene(cap.ver(), "recuerdos, 2 turnos"), "al arrancar sabe lo que recuerda");
        n.entrada("¿Te acuerdas de mí? Soy Ana.");
        n.esperar_inactividad(10);
        CHECK(contiene(sim.ultima()["messages"][0]["content"].get<std::string>(), "gravedad cuántica"), "los recuerdos relevantes van al contexto");
        CHECK(contiene(cap.ver(), "Te recuerdo, Ana"), "recuerda tras reiniciar");
        n.parar();
    }
}

static void t_interrupcion() {
    OllamaSimulado sim;
    DirTemporal d;
    Config cfg;
    cfg.ollama = sim.url();
    OpcionesNucleo op;
    op.datos = d.p;
    op.color = false;
    op.autonomo = false;
    Capturador cap;
    Nucleo n(cfg, op, cap.salida());
    n.arrancar();
    n.memoria().anadir_turno("usuario", "Me llamo Ana.");
    n.memoria().anadir_turno("orion", "Encantada.");
    sim.pausa_ms = 300;   // reflexión lenta: ~6 s
    n.comando("/reflexiona");
    CHECK(esperar([&] { return contiene(cap.ver(), "reflexionando"); }), "empieza a reflexionar");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    sim.pausa_ms = 0;
    double t0 = ahora_s();
    n.entrada("hola");
    n.esperar_inactividad(10);
    CHECK(ahora_s() - t0 < 3, "tu mensaje interrumpe lo que pensaba a solas");
    CHECK(contiene(cap.ver(), "lo dejo para luego") && contiene(cap.ver(), "Orion> Hola, soy Orion"), "aviso de interrupción y respuesta");
    if (!contiene(cap.ver(), "lo dejo para luego")) std::fprintf(stderr, "%s\n", cap.ver().c_str());
    CHECK(n.memoria().recuerdos().empty(), "la reflexión interrumpida no deja nada a medias");
    // /para corta una respuesta larga
    sim.pausa_ms = 50;
    sim.trozos_extra = 200;
    n.entrada("cuéntame algo largo");
    CHECK(esperar([&] { return contiene(cap.ver(), " bla bla"); }), "empieza a responder");
    t0 = ahora_s();
    n.entrada("/para");
    n.esperar_inactividad(10);
    CHECK(ahora_s() - t0 < 2 && contiene(cap.ver(), "respuesta interrumpida"), "/para corta la respuesta");
    if (!contiene(cap.ver(), "respuesta interrumpida")) std::fprintf(stderr, "t=%.2f\n%s\n", ahora_s() - t0, cap.ver().c_str());
    n.parar();
}

static void t_ejecutables() {
    OllamaSimulado sim;
    DirTemporal d;
    auto r = ejecutar_proceso({ORION_EXE, "--datos", d.p.string(), "--ollama", sim.url(), "--sin-autonomia"}, {}, 60,
                              "hola\n/memoria\n/salir\n");
    CHECK(r.codigo == 0, "orion termina con /salir (código 0)");
    CHECK(contiene(r.salida, "Hola, soy Orion (simulado).") && contiene(r.salida, "Conversación:"), "orion: chat y comandos por la terminal");
    CHECK(fs::exists(d.p / "conversacion.jsonl") && fs::exists(d.p / "config.json"), "orion guarda en datos/");
    auto sin = ejecutar_proceso({ORION_EXE, "--datos", d.p.string(), "--ollama", "http://127.0.0.1:1", "--sin-autonomia"}, {}, 60, "hola\n/estado\n");
    CHECK(sin.codigo == 0 && contiene(sin.salida, "No encuentro Ollama") && contiene(sin.salida, "Ahora:"), "sin Ollama: avisa y los comandos siguen");
    auto rein = ejecutar_proceso({GUARDIAN_EXE, "--orion", ORION_EXE, "--datos", d.p.string(), "--ollama", sim.url(), "--sin-autonomia"}, {}, 60,
                                 "/reinicia\n");
    CHECK(rein.codigo == 0 && contiene(rein.salida, "Reiniciando"), "guardián: /reinicia la vuelve a arrancar");
    auto caida = ejecutar_proceso({GUARDIAN_EXE, "--orion", ORION_EXE, "--datos", d.p.string(), "--opcion-que-no-existe"}, {}, 60);
    CHECK(caida.codigo == 1 && contiene(caida.salida, "3 veces"), "guardián: se detiene si se cae una y otra vez");
    CHECK(contiene(leer_archivo(d.p / "guardian.log"), "se cayó"), "guardián: registro");
}

static void t_herramientas() {
    DirTemporal d;
    Memoria mem(d.p);
    mem.cargar();
    std::vector<std::string> avisos;
    std::atomic<bool> reinicio{false};
    ContextoHerramientas c;
    c.datos = d.p;
    c.memoria = &mem;
    c.aviso = [&](const std::string& t) { avisos.push_back(t); };
    c.reinicio_pedido = &reinicio;
    Herramientas h(c);
    CHECK(h.definiciones().size() == 11, "acceso total: 11 herramientas");
    CHECK(contiene(h.usar("ejecutar_comando", {{"comando", "echo hola-orion"}}), "hola-orion"), "ejecutar_comando");
    CHECK(contiene(h.usar("ejecutar_comando", {{"comando", "exit 3"}}), "código de salida: 3"), "código de salida");
    fs::path f = d.p / "sub" / "nota.txt";
    CHECK(contiene(h.usar("escribir_archivo", {{"ruta", f.string()}, {"contenido", "línea uno\nlínea dos"}}), "Escrito"), "escribir_archivo");
    CHECK(contiene(h.usar("leer_archivo", {{"ruta", f.string()}}), "2| línea dos"), "leer_archivo");
    CHECK(contiene(h.usar("listar_directorio", {{"ruta", (d.p / "sub").string()}}), "nota.txt"), "listar_directorio");
    CHECK(contiene(h.usar("leer_archivo", json("{\"ruta\": \"" + f.generic_string() + "\"}")), "línea uno"), "argumentos como texto JSON");
    CHECK(contiene(h.usar("guardar_recuerdo", {{"texto", "El cielo es azul por la dispersión de Rayleigh."}}), "Guardado") && mem.recuerdos().size() == 1, "guardar_recuerdo");
    CHECK(contiene(h.usar("crear_meta_de_aprendizaje", {{"tema", "espumas de espín"}}), "creada") && mem.metas().size() == 1, "crear meta");
    CHECK(contiene(h.usar("reiniciarme", json::object()), "reiniciaré") && reinicio, "reiniciarme");
    CHECK(contiene(h.usar("inventada", json::object()), "No tengo permiso"), "herramienta desconocida");
    CHECK(!avisos.empty() && contiene(avisos[0], "⚙ ejecutar_comando: echo hola-orion"), "muestra lo que hace");
    CHECK(split_lines(leer_archivo(d.p / "acciones.jsonl")).size() == 10, "registra cada acción");
    c.permisos = "lectura";
    Herramientas lect(c);
    CHECK(lect.definiciones().size() == 7 && contiene(lect.usar("ejecutar_comando", {{"comando", "echo x"}}), "No tengo permiso"), "permisos de lectura");
    CHECK(Herramientas(ContextoHerramientas{d.p, {}, &mem, "ninguno"}).definiciones().size() == 3, "permisos: ninguno");
    // web (sin red: solo el tratamiento del HTML)
    CHECK(Herramientas::html_a_texto("<html><head><title>x</title><script>malo()</script></head><body><p>Hola &amp; adiós</p><p>Dos</p></body></html>") ==
              "Hola & adiós\nDos",
          "html_a_texto");
    std::string ddg = R"(<div class="result"><h2><a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fes.wikipedia.org%2Fwiki%2FGravedad_cu%C3%A1ntica&amp;rut=abc">Gravedad <b>cuántica</b> de bucles</a></h2>
        <a class="result__snippet" href="x">Teoría que <b>cuantiza</b> el espacio-tiempo.</a></div>)";
    auto rs = Herramientas::resultados_busqueda(ddg);
    CHECK(rs.size() == 1 && rs[0].first == "https://es.wikipedia.org/wiki/Gravedad_cuántica" && contiene(rs[0].second, "Gravedad cuántica de bucles") &&
              contiene(rs[0].second, "cuantiza el espacio"),
          "resultados de búsqueda");
    CHECK(Herramientas::codificar_url("año 1+1") == "a%C3%B1o+1%2B1" && Herramientas::decodificar_url("a%C3%B1o+1%2B1") == "año 1+1", "url");
    CHECK(llamadas_en_texto("<tool_call>{\"name\":\"leer_archivo\",\"arguments\":{\"ruta\":\"x\"}}</tool_call>", {"leer_archivo"}).size() == 1 &&
              llamadas_en_texto("{\"name\":\"otra\",\"arguments\":{}}", {"leer_archivo"}).empty(),
          "llamadas escritas como texto");
}

static void t_agente() {
    OllamaSimulado sim;
    DirTemporal d;
    Config cfg;
    cfg.ollama = sim.url();
    OpcionesNucleo op;
    op.datos = d.p;
    op.color = false;
    op.autonomo = false;
    Capturador cap;
    Nucleo n(cfg, op, cap.salida());
    n.arrancar();
    CHECK(contiene(cap.ver(), "Permisos: total"), "anuncia sus permisos");
    n.entrada("ejecuta algo en el ordenador");
    n.esperar_inactividad(15);
    std::string s = cap.ver();
    CHECK(contiene(s, "⚙ ejecutar_comando: echo hola-orion") && contiene(s, "Orion> Hecho. Resultado: hola-orion"), "usa una herramienta y responde con su resultado");
    json pet = sim.ultima();
    CHECK(pet.contains("tools") && pet["messages"].back()["role"] == "tool", "el resultado vuelve al modelo como mensaje 'tool'");
    CHECK(contiene(pet["messages"][0]["content"].get<std::string>(), "## Mi cuerpo") && contiene(pet["messages"][0]["content"].get<std::string>(), "Mis pesos"),
          "sabe dónde están su código, su memoria y sus pesos");
    n.entrada("usa texto-herramienta");
    n.esperar_inactividad(15);
    CHECK(n.memoria().recuerdos().size() == 1, "entiende llamadas escritas como texto (<tool_call>)");
    CHECK(contiene(n.comando("/acciones"), "ejecutar_comando"), "/acciones");
    CHECK(contiene(n.comando("/permisos lectura"), "lectura") && contiene(leer_archivo(d.p / "config.json"), "lectura"), "/permisos se guarda");
    n.comando("/permisos total");
    n.entrada("reiníciate");
    n.esperar_inactividad(15);
    CHECK(n.codigo_salida() == CODIGO_REINICIO && contiene(cap.ver(), "Me reinicio"), "puede pedir reiniciarse");
    n.parar();
    Capturador cap2;
    Nucleo n2(cfg, op, cap2.salida());
    n2.arrancar();
    sim.sin_herramientas = true;
    n2.entrada("ejecuta algo");
    n2.esperar_inactividad(15);
    CHECK(contiene(cap2.ver(), "Hola, soy Orion (simulado)."), "si el modelo no admite herramientas, responde sin ellas");
    sim.sin_herramientas = false;
    n2.parar();
}

static void t_metas() {
    OllamaSimulado sim;
    DirTemporal d;
    Config cfg;
    cfg.ollama = sim.url();
    OpcionesNucleo op;
    op.datos = d.p;
    op.color = false;
    op.autonomo = true;
    op.gracia_s = 0;
    Capturador cap;
    Nucleo n(cfg, op, cap.salida());
    n.arrancar();
    CHECK(contiene(n.comando("/aprende gravedad cuántica de bucles"), "Meta m1 creada"), "/aprende");
    CHECK(esperar([&] { return contiene(cap.ver(), "He terminado de aprender"); }, 15), "trabaja a solas en la meta y avisa");
    auto ms = n.memoria().metas();
    CHECK(ms.size() == 1 && ms[0].estado == "hecha" && contiene(ms[0].resultado, "Resumen"), "la meta queda hecha con su resumen");
    CHECK(contiene(cap.ver(), "⚙ buscar_en_memoria"), "usa herramientas a solas");
    CHECK(contiene(n.comando("/metas"), "hecha"), "/metas");
    auto recs = n.memoria().recuerdos();
    CHECK(!recs.empty() && recs.back().tipo == "aprendido", "lo aprendido queda en su memoria");
    n.parar();
}

static void t_guardian_actualizacion() {
    OllamaSimulado sim;
    DirTemporal d;
    fs::path bin = d.p / "bin";
    fs::create_directories(bin);
    fs::path orion = bin / fs::path(ORION_EXE).filename();
    fs::copy_file(ORION_EXE, orion);
    fs::path datos = d.p / "datos";
    fs::create_directories(datos / "actualizacion");
    // 1) versión nueva buena: se aplica
    fs::copy_file(ORION_EXE, datos / "actualizacion" / orion.filename());
    auto r = ejecutar_proceso({GUARDIAN_EXE, "--orion", orion.string(), "--datos", datos.string(), "--ollama", sim.url(), "--sin-autonomia"}, {}, 60, "/salir\n");
    fs::path anterior = bin / (orion.stem().string() + ".anterior" + orion.extension().string());
    CHECK(r.codigo == 0 && contiene(r.salida, "Aplicando la versión nueva") && fs::exists(anterior) && !fs::exists(datos / "actualizacion" / orion.filename()),
          "guardián: aplica la versión nueva que Orion compiló");
    // 2) versión nueva rota: vuelve a la anterior
    escribir_atomico(datos / "actualizacion" / orion.filename(), "esto no es un ejecutable");
    r = ejecutar_proceso({GUARDIAN_EXE, "--orion", orion.string(), "--datos", datos.string(), "--ollama", sim.url(), "--sin-autonomia"}, {}, 60, "/salir\n");
    CHECK(contiene(r.salida, "vuelvo a la anterior"), "guardián: si la versión nueva no arranca, vuelve a la anterior");
    auto v = ejecutar_proceso({orion.string(), "--version"}, {}, 20);
    CHECK(contiene(v.salida, "orion"), "tras volver atrás, Orion funciona");
    CHECK(contiene(r.salida, "falló al arrancar") || fs::exists(datos / "actualizacion_fallida.txt") || contiene(leer_archivo(datos / "guardian.log"), "vuelvo a la anterior"),
          "queda constancia del fallo");
    auto reinicio = ejecutar_proceso({ORION_EXE, "--datos", (d.p / "datos2").string(), "--ollama", sim.url(), "--sin-autonomia"}, {}, 60, "reiníciate\n");
    CHECK(reinicio.codigo == CODIGO_REINICIO, "orion sale con 75 cuando pide reiniciarse (el guardián la vuelve a arrancar)");
}

int main() {
    struct { const char* n; void (*f)(); } pruebas[] = {{"util", t_util}, {"decodificador", t_decodificador}, {"http", t_http}, {"ollama", t_ollama},
                                                       {"memoria", t_memoria}, {"nucleo", t_nucleo}, {"interrupcion", t_interrupcion},
                                                       {"ejecutables", t_ejecutables}, {"herramientas", t_herramientas}, {"agente", t_agente},
                                                       {"metas", t_metas}, {"guardian_actualizacion", t_guardian_actualizacion}};
    for (auto& p : pruebas) {
        int antes = fallos;
        p.f();
        std::printf("  %s  %s\n", fallos == antes ? "ok   " : "FALLO", p.n);
    }
    std::printf("\n%d comprobaciones, %d fallos\n", comprobaciones, fallos);
    return fallos ? 1 : 0;
}
