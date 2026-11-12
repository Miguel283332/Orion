// orion: el agente. Normalmente lo arranca el guardián (guardian.exe), que lo reinicia si se cae.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>

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
        "  --sin-color      sin colores en la terminal\n"
        "  --version");
}

int main(int argc, char** argv) {
    preparar_consola();
    OpcionesNucleo op;
    std::string modelo, ollama;
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
        else if (a == "--version") { std::puts("orion 0.2 (herramientas, metas y automejora)"); return 0; }
        else if (a == "-h" || a == "--help" || a == "--ayuda") { uso(); return 0; }
        else { std::fprintf(stderr, "opción desconocida: %s\n", a.c_str()); uso(); return 2; }
    }
    if (!ES_TERMINAL()) op.color = false;
    if (op.repo.empty() && fs::exists(".git") && fs::exists("CMakeLists.txt")) op.repo = fs::current_path();
    if (op.repo.empty())   // el ejecutable suele estar en <repo>/build/Release: buscar hacia arriba
        for (fs::path p = ruta_ejecutable_actual(argv[0]).parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
            if (fs::exists(p / "CMakeLists.txt") && fs::exists(p / "src" / "nucleo.cpp")) { op.repo = p; break; }
    fs::create_directories(op.datos);
    Config cfg = Config::cargar(op.datos / "config.json");
    if (!modelo.empty()) cfg.modelo = modelo;
    if (!ollama.empty()) cfg.ollama = ollama;
    else if (const char* h = std::getenv("OLLAMA_HOST")) cfg.ollama = std::string(h).find("://") == std::string::npos ? "http://" + std::string(h) : h;

    // Ctrl+C: todo se guarda en el momento en que ocurre, así que se puede salir sin más.
    std::signal(SIGINT, [](int) { std::fputs("\nHasta luego.\n", stdout); std::fflush(stdout); std::_Exit(0); });

    op.terminar = [](int codigo) { std::fflush(stdout); std::_Exit(codigo); };   // reinicio pedido por ella misma
    Nucleo n(cfg, op, [](const std::string& t) { std::fwrite(t.data(), 1, t.size(), stdout); std::fflush(stdout); });
    n.arrancar();
    return n.ejecutar([](std::string& l) { return leer_linea(l); });
}
