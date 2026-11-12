// Habilidades: lo que Orion ya sabe hacer, guardado como scripts que se pueden volver a usar (datos/habilidades/).
// Una habilidad solo se guarda si su prueba funciona (código de salida 0). Se cuentan sus usos y sus éxitos.
#pragma once
#include <string>
#include <vector>

#include "orion/sistema.hpp"
#include "orion/third_party_json.hpp"

namespace orion {

struct Habilidad {
    std::string nombre;        // [a-z0-9_]
    std::string descripcion;   // qué hace y cómo se usa (argumentos)
    std::string lenguaje;      // powershell | cmd | sh
    std::string codigo;
    std::string prueba;        // argumentos con los que se probó
    int usos = 0, exitos = 0;
    double creada = 0;
    json a_json() const;
    static Habilidad de_json(const json& j);
};

class Habilidades {
public:
    explicit Habilidades(fs::path dir);   // datos/habilidades
    // Prueba la habilidad y solo la guarda si funciona. Devuelve el texto para el modelo.
    std::string guardar(const Habilidad& h, bool probar = true);
    std::vector<Habilidad> todas() const;
    bool obtener(const std::string& nombre, Habilidad& h) const;
    std::vector<Habilidad> relevantes(const std::string& consulta, size_t k) const;
    ResultadoProceso ejecutar(const Habilidad& h, const std::string& argumentos, int timeout_s = 120) const;
    std::string usar(const std::string& nombre, const std::string& argumentos, int timeout_s = 120);
    bool borrar(const std::string& nombre);
    static std::string lenguaje_por_defecto();
    static bool nombre_valido(const std::string& n);
private:
    fs::path archivo_script(const Habilidad& h) const;
    fs::path dir_;
};

}  // namespace orion
