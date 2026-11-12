// Utilidades: cadenas, ficheros, SHA-256, procesos con tiempo límite, extracción de JSON y de código.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace demo {
namespace fs = std::filesystem;
using json = nlohmann::json;

std::string ahora();
std::string trim(const std::string& s);
std::string lower(std::string s);
bool starts_with(const std::string& s, const std::string& p);
bool contains(const std::string& s, const std::string& sub);
std::vector<std::string> split_lines(const std::string& s);   // separa por '\n' (sin perder líneas vacías)
std::string join(const std::vector<std::string>& v, const std::string& sep);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
std::string recortar(const std::string& s, size_t n);
std::string dedent(const std::string& s);
std::string sangrar(const std::vector<std::string>& lineas, int n = 2);
std::string ensamblar(const std::vector<std::string>& partes);   // une las no vacías con una línea en blanco

std::string normalizar(const std::string& s);
std::string huella(const std::string& s);                      // sha256(normalizar) -> 16 hex
std::string sha256_hex(const std::string& data);

std::string leer_archivo(const fs::path& p);
void escribir_archivo(const fs::path& p, const std::string& data, bool anexar = false);
void escribir_atomico(const fs::path& p, const std::string& data);
fs::path crear_temporal(const std::string& prefijo);
std::string env_o(const char* nombre, const std::string& defecto = "");

// ---- extracción
std::optional<json> extraer_json(const std::string& texto);          // objeto JSON tolerante (``` y texto alrededor)
std::vector<std::string> bloques_codigo(const std::string& texto, const std::vector<std::string>& lenguajes);
std::string elegir_bloque(const std::string& texto, const std::vector<std::string>& lenguajes,
                          const std::function<bool(const std::string&)>& pista);
std::string quitar_comentarios_lean(const std::string& c);
std::string quitar_comentarios_py(const std::string& c);

// ---- procesos
std::atomic<bool>& bandera_parar();    // la ponen SIGINT/SIGTERM; los procesos en curso se cancelan
struct ResultadoProceso {
    int codigo = 0;
    bool tiempo_agotado = false, interrumpido = false, no_ejecutado = false;
    int senal = 0;                     // != 0 si el sistema lo terminó (POSIX)
    std::string salida;                // stdout + stderr
};
struct OpcionesProceso {
    fs::path cwd;
    double timeout_s = 60;
    bool limites_py = false;           // RLIMIT de CPU y memoria (POSIX)
    bool limpiar_secretos = false;     // quitar variables con KEY/TOKEN/SECRET/PASS/CRED/AUTH
    double cpu_s = 0;
};
ResultadoProceso ejecutar(const std::vector<std::string>& argv, const OpcionesProceso& op);
void dormir_ms(int ms);

}  // namespace demo
