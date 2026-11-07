// Generación de texto y memoria de conversación.
#pragma once
#include <random>
#include <string>
#include <vector>

#include "orion/dataset.hpp"
#include "orion/model.hpp"

namespace orion {

struct SampleConfig {
    float temperature = 0.5f;
    int topK = 40;
    int maxBytes = 240;
};

class Chat {
public:
    Chat(Model& m, uint64_t seed) : model_(m), rng_(seed) {}
    SampleConfig& config() { return cfg_; }
    // Continúa `prompt` hasta fin de línea (o maxBytes). No incluye el prompt en el resultado.
    std::string continueText(const std::string& prompt, bool stopAtNewline = true);
    // Formato de diálogo del corpus: "Usuario: ...\nOrion: ...\n"
    std::string reply(const std::string& user);
    // Limita el muestreo a los bytes que aparecen en el corpus (sin controles salvo \n y \t).
    void restrictTo(const Dataset& d);
    void reset() { history_.clear(); }
    const std::string& history() const { return history_; }

private:
    int sampleByte(const std::vector<float>& logits);
    Model& model_;
    std::mt19937_64 rng_;
    SampleConfig cfg_;
    std::string history_;
    std::vector<char> allowed_;  // vacío = sin restricción
};

}  // namespace orion
