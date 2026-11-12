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
        if (contiene(todo, "extrae los hechos")) {
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

int main() {
    struct { const char* n; void (*f)(); } pruebas[] = {{"util", t_util}, {"decodificador", t_decodificador}, {"http", t_http}, {"ollama", t_ollama},
                                                       {"memoria", t_memoria}, {"nucleo", t_nucleo}, {"interrupcion", t_interrupcion},
                                                       {"ejecutables", t_ejecutables}};
    for (auto& p : pruebas) {
        int antes = fallos;
        p.f();
        std::printf("  %s  %s\n", fallos == antes ? "ok   " : "FALLO", p.n);
    }
    std::printf("\n%d comprobaciones, %d fallos\n", comprobaciones, fallos);
    return fallos ? 1 : 0;
}
