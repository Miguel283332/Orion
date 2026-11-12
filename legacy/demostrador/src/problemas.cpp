#include "demo/problemas.hpp"

namespace demo {

const json& problemas_incluidos() {
    static const json p = {
        {"par_mayor", {{"id", R"P(par_mayor)P"}, {"dominio", R"P(matematicas)P"}, {"backend", R"P(lean)P"}, {"enunciado", R"P(Demuestra la existencia de un número natural m mayor que cualquier número natural n dado, tal que m sea además un número par.)P"}}},
        {"escape_schwarzschild", {{"id", R"P(escape_schwarzschild)P"}, {"dominio", R"P(fisica)P"}, {"backend", R"P(sympy)P"}, {"enunciado", R"P(Una masa m escapa de la atracción de una masa M desde una distancia r, justo con energía mecánica total cero (energía cinética + energía potencial gravitatoria -G M m / r). Deduce la velocidad de escape v_esc(r) y demuestra que a la distancia r_s = 2 G M / c^2 (el radio de Schwarzschild) la velocidad de escape vale exactamente c.)P"}, {"especificacion", {{R"P(descripcion)P", R"P(Deducir la velocidad de escape v_esc(r) con energía mecánica total cero y comprobar que en r_s = 2GM/c^2 vale c.)P"}, {R"P(preambulo)P", R"P(G, M, c, r, m, v = symbols('G M c r m v', positive=True)
r_s = 2*G*M/c**2)P"}, {R"P(variables)P", json::array({R"P(v_esc)P"})}, {R"P(criterios)P", json::array({R"P(simplify(v_esc**2 - 2*G*M/r) == 0)P", R"P(simplify(v_esc.subs(r, r_s) - c) == 0)P", R"P(v_esc.is_positive == True)P"})}}}}}};
    return p;
}

std::vector<std::string> nombres_problemas_incluidos() {
    std::vector<std::string> v;
    for (auto& [k, x] : problemas_incluidos().items()) v.push_back(k);
    return v;
}

std::optional<json> problema_incluido(const std::string& nombre) {
    std::string clave = nombre;
    if (clave.size() > 5 && clave.substr(clave.size() - 5) == ".json") clave = fs::path(clave).stem().string();
    if (clave == "facil_par_mayor") clave = "par_mayor";
    if (clave == "facil_velocidad_escape_schwarzschild") clave = "escape_schwarzschild";
    if (problemas_incluidos().contains(clave)) return problemas_incluidos()[clave];
    return std::nullopt;
}

}  // namespace demo
