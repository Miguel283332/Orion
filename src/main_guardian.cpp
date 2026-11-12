// guardian: arranca Orion y la vuelve a arrancar si se cae o si pide reiniciarse (/reinicia, código 75).
// Si se cae 3 veces en 2 minutos, se detiene para no entrar en un bucle. Registra todo en datos/guardian.log.
// Uso: guardian [--orion RUTA] [opciones de orion...]
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <thread>

#include "orion/nucleo.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace orion;

int main(int argc, char** argv) {
    preparar_consola();
#ifdef _WIN32
    SetConsoleCtrlHandler(nullptr, TRUE);   // Ctrl+C lo gestiona Orion; el guardián espera a que termine
#else
    std::signal(SIGINT, SIG_IGN);
#endif
    fs::path exe = ruta_ejecutable_actual(argc > 0 ? argv[0] : nullptr);
#ifdef _WIN32
    fs::path orion = exe.parent_path() / "orion.exe";
#else
    fs::path orion = exe.parent_path() / "orion";
#endif
    std::vector<std::string> args_orion;
    fs::path datos = "datos";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--orion" && i + 1 < argc) { orion = argv[++i]; continue; }
        if (a == "--datos" && i + 1 < argc) datos = argv[i + 1];
        args_orion.push_back(a);
    }
    if (!fs::exists(orion)) {
        std::fprintf(stderr, "No encuentro %s. Compila primero (cmake --build build) o usa --orion RUTA.\n", orion.string().c_str());
        return 1;
    }
    auto registrar = [&](const std::string& m) {
        try { anadir_linea(datos / "guardian.log", fecha_hora() + "  " + m); } catch (...) {}
    };
    std::deque<double> caidas;
    for (;;) {
        std::vector<std::string> cmd = {orion.string()};
        cmd.insert(cmd.end(), args_orion.begin(), args_orion.end());
        registrar("arranco " + orion.string());
        int codigo = ejecutar_en_consola(cmd);
        if (codigo == 0) { registrar("Orion terminó normalmente"); return 0; }
        if (codigo == CODIGO_REINICIO) {
            registrar("Orion pidió reiniciarse");
            std::puts("[guardián] Reiniciando Orion…");
            continue;
        }
        double t = ahora_s();
        caidas.push_back(t);
        while (!caidas.empty() && t - caidas.front() > 120) caidas.pop_front();
        registrar("Orion se cayó (código " + std::to_string(codigo) + ")");
        if (caidas.size() >= 3) {
            std::fprintf(stderr, "[guardián] Orion se ha caído 3 veces en 2 minutos (último código %d). Me detengo; mira %s\n", codigo,
                         (datos / "guardian.log").string().c_str());
            registrar("demasiadas caídas seguidas: me detengo");
            return 1;
        }
        std::fprintf(stderr, "[guardián] Orion se cayó (código %d). La vuelvo a arrancar en 3 s…\n", codigo);
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}
