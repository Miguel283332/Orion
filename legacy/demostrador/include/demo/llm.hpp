// Cliente del LLM (Ollama) mediante el ejecutable `curl`.
#pragma once
#include "demo/config.hpp"
#include "demo/util.hpp"

namespace demo {

class Llm {
public:
    virtual ~Llm() = default;
    virtual std::string chat(const std::string& sistema, const std::string& usuario, double temperatura, int num_predict, int seed, bool json_mode) = 0;
};

class OllamaLlm : public Llm {
public:
    OllamaLlm(const Config& cfg, double retardo_reintento_s = 2.0);
    std::string chat(const std::string& sistema, const std::string& usuario, double temperatura, int num_predict, int seed, bool json_mode) override;
    const std::string& base() const { return base_; }
private:
    json post(const std::string& ruta, const json& payload, int reintentos = 3);
    std::string base_, modelo_;
    int num_ctx_;
    double retardo_;
};

std::string normalizar_url_ollama(const std::string& raw);

}  // namespace demo
