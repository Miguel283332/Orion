// Evaluación: un conjunto de tareas con comprobación objetiva que mide lo bien que funciona Orion.
//   evaluacion/banco.json             su banco de pruebas (lo puede cambiar ella)
//   evaluacion/examen_referencia.json el examen de referencia: no se usa para elegir cambios, solo para confirmar
//                                     que no empeora. Su huella SHA-256 se anota la primera vez: si cambia, se avisa.
// Cada tarea se ejecuta con el agente completo (herramientas incluidas) en una carpeta temporal propia.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "orion/mente.hpp"

namespace orion {

struct TareaEvaluacion {
    std::string id, pregunta;
    json archivos = json::object();   // archivos que se crean antes en la carpeta de la tarea: {"a.txt": "contenido"}
    json comprobar;                   // {"tipo": "contiene|alguno|no_contiene|numero|archivo|json", ...}
};

struct ResultadoEvaluacion {
    bool ok = false;                  // la evaluación se pudo hacer
    std::string error;
    int aprobadas = 0, total = 0;
    double segundos = 0;
    long long tokens = 0;
    json detalles = json::array();
    double puntuacion() const { return total ? static_cast<double>(aprobadas) / total : 0; }
    json a_json() const;
    static ResultadoEvaluacion de_json(const json& j);
};

std::vector<TareaEvaluacion> cargar_tareas(const fs::path& archivo, std::string* error = nullptr);
// ¿La respuesta (y los archivos que dejó en dir) cumple la comprobación?
bool comprobar_tarea(const TareaEvaluacion& t, const std::string& respuesta, const fs::path& dir, std::string* motivo = nullptr);
// Ejecuta las tareas EN ESTE PROCESO (lo usa `orion --evaluar`). Cambia el directorio de trabajo durante cada tarea.
ResultadoEvaluacion evaluar_en_proceso(const Config& cfg, const fs::path& tareas, const fs::path& dir_prompts, const fs::path& dir_trabajo,
                                       const std::function<void(const std::string&)>& progreso = nullptr);
// Ejecuta la evaluación en un proceso aparte (otro ejecutable o unas instrucciones distintas) y lee su resultado.
ResultadoEvaluacion evaluar_con_proceso(const fs::path& ejecutable, const fs::path& tareas, const fs::path& dir_prompts, const Config& cfg,
                                        const fs::path& dir_trabajo, int timeout_s = 7200, const std::atomic<bool>* cancelar = nullptr);

}  // namespace orion
