// Verificadores externos: el modelo propone, el verificador decide.
#pragma once
#include <utility>

#include "demo/config.hpp"
#include "demo/util.hpp"

namespace demo {

class Verificador {
public:
    virtual ~Verificador() = default;
    std::string nombre, extension, sistema;
    bool admite_plan = false;

    virtual std::string entorno() = 0;
    virtual std::pair<bool, std::string> comprobar(const std::string& codigo, const std::string& nombre_final = "", bool permitir_sorry = false) = 0;
    virtual std::optional<std::string> prohibido(const std::string& texto) = 0;
    virtual std::string extraer_cuerpo(const std::string& texto) = 0;
    virtual std::string bloque(const std::string& firma, const std::string& cuerpo) = 0;
    virtual std::string codigo(const json& estado, const std::vector<std::string>& bloques, const std::string& firma = "", const std::string& cuerpo = "") = 0;
    virtual std::optional<std::string> auto_(const json& estado, const std::string& firma, const std::vector<std::string>& bloques) = 0;
    virtual std::string resumir_error(const std::string& salida) = 0;
    virtual std::string titulo_error(const std::string& resumen);
    virtual std::optional<std::string> enunciado_trivial(const std::string&) { return std::nullopt; }
    virtual std::string prompt_formalizar(const std::string& problema, const std::string& error) = 0;
    virtual std::string prompt_fidelidad(const std::string& problema, const std::string& defs, const std::string& firma) = 0;
    virtual std::string prompt_prueba(const std::string& firma, const std::vector<std::string>& bloques, const json& estado, const json& nodo, const std::string& tipo) = 0;
    virtual std::string prompt_plan(const std::string&, const std::vector<std::string>&, const json&, const json&, int, const std::vector<std::string>&) { return ""; }
};

class LeanVerificador : public Verificador {
public:
    explicit LeanVerificador(const Config& cfg);
    std::string entorno() override;
    std::pair<bool, std::string> comprobar(const std::string& codigo, const std::string& nombre_final = "", bool permitir_sorry = false) override;
    std::optional<std::string> prohibido(const std::string& texto) override;
    std::string extraer_cuerpo(const std::string& texto) override;
    std::string bloque(const std::string& firma, const std::string& cuerpo) override;
    std::string codigo(const json& estado, const std::vector<std::string>& bloques, const std::string& firma = "", const std::string& cuerpo = "") override;
    std::optional<std::string> auto_(const json& estado, const std::string& firma, const std::vector<std::string>& bloques) override;
    std::string resumir_error(const std::string& salida) override;
    std::string titulo_error(const std::string& resumen) override;
    std::optional<std::string> enunciado_trivial(const std::string& firma) override;
    std::string prompt_formalizar(const std::string& problema, const std::string& error) override;
    std::string prompt_fidelidad(const std::string& problema, const std::string& defs, const std::string& firma) override;
    std::string prompt_prueba(const std::string& firma, const std::vector<std::string>& bloques, const json& estado, const json& nodo, const std::string& tipo) override;
    std::string prompt_plan(const std::string& firma, const std::vector<std::string>& bloques, const json& estado, const json& nodo, int max_lemas, const std::vector<std::string>& fallidos) override;

    std::string extraer_firma(const std::string& texto);
    std::string nombre_de(const std::string& firma);
    std::string renombrar(const std::string& firma, const std::string& nuevo);
    std::string clave(const std::string& firma);
    std::vector<std::string> tacticas_auto() const;
    std::string estrategia(const std::string& firma);
    std::vector<std::string> testigos_auto(const std::string& firma);
    const std::string& encabezado() const { return encabezado_; }

private:
    std::string codigo_lean(const std::string& texto);
    std::vector<std::string> comando(const fs::path& archivo) const;
    Config cfg_;
    bool mathlib_;
    std::string lean_bin_, encabezado_;
};

class SympyVerificador : public Verificador {
public:
    explicit SympyVerificador(const Config& cfg);
    std::string entorno() override;
    std::pair<bool, std::string> comprobar(const std::string& codigo, const std::string& nombre_final = "", bool permitir_sorry = false) override;
    std::optional<std::string> prohibido(const std::string& texto) override;
    std::string extraer_cuerpo(const std::string& texto) override;
    std::string bloque(const std::string&, const std::string& cuerpo) override { return cuerpo; }
    std::string codigo(const json& estado, const std::vector<std::string>& bloques, const std::string& firma = "", const std::string& cuerpo = "") override;
    std::optional<std::string> auto_(const json&, const std::string&, const std::vector<std::string>&) override { return std::nullopt; }
    std::string resumir_error(const std::string& salida) override;
    std::string titulo_error(const std::string& resumen) override;
    std::string prompt_formalizar(const std::string& problema, const std::string& error) override;
    std::string prompt_fidelidad(const std::string& problema, const std::string& defs, const std::string& firma) override;
    std::string prompt_prueba(const std::string& firma, const std::vector<std::string>& bloques, const json& estado, const json& nodo, const std::string& tipo) override;
private:
    Config cfg_;
    std::string python_;
};

extern const char* PRELUDIO_PY;
std::string buscar_en_path(const std::string& nombre);

}  // namespace demo
