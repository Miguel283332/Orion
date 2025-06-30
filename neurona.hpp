#ifndef NEURONA_HPP
#define NEURONA_HPP

#include <string>
#include <vector>
#include <sstream>

enum class Neurotransmisor {
    DOPAMINA, SEROTONINA, GABA, GLUTAMATO, ACETILCOLINA, DESCONOCIDO
};

enum class Receptor {
    D1, D2, NMDA, AMPA, GABA_A, DESCONOCIDO
};

// Conversores básicos (para archivos)
Neurotransmisor parseNT(const std::string& s);
std::string toString(Neurotransmisor nt);
Receptor parseReceptor(const std::string& s);
std::string toString(Receptor r);

struct Conexion {
    int destinoID;
    Neurotransmisor tipoNT;
};

struct Neurona {
    int id;
    float x, y, z;
    Neurotransmisor nt;
    std::vector<Receptor> receptores;
    std::vector<Conexion> conexiones;
    bool estado = false;

    // Constructor por defecto para garantizar valores iniciales conocidos
    Neurona() : id(0), x(0.0f), y(0.0f), z(0.0f),
                nt(Neurotransmisor::DESCONOCIDO), estado(false) {}

    // Diverge forma funcional a partir de sus propiedades
    std::string formaDivergente() const;
};

#endif

