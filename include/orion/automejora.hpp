// Automejora MEDIDA (fase 4).
// Ciclo: medir la versión actual con su banco de pruebas → proponer UN cambio en sus instrucciones (prompts/sistema.md)
// o en sus parámetros (temperatura, herramientas por turno) → medir la propuesta con el banco → si mejora, confirmar con
// el examen de referencia que no empeora → aceptar (y commit en git) o descartar. Todo queda en datos/mejoras.jsonl.
// Las mediciones se hacen en un proceso aparte (`orion --evaluar`), con la misma semilla, para que sean comparables.
#pragma once
#include <atomic>
#include <functional>
#include <string>

#include "orion/evaluacion.hpp"

namespace orion {

struct ContextoAutomejora {
    fs::path repo;         // repositorio (prompts/ y evaluacion/ están aquí)
    fs::path datos;        // datos/ (mejoras.jsonl, huella del examen, caché de mediciones)
    fs::path ejecutable;   // el orion que se mide
};

struct ResultadoMejora {
    bool hecho = false;          // se completó un ciclo (aceptado o no)
    bool aceptada = false;
    bool cancelada = false;
    std::string texto;           // resumen para la terminal
    double temperatura = -1;     // parámetros nuevos si se aceptaron (-1 / 0 = sin cambio)
    int max_pasos = 0;
};

class Automejora {
public:
    Automejora(ContextoAutomejora c, Ollama& llm);
    std::string disponible() const;   // "" si se puede; si no, el motivo
    ResultadoMejora ciclo(const Config& cfg, const std::atomic<bool>* cancelar, const std::function<void(const std::string&)>& aviso);
    ResultadoEvaluacion medir(const Config& cfg, const fs::path& tareas, const fs::path& prompts, const std::atomic<bool>* cancelar);
    json historial() const;                  // lista de mejoras (datos/mejoras.jsonl)
    ResultadoMejora volver(int n, const Config& cfg);   // deshacer la mejora aceptada número n
    std::string aviso_examen() const;        // "" si el examen de referencia no ha cambiado
    fs::path banco() const { return c_.repo / "evaluacion" / "banco.json"; }
    fs::path examen() const { return c_.repo / "evaluacion" / "examen_referencia.json"; }
    fs::path prompts() const { return c_.repo / "prompts"; }
private:
    std::string clave_medicion(const Config& cfg, const fs::path& tareas, const fs::path& prompts) const;
    void registrar(json entrada) const;
    std::string commit(const std::string& mensaje) const;
    ContextoAutomejora c_;
    Ollama& llm_;
};

}  // namespace orion
