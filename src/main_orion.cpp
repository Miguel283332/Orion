// orion: el agente. Normalmente lo arranca el guardián (guardian.exe), que lo reinicia si se cae.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "orion/evaluacion.hpp"
#include "orion/nucleo.hpp"

#ifdef _WIN32
#include <io.h>
#define ES_TERMINAL() _isatty(_fileno(stdout))
#else
#include <unistd.h>
#define ES_TERMINAL() isatty(1)
#endif

using namespace orion;

static void uso() {
    std::puts(
        "uso: orion [opciones]\n"
        "  --datos DIR      carpeta de memoria y configuración (por defecto: datos)\n"
        "  --modelo M       modelo de Ollama (por defecto el de datos/config.json, inicialmente qwen2.5:3b)\n"
        "  --ollama URL     dirección de Ollama (por defecto http://127.0.0.1:11434)\n"
        "  --repo DIR       carpeta con el código de Orion (para /versiones); por defecto, la actual si tiene .git\n"
        "  --sin-autonomia  no pensar a solas (solo responder)\n"
        "  --prompts DIR    carpeta con sus instrucciones (por defecto <repo>/prompts)\n"
        "  --evaluar ARCH   medirse con un banco de tareas y terminar (lo usa la automejora)\n"
        "  --web-puerto N   puerto de la interfaz web (0 = sin interfaz web)\n"
        "  --sin-color      sin colores en la terminal\n"
        "  --version");
}

int main(int argc, char** argv) {
    preparar_consola();
    OpcionesNucleo op;
    std::string modelo, ollama, evaluar;
    double temperatura = -1;
    int max_pasos = 0, num_ctx = 0, web_puerto = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto valor = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "falta el valor de %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--datos") op.datos = valor();
        else if (a == "--modelo") modelo = valor();
        else if (a == "--ollama") ollama = valor();
        else if (a == "--repo") op.repo = valor();
        else if (a == "--sin-autonomia") op.autonomo = false;
        else if (a == "--sin-color") op.color = false;
        else if (a == "--gracia") op.gracia_s = std::atof(valor().c_str());
        else if (a == "--prompts") op.prompts = valor();
        else if (a == "--evaluar") evaluar = valor();
        else if (a == "--temperatura") temperatura = std::atof(valor().c_str());
        else if (a == "--max-pasos") max_pasos = std::atoi(valor().c_str());
        else if (a == "--num-ctx") num_ctx = std::atoi(valor().c_str());
        else if (a == "--web-puerto") web_puerto = std::atoi(valor().c_str());
        else if (a == "--version") { std::puts("orion 0.3 (memoria por significado, aprendizaje, automejora medida, web y escalado)"); return 0; }
        else if (a == "-h" || a == "--help" || a == "--ayuda") { uso(); return 0; }
        else { std::fprintf(stderr, "opción desconocida: %s\n", a.c_str()); uso(); return 2; }
    }
    if (!ES_TERMINAL()) op.color = false;
    if (op.repo.empty() && fs::exists(".git") && fs::exists("CMakeLists.txt")) op.repo = fs::current_path();
    if (op.repo.empty())   // el ejecutable suele estar en <repo>/build/Release: buscar hacia arriba
        for (fs::path p = ruta_ejecutable_actual(argv[0]).parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
            if (fs::exists(p / "CMakeLists.txt") && fs::exists(p / "src" / "nucleo.cpp")) { op.repo = p; break; }
    if (op.prompts.empty() && !op.repo.empty() && fs::exists(op.repo / "prompts")) op.prompts = op.repo / "prompts";
    op.ejecutable = ruta_ejecutable_actual(argv[0]);
    fs::create_directories(op.datos);
    Config cfg = evaluar.empty() ? Config::cargar(op.datos / "config.json") : Config{};
    if (!modelo.empty()) cfg.modelo = modelo;
    if (!ollama.empty()) cfg.ollama = ollama;
    else if (const char* h = std::getenv("OLLAMA_HOST")) cfg.ollama = std::string(h).find("://") == std::string::npos ? "http://" + std::string(h) : h;
    if (temperatura >= 0) cfg.temperatura = temperatura;
    if (max_pasos > 0) cfg.max_pasos = max_pasos;
    if (num_ctx > 0) cfg.num_ctx = num_ctx;
    if (web_puerto >= 0) cfg.web_puerto = web_puerto;

    if (!evaluar.empty()) {   // modo medición: no es interactivo; imprime el resultado en una línea al final
        auto r = evaluar_en_proceso(cfg, evaluar, op.prompts, op.datos, [](const std::string& l) { std::puts(l.c_str()); std::fflush(stdout); });
        std::printf("RESULTADO_EVALUACION %s\n", r.a_json().dump().c_str());
        return r.ok ? 0 : 1;
    }

    // Ctrl+C: todo se guarda en el momento en que ocurre, así que se puede salir sin más.
    std::signal(SIGINT, [](int) { std::fputs("\nHasta luego.\n", stdout); std::fflush(stdout); std::_Exit(0); });

    op.terminar = [](int codigo) { std::fflush(stdout); std::_Exit(codigo); };   // reinicio pedido por ella misma
    Nucleo n(cfg, op, [](const std::string& t) { std::fwrite(t.data(), 1, t.size(), stdout); std::fflush(stdout); });
    n.arrancar();
    return n.ejecutar([](std::string& l) { return leer_linea(l); });
}
