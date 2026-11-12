// Problemas de ejemplo incluidos en el programa (no hace falta ningún archivo): --problema par_mayor
#pragma once
#include "demo/util.hpp"

namespace demo {

// Busca por nombre (par_mayor) o por la ruta de su archivo (problemas/facil_par_mayor.json).
std::optional<json> problema_incluido(const std::string& nombre);
std::vector<std::string> nombres_problemas_incluidos();
const json& problemas_incluidos();

}  // namespace demo
