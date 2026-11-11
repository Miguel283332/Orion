// demostrador: resuelve problemas matemáticos y físicos con un LLM local (Ollama) y un verificador externo.
#include <csignal>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#include "demo/engine.hpp"

using namespace demo;

#ifndef DEMO_VERSION
#define DEMO_VERSION "4.0.0"
#endif

static void al_senal(int) { bandera_parar() = true; }

static void uso() {
    std::cout <<
R"(demostrador - problemas matemáticos y físicos con LLM local (Ollama) y verificador externo

Uso:
  demostrador resolver --problema ARCHIVO.json   [opciones]
  demostrador resolver --enunciado "texto"       [opciones]
  demostrador estado --dir CARPETA

Opciones de resolver:
  --backend lean|sympy   --dir CARPETA   --modelo M   --ollama-url URL   --lean RUTA   --lake-dir DIR   --mathlib
  --horas H (límite de la sesión)   --max-intentos N (llamadas al modelo en la sesión)
  --directos N --finales N --replanes N --profundidad N   --num-ctx N --num-predict N --timeout-lean S --semilla N
  --sin-fidelidad   --reiniciar
Códigos de salida: 0 verificado, 1 no resuelto, 3 pausa por presupuesto, 4 Ollama caído, 130 interrumpido.
)";
}

struct Args {
    std::string cmd, problema, enunciado, backend, dir, modelo = "qwen2.5:7b", ollama_url, lean, lake_dir;
    bool mathlib = false, sin_fidelidad = false, reiniciar = false;
    double horas = 0;
    int max_intentos = 0, directos = 6, finales = 6, replanes = 3, profundidad = 2, num_ctx = 8192, num_predict = 1600, timeout_lean = 90, semilla = 1;
};

static Args parsear(int argc, char** argv) {
    Args a;
    if (argc < 2) { uso(); std::exit(2); }
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto sig = [&]() -> std::string { if (i + 1 >= argc) { std::cerr << "falta valor para " << s << "\n"; std::exit(2); } return argv[++i]; };
        if (s == "-h" || s == "--help") { uso(); std::exit(0); }
        else if (s == "--problema") a.problema = sig();
        else if (s == "--enunciado") a.enunciado = sig();
        else if (s == "--backend") a.backend = sig();
        else if (s == "--dir") a.dir = sig();
        else if (s == "--modelo") a.modelo = sig();
        else if (s == "--ollama-url") a.ollama_url = sig();
        else if (s == "--lean") a.lean = sig();
        else if (s == "--lake-dir") a.lake_dir = sig();
        else if (s == "--mathlib") a.mathlib = true;
        else if (s == "--sin-fidelidad") a.sin_fidelidad = true;
        else if (s == "--reiniciar") a.reiniciar = true;
        else if (s == "--horas") a.horas = std::stod(sig());
        else if (s == "--max-intentos") a.max_intentos = std::stoi(sig());
        else if (s == "--directos") a.directos = std::stoi(sig());
        else if (s == "--finales") a.finales = std::stoi(sig());
        else if (s == "--replanes") a.replanes = std::stoi(sig());
        else if (s == "--profundidad") a.profundidad = std::stoi(sig());
        else if (s == "--num-ctx") a.num_ctx = std::stoi(sig());
        else if (s == "--num-predict") a.num_predict = std::stoi(sig());
        else if (s == "--timeout-lean") a.timeout_lean = std::stoi(sig());
        else if (s == "--semilla") a.semilla = std::stoi(sig());
        else if (!s.empty() && s[0] != '-' && a.cmd.empty()) a.cmd = s;
        else { std::cerr << "opción desconocida: " << s << "\n"; uso(); std::exit(2); }
    }
    return a;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::signal(SIGINT, al_senal);
    std::signal(SIGTERM, al_senal);
    Args a = parsear(argc, argv);
    try {
        if (a.cmd == "estado") {
            if (a.dir.empty()) { std::cerr << "falta --dir\n"; return 2; }
            std::cout << informe(a.dir) << std::endl;
            return 0;
        }
        if (a.cmd != "resolver") { uso(); return 2; }
        json p;
        if (!a.problema.empty()) {
            p = json::parse(leer_archivo(a.problema), nullptr, false);
            if (p.is_discarded() || !p.is_object() || !p.contains("enunciado")) { std::cerr << "El archivo del problema necesita el campo 'enunciado'.\n"; return 2; }
            if (!p.contains("id")) p["id"] = fs::path(a.problema).stem().string();
        } else if (!a.enunciado.empty()) {
            p = {{"id", ""}, {"enunciado", a.enunciado}};
        } else {
            std::cerr << "Indica --problema ARCHIVO.json o --enunciado \"texto\".\n";
            return 2;
        }
        p["enunciado"] = trim(dedent(p["enunciado"].get<std::string>()));
        p["backend"] = !a.backend.empty() ? a.backend : p.value("backend", std::string("lean"));
        if (!p.contains("dominio")) p["dominio"] = "matematicas";
        if (p["id"].get<std::string>().empty()) {
            std::string id, e = p["enunciado"].get<std::string>().substr(0, 30);
            for (unsigned char c : e) id.push_back(std::isalnum(c) || c >= 0x80 ? (char)std::tolower(c) : '_');
            while (!id.empty() && id.front() == '_') id.erase(id.begin());
            while (!id.empty() && id.back() == '_') id.pop_back();
            p["id"] = id.empty() ? "problema" : id;
        }
        if (p["backend"] != "lean" && p["backend"] != "sympy") { std::cerr << "backend desconocido: " << p["backend"].get<std::string>() << " (usa lean o sympy)\n"; return 2; }

        Config cfg;
        cfg.modelo = a.modelo; cfg.ollama_url = a.ollama_url.empty() ? env_o("OLLAMA_HOST", "http://127.0.0.1:11434") : a.ollama_url;
        cfg.backend = p["backend"]; cfg.lean = a.lean; cfg.lake_dir = a.lake_dir; cfg.mathlib = a.mathlib;
        cfg.horas = a.horas; cfg.max_intentos = a.max_intentos; cfg.directos = a.directos; cfg.finales = a.finales; cfg.replanes = a.replanes;
        cfg.profundidad = a.profundidad; cfg.num_ctx = a.num_ctx; cfg.num_predict = a.num_predict; cfg.fidelidad = !a.sin_fidelidad;
        cfg.semilla = a.semilla; cfg.timeout_lean = a.timeout_lean;
        std::unique_ptr<Verificador> ver;
        if (cfg.backend == "lean") ver = std::make_unique<LeanVerificador>(cfg);
        else ver = std::make_unique<SympyVerificador>(cfg);
        fs::path base = !a.dir.empty() ? fs::path(a.dir) : fs::path("demostrador_runs") / (p["id"].get<std::string>() + "_" + huella(p["enunciado"].get<std::string>() + p["backend"].get<std::string>()).substr(0, 8));
        OllamaLlm llm(cfg);
        Motor motor(cfg, llm, *ver, base);
        std::cout << "Directorio de trabajo (estado y resultado): " << base.string() << std::endl;
        return motor.ejecutar(p, a.reiniciar);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 2;
    }
}
