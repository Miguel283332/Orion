// Utilidades comunes: texto, tiempo, archivos y consola.
#pragma once
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace orion {
namespace fs = std::filesystem;

// ---- texto
std::string trim(const std::string& s);
std::vector<std::string> split_lines(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& v, const std::string& sep);
bool empieza_por(const std::string& s, const std::string& prefijo);
bool contiene(const std::string& s, const std::string& sub);
std::string recortar(const std::string& s, size_t max);        // corta sin romper UTF-8 y añade "…"
std::string minusculas_sin_acentos(const std::string& s);       // para buscar: "Canción" -> "cancion"
std::vector<std::string> palabras(const std::string& s);        // tokens normalizados de 2+ caracteres
std::string formato_bytes(double bytes);                        // 1536 -> "1.5 KB"
std::string formato_numero(double n);                           // 3085938688 -> "3.09 mil millones"
std::string formato_duracion(double segundos);                  // 3725 -> "1 h 2 min"

std::string sha256_hex(const std::string& datos);              // huella SHA-256 en hexadecimal

// ---- tiempo
double ahora_s();                                               // reloj de pared, segundos desde la época
std::string fecha_hora();                                       // "2026-11-12 14:03"

// ---- archivos
std::string leer_archivo(const fs::path& p);                    // "" si no existe
void escribir_atomico(const fs::path& p, const std::string& contenido);
void anadir_linea(const fs::path& p, const std::string& linea); // append + salto de línea
double tamano_directorio(const fs::path& p);

// ---- consola
void preparar_consola();                                         // UTF-8 en Windows
bool leer_linea(std::string& linea);                             // stdin; en la consola de Windows lee UTF-16 y convierte

}  // namespace orion
