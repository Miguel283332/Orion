// Sistema: medir el hardware (para que Orion conozca sus límites) y ejecutar procesos (git, guardián, herramientas).
#pragma once
#include <string>
#include <vector>

#include "orion/util.hpp"

namespace orion {

struct Hardware {
    std::string so;            // "Windows 11" / "Linux 6.x"
    std::string cpu;           // nombre del procesador
    int hilos = 0;             // hilos lógicos
    double ram_total = 0, ram_libre = 0;   // bytes
    double ram_proceso = 0;    // memoria usada por Orion (sin el modelo, que vive en Ollama)
    double disco_libre = 0;    // en la unidad de datos/
};
Hardware medir_hardware(const fs::path& dir_datos);

struct ResultadoProceso {
    int codigo = -1;           // -1: no se pudo ejecutar
    std::string salida;        // stdout + stderr
    bool tiempo_agotado = false;
};
// Ejecuta y captura la salida. args[0] se busca en el PATH.
// entrada: texto que recibe por stdin (vacío = nada).
ResultadoProceso ejecutar_proceso(const std::vector<std::string>& args, const fs::path& directorio = {}, int timeout_s = 60,
                                  const std::string& entrada = "");
// Ejecuta compartiendo la consola (para el guardián). Devuelve el código de salida.
int ejecutar_en_consola(const std::vector<std::string>& args);
fs::path ruta_ejecutable_actual(const char* argv0);

}  // namespace orion
