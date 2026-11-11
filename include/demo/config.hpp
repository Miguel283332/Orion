// Configuración y excepciones comunes.
#pragma once
#include <stdexcept>
#include <string>

namespace demo {

struct Config {
    std::string modelo = "qwen2.5:7b";
    std::string ollama_url = "http://127.0.0.1:11434";
    int num_ctx = 8192, num_predict = 1600;
    std::string backend = "lean";
    std::string lean, lake_dir;      // ruta a lean; proyecto lake con Mathlib (usa `lake env lean`)
    bool mathlib = false;
    int timeout_lean = 90, timeout_py = 120;
    int formalizar = 8, directos = 6, finales = 6, planes = 4, replanes = 3, max_lemas = 4, profundidad = 2;
    bool fidelidad = true;
    double horas = 0.0;              // 0 = sin límite (por sesión)
    int max_intentos = 0;            // 0 = sin límite de llamadas al modelo (por sesión)
    int semilla = 1;
};

struct Pausa : std::runtime_error { using std::runtime_error::runtime_error; };        // presupuesto o parada: el estado está guardado
struct ErrorModelo : std::runtime_error { using std::runtime_error::runtime_error; };  // Ollama no responde
struct ErrorValor : std::runtime_error { using std::runtime_error::runtime_error; };   // respuesta del modelo inválida (se reintenta)

}  // namespace demo
