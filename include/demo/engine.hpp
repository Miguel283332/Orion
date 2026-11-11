// Motor: árbol de demostración persistente y reanudable (mismo formato de estado que la versión Python).
#pragma once
#include <functional>

#include "demo/config.hpp"
#include "demo/llm.hpp"
#include "demo/verifier.hpp"

namespace demo {

constexpr int ESTADO_VERSION = 6;

class Motor {
public:
    Motor(const Config& cfg, Llm& llm, Verificador& ver, fs::path directorio, std::function<void(const std::string&)> log = {});
    int ejecutar(const json& problema, bool reiniciar = false);   // 0 verificado, 1 fallido, 3 pausa, 4 error del modelo, 130 interrumpido
    const json& estado() const { return estado_; }
    fs::path ruta_estado() const { return dir_ / "estado.json"; }

private:
    void cargar(const json& problema, bool reiniciar);
    void guardar();
    void reabrir_fallidos();
    void evento(const std::string& nombre, json datos = json::object());
    void say(const json* n, const std::string& msg);
    void presupuesto();
    std::string llm_(const std::string& prompt, double temperatura, int seed, bool json_mode = false, int num_predict = 0);
    json nuevo_estado(const json& problema);
    std::string huella_problema(const json& problema);

    void formalizar();
    std::pair<std::string, std::string> validar_formalizacion(const json& datos);
    void fidelidad(const std::string& problema, const std::string& firma);

    json nodo(const std::string& firma, int prof);
    std::string bloque_nodo(const json& n);
    bool fallo(json& n, const std::string& cuerpo, const std::string& salida, const std::string& tipo);
    bool aceptar(json& n, const std::string& cuerpo, const std::string& tipo);
    bool intento(json& n, const std::vector<std::string>& ctx, const std::string& tipo);
    std::vector<json> planificar(json& n, const std::vector<std::string>& ctx);
    bool resolver_nodo(json& n, const std::vector<std::string>& ctx);
    bool fallar(json& n);
    bool finalizar();

    Config cfg_;
    Llm& llm_cli_;
    Verificador& ver_;
    fs::path dir_;
    std::function<void(const std::string&)> log_;
    json estado_;
    std::chrono::steady_clock::time_point t_inicio_;
    double t_base_ = 0.0;
    int llamadas_sesion_ = 0;
};

std::string informe(const fs::path& directorio);
std::vector<std::string> arbol(const json& n, int nivel = 0);

}  // namespace demo
