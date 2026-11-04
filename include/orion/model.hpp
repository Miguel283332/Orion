// Modelo de lenguaje a nivel de byte (vocabulario 256, sin tokenizador externo).
//
//   ids[ctx bytes] -> embeddings concatenados -> Dense+ReLU -> bloque residual Dense+ReLU -> logits[256]
//
// Todo el cómputo pasa por `Backend`, así que corre igual en GPU dedicada, integrada o CPU.
// El estado completo (genoma + hiperparámetros + pesos + Adam) se puede copiar (`State`),
// ensanchar sin perder lo aprendido (`growState`) y guardar/cargar de forma atómica.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "orion/backend.hpp"

namespace orion {

// "Genoma": la arquitectura. La auto-evolución puede cambiarla en tiempo de ejecución.
struct Genome {
    int vocab = 256;
    int dim = 32;      // dimensión de embedding por byte
    int ctx = 48;      // bytes de contexto
    int hidden = 512;  // anchura de las capas ocultas
    bool operator==(const Genome& o) const {
        return vocab == o.vocab && dim == o.dim && ctx == o.ctx && hidden == o.hidden;
    }
    bool operator!=(const Genome& o) const { return !(*this == o); }
    int inputs() const { return ctx * dim; }
};

struct HyperParams {
    float lr = 2e-3f;
    float wd = 0.05f;
    float clip = 1.0f;
    float beta1 = 0.9f, beta2 = 0.999f, eps = 1e-8f;
    int batch = 128;
    float ctxDrop = 0.15f;  // prob. de borrar cada byte del contexto al entrenar (regularización)
};

struct Layout {
    size_t E, W1, b1, W2, b2, W3, b3, total;
    explicit Layout(const Genome& g);
};

// Copia completa y portable del estado del modelo.
struct State {
    Genome genome;
    HyperParams hp;
    int adamStep = 0;
    std::vector<float> p, m, v;
    size_t paramCount() const { return p.size(); }
};

// Estado nuevo con más capacidad (hidden/ctx mayores) que calcula EXACTAMENTE la misma función.
State growState(const State& s, int newHidden, int newCtx, uint64_t seed);
State freshState(const Genome& g, const HyperParams& hp, uint64_t seed);
void saveState(const State& s, const std::string& path);  // atómico (tmp + rename)
bool loadState(State& s, const std::string& path);

class Model {
public:
    Model(Backend& backend, const State& initial);

    const Genome& genome() const { return state_.genome; }
    HyperParams& hp() { return state_.hp; }
    const HyperParams& hp() const { return state_.hp; }
    size_t paramCount() const { return layout_->total; }
    Backend& backend() { return be_; }

    // ids: B*ctx bytes de contexto (más antiguo primero); targets: B bytes siguientes.
    float forwardBackward(const std::vector<int>& ids, const std::vector<int>& targets, int B);
    void applyAdam();  // consume los gradientes de forwardBackward
    float trainStep(const std::vector<int>& ids, const std::vector<int>& targets, int B) {
        float l = forwardBackward(ids, targets, B);
        applyAdam();
        return l;
    }
    float evalLoss(const std::vector<int>& ids, const std::vector<int>& targets, int B);  // sin gradientes
    void logits(const std::vector<int>& ctxIds, std::vector<float>& out);                 // B=1

    State snapshot();
    void restore(const State& s);  // reconfigura buffers si cambia el genoma
    std::vector<float> getGrads();

private:
    void configure(const State& s);
    void forward(const std::vector<int>& ids, int B);
    void ensureBatch(int B);

    Backend& be_;
    State state_;  // solo metadatos (genoma/hp/adamStep); los pesos viven en el dispositivo
    std::unique_ptr<Layout> layout_;
    std::unique_ptr<Buffer> P_, G_, M_, V_;
    // activaciones
    int cap_ = 0;
    std::unique_ptr<Buffer> idsB_, tgtB_, X_, H1_, A2_, H2_, L_, dL_, dH2_, dA2_, dH1_, dX_, loss_;
};

}  // namespace orion
