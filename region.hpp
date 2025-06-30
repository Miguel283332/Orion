#ifndef REGION_HPP
#define REGION_HPP

#include "neurona.hpp"
#include <vector>
#include <string>

class RegionCerebral {
public:
    std::string nombre;
    std::vector<Neurona> neuronas;

    bool cargarDesdeArchivo(const std::string& archivo);
    bool guardarEnArchivo(const std::string& archivo) const;
};

#endif

