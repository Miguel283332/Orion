// Cliente de Ollama: chat en streaming (cancelable), información del modelo y uso de memoria.
// Es la única puerta al modelo: más adelante se podrá sustituir por llama.cpp integrado (fase 5) sin tocar el resto.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
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
    json llamadas = json::array();   // herramientas que el modelo pide usar: [{"function":{"name":..,"arguments":{..}}}]
    long long tokens_prompt = 0, tokens_generados = 0;
    double segundos_prompt = 0, segundos_generacion = 0;
    double tokens_por_segundo() const { return segundos_generacion > 0 ? tokens_generados / segundos_generacion : 0; }
};

struct InfoModelo {
    bool ok = false;
    std::string error;
    std::string nombre, familia, cuantizacion, tamano_parametros;   // tamano_parametros: "3.1B" según Ollama
    std::string ruta_pesos;                                           // archivo GGUF con los pesos (línea FROM del Modelfile)
    double bytes_pesos = 0;
    bool admite_herramientas = false;
    double parametros = 0;                                            // exacto si Ollama lo da (general.parameter_count)
    long long contexto_max = 0;
    double bytes_en_memoria = 0, bytes_en_vram = 0;                  // de /api/ps (0 si no está cargado)
    bool cargado = false;
};

class Ollama {
public:
    Ollama(const std::string& url, std::string modelo);
    std::string modelo() const;
    std::string url() const;
    void cambiar_modelo(const std::string& m);
    void cambiar_url(const std::string& url);
    // Vectores de significado (embeddings) con el MISMO modelo. Devuelve false (y error) si el modelo no los da.
    bool embed(const std::vector<std::string>& textos, std::vector<std::vector<float>>& vectores, std::string* error = nullptr) const;
    bool disponible(std::string* version = nullptr) const;
    // mensajes: [{"role":"system|user|assistant","content":"..."}]. por_trozo recibe el texto según llega.
    // herramientas: lista en el formato de Ollama (o nullptr). Si el modelo pide usarlas, vienen en ResultadoGeneracion::llamadas.
    ResultadoGeneracion chat(const json& mensajes, const OpcionesGeneracion& op, const std::function<void(const std::string&)>& por_trozo,
                             const std::atomic<bool>* cancelar, const json* herramientas = nullptr) const;
    InfoModelo info() const;
private:
    ClienteHttp cliente() const;
    mutable std::mutex mu_;
    ClienteHttp http_;
    std::string url_;
    std::string modelo_;
};

}  // namespace orion
