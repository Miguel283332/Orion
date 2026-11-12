// Memoria persistente de Orion, en archivos de texto legibles (y modificables por ella misma) dentro de datos/:
//   conversacion.jsonl  memoria episódica: cada turno de conversación
//   recuerdos.jsonl     memoria semántica: hechos duraderos (usuario, proyectos, preferencias, sobre sí misma...)
//   diario.md           autobiografía: lo que hizo y aprendió
//   yo.md               su modelo de sí misma (lo reescribe ella)
//   estadisticas.json   contadores acumulados (llamadas, tokens, tiempo encendida...)
// La búsqueda es por palabras (BM25). En la fase 2 se añade búsqueda por significado con el mismo modelo.
#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "orion/third_party_json.hpp"
#include "orion/util.hpp"

namespace orion {

struct Turno {
    double t = 0;
    std::string rol;     // "usuario" | "orion"
    std::string texto;
};

struct Recuerdo {
    std::string id;      // "r12"
    double t = 0;
    std::string tipo;    // usuario | proyecto | preferencia | yo | mundo | manual
    std::string texto;
    std::string origen;  // reflexion | manual | ...
};

struct Encontrado {
    double puntuacion = 0;
    std::string id;      // id del recuerdo o "t<n>" para un turno de conversación
    std::string texto;
    std::string tipo;
};

class Memoria {
public:
    explicit Memoria(fs::path dir);
    void cargar();
    const fs::path& dir() const { return dir_; }

    // episódica
    void anadir_turno(const std::string& rol, const std::string& texto);
    std::vector<Turno> turnos_desde(size_t indice) const;
    std::vector<Turno> ultimos_turnos(size_t n) const;
    size_t num_turnos() const;

    // semántica
    std::string recordar(const std::string& tipo, const std::string& texto, const std::string& origen);   // "" si ya lo sabía
    bool olvidar(const std::string& id);
    std::vector<Recuerdo> recuerdos() const;
    std::vector<Encontrado> buscar(const std::string& consulta, size_t k, bool incluir_conversacion = false) const;

    // diario
    void escribir_diario(const std::string& texto);
    std::string diario(size_t ultimas_entradas) const;
    size_t entradas_diario() const;

    // modelo de sí misma
    std::string yo() const;
    void escribir_yo(const std::string& texto);   // guarda la versión anterior en yo_versiones/

    // estadísticas acumuladas (persisten entre arranques)
    json estadisticas() const;
    void sumar(const std::string& clave, double valor);
    void fijar(const std::string& clave, const json& valor);

private:
    void guardar_recuerdos();
    void guardar_estadisticas();
    fs::path dir_;
    mutable std::mutex mu_;
    std::vector<Turno> turnos_;
    std::vector<Recuerdo> recuerdos_;
    json stats_ = json::object();
    long long siguiente_id_ = 1;
};

std::string yo_inicial();

}  // namespace orion
