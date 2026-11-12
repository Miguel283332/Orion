// Herramientas: las "manos" de Orion. El modelo decide cuándo usarlas (llamadas a herramientas nativas de Ollama).
// Acceso total por defecto (decisión de su creador): ejecutar cualquier comando, leer y escribir cualquier archivo,
// internet, su memoria, su código y compilarse a sí misma. Cada acción se muestra en la terminal y se anota en
// datos/acciones.jsonl. Los permisos se pueden bajar con /permisos (lectura | ninguno).
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "orion/memoria.hpp"

namespace orion {

struct ContextoHerramientas {
    fs::path datos;
    fs::path repo;                                   // su código fuente (vacío si no se conoce)
    Memoria* memoria = nullptr;
    std::string permisos = "total";                  // total | lectura | ninguno
    std::string origen = "chat";                     // chat | meta (para el registro)
    std::function<void(const std::string&)> aviso;   // mostrar en la terminal lo que hace
    std::atomic<bool>* reinicio_pedido = nullptr;
    std::string cmake = "cmake";
};

class Herramientas {
public:
    explicit Herramientas(ContextoHerramientas c);
    json definiciones() const;                                   // formato "tools" de Ollama, según los permisos
    std::string usar(const std::string& nombre, const json& argumentos);   // texto para el modelo
    static std::vector<std::string> nombres(const std::string& permisos);

    // utilidades públicas (para probarlas)
    static std::string html_a_texto(const std::string& html);
    static std::vector<std::pair<std::string, std::string>> resultados_busqueda(const std::string& html);   // (url, título + resumen)
    static std::string codificar_url(const std::string& s);
    static std::string decodificar_url(const std::string& s);
    static fs::path ejecutable_compilado(const fs::path& dir_build);

private:
    std::string usar_sin_registro(const std::string& nombre, const json& a);
    ContextoHerramientas c_;
};

}  // namespace orion
