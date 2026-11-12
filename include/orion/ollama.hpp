// Cliente de Ollama: chat en streaming (cancelable), información del modelo y uso de memoria.
// Es la única puerta al modelo: más adelante se podrá sustituir por llama.cpp integrado (fase 5) sin tocar el resto.
#pragma once
#include <atomic>
#include <functional>
#include <string>

#include "orion/http.hpp"
#include "third_party_json.hpp"

namespace orion {

struct OpcionesGeneracion {
    double temperatura = 0.7;
    int num_ctx = 4096;
    int num_predict = 512;
    int semilla = -1;            // -1: aleatoria
    bool json = false;           // pedir JSON
};

struct ResultadoGeneracion {
    std::string texto;
    bool ok = false;
    bool cancelada = false;
    std::string error;
    long long tokens_prompt = 0, tokens_generados = 0;
    double segundos_prompt = 0, segundos_generacion = 0;
    double tokens_por_segundo() const { return segundos_generacion > 0 ? tokens_generados / segundos_generacion : 0; }
};

struct InfoModelo {
    bool ok = false;
    std::string error;
    std::string nombre, familia, cuantizacion, tamano_parametros;   // tamano_parametros: "3.1B" según Ollama
    double parametros = 0;                                            // exacto si Ollama lo da (general.parameter_count)
    long long contexto_max = 0;
    double bytes_en_memoria = 0, bytes_en_vram = 0;                  // de /api/ps (0 si no está cargado)
    bool cargado = false;
};

class Ollama {
public:
    Ollama(const std::string& url, std::string modelo);
    const std::string& modelo() const { return modelo_; }
    void cambiar_modelo(const std::string& m) { modelo_ = m; }
    bool disponible(std::string* version = nullptr) const;
    // mensajes: [{"role":"system|user|assistant","content":"..."}]. por_trozo recibe el texto según llega.
    ResultadoGeneracion chat(const json& mensajes, const OpcionesGeneracion& op, const std::function<void(const std::string&)>& por_trozo,
                             const std::atomic<bool>* cancelar) const;
    InfoModelo info() const;
private:
    ClienteHttp http_;
    std::string modelo_;
};

}  // namespace orion
