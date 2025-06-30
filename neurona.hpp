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

    // Diverge forma funcional a partir de sus propiedades
    std::string formaDivergente() const;
};

#endif

