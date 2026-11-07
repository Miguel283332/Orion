// Auto-mejora: bucle (1+1)-evolutivo con selección por validación y rollback.
//
// Cada "generación":
//   1. Si hay datos nuevos (conversaciones, archivos ingeridos) se recalcula la línea base.
//   2. Se elige un operador de mutación (bandido: favorece los que más han mejorado por segundo).
//   3. Se aplica sobre una copia del estado (snapshot) y se reentrena un presupuesto de pasos.
//   4. Se mide la pérdida de VALIDACIÓN. Si mejora lo bastante -> se acepta y se guarda el
//      checkpoint; si no -> rollback exacto al snapshot. Nada empeora nunca.
// Operadores: finetune, lr_up/down, wd_up/down, mutate (perturbación dispersa de pesos),
//             grow_hidden / grow_ctx (la red crece sin perder lo aprendido), batch_up.
#pragma once
#include <atomic>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "orion/dataset.hpp"
#include "orion/model.hpp"

namespace orion {

struct EvolveConfig {
    int stepsPerGen = 150;
    size_t evalMax = 2048;
    double minRelGain = 5e-4;  // mejora relativa mínima para aceptar
    int maxHidden = 2048;
    int maxCtx = 256;
    int maxBatch = 512;
};

struct GenResult {
    int gen = 0;
    std::string op, detail;
    bool accepted = false, aborted = false;
    double before = 0, after = 0, secs = 0;
};

class Learner {
public:
    Learner(Model& m, Dataset& d, uint64_t seed) : model_(m), data_(d), rng_(seed) {}
    // Entrena `steps` pasos; devuelve la pérdida de entrenamiento media de los últimos pasos.
    // `yield` se llama entre pasos (permite ceder el modelo al chat); `stop` aborta pronto.
    float train(int steps, const std::function<void()>& yield = {}, const std::atomic<bool>* stop = nullptr);
    float evalSplit(Split s, size_t maxN);
    float evalVal(size_t maxN) { return evalSplit(Split::Val, maxN); }
    std::mt19937_64& rng() { return rng_; }

private:
    Model& model_;
    Dataset& data_;
    std::mt19937_64 rng_;
};

class Evolver {
public:
    Evolver(Model& model, Dataset& data, Learner& learner, EvolveConfig cfg, std::string homeDir);

    GenResult step(const std::function<void()>& yield = {}, const std::atomic<bool>* stop = nullptr);
    double bestVal() const { return bestVal_; }
    int generation() const { return gen_; }
    std::string opTable() const;
    void rebaseline();           // recalcula bestVal con el modelo y el corpus actuales
    void saveCheckpoint() const; // checkpoints/best.orn + evolution.state
    bool loadCheckpoint();       // carga best.orn si existe (devuelve true si cargó)
    EvolveConfig& config() { return cfg_; }

private:
    struct Op { std::string name; int tries = 0, wins = 0; double gain = 1e-3; bool growth = false; };
    int chooseOp(bool stagnating);
    void apply(int op, std::string& detail, const std::function<void()>& yield, const std::atomic<bool>* stop);
    void log(const GenResult& r) const;

    Model& model_;
    Dataset& data_;
    Learner& learner_;
    EvolveConfig cfg_;
    std::string home_;
    std::vector<Op> ops_;
    double bestVal_ = 0;
    int gen_ = 0;
    int stagnation_ = 0;
    std::mt19937_64 rng_;
};

}  // namespace orion
