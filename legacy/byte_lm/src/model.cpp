#include "orion/model.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace orion {

Layout::Layout(const Genome& g) {
    size_t o = 0;
    const size_t in = (size_t)g.ctx * g.dim, h = g.hidden, V = g.vocab;
    E = o;  o += V * g.dim;
    W1 = o; o += in * h;
    b1 = o; o += h;
    W2 = o; o += h * h;
    b2 = o; o += h;
    W3 = o; o += h * V;
    b3 = o; o += V;
    total = o;
}

// ---------------------------------------------------------------- estado

State freshState(const Genome& g, const HyperParams& hp, uint64_t seed) {
    State s;
    s.genome = g; s.hp = hp;
    Layout L(g);
    s.p.assign(L.total, 0.0f);
    s.m.assign(L.total, 0.0f);
    s.v.assign(L.total, 0.0f);
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    const size_t in = (size_t)g.ctx * g.dim;
    auto fill = [&](size_t off, size_t n, float sd) { for (size_t i = 0; i < n; ++i) s.p[off + i] = nd(rng) * sd; };
    fill(L.E, (size_t)g.vocab * g.dim, 0.3f);
    fill(L.W1, in * g.hidden, std::sqrt(2.0f / in));
    fill(L.W2, (size_t)g.hidden * g.hidden, 0.5f * std::sqrt(2.0f / g.hidden));
    fill(L.W3, (size_t)g.hidden * g.vocab, 0.5f * std::sqrt(1.0f / g.hidden));
    return s;
}

namespace {

// Copia un tensor [rows x cols] a otro [nrows x ncols]. `rowShift` desplaza las filas antiguas
// (para insertar contexto más antiguo al principio). Las posiciones nuevas quedan como estén.
void copyBlock(const float* src, int rows, int cols, float* dst, int ncols, int rowShift) {
    for (int r = 0; r < rows; ++r)
        std::memcpy(dst + (size_t)(r + rowShift) * ncols, src + (size_t)r * cols, sizeof(float) * cols);
}

}  // namespace

State growState(const State& s, int newHidden, int newCtx, uint64_t seed) {
    const Genome og = s.genome;
    Genome ng = og;
    ng.hidden = std::max(newHidden, og.hidden);
    ng.ctx = std::max(newCtx, og.ctx);
    Layout ol(og), nl(ng);
    State t;
    t.genome = ng; t.hp = s.hp; t.adamStep = s.adamStep;
    t.p.assign(nl.total, 0.0f);
    t.m.assign(nl.total, 0.0f);
    t.v.assign(nl.total, 0.0f);

    const int oh = og.hidden, nh = ng.hidden, dim = og.dim, V = og.vocab;
    const int oin = og.ctx * dim, nin = ng.ctx * dim;
    const int shiftRows = (ng.ctx - og.ctx) * dim;  // filas nuevas de contexto antiguo, al principio

    auto grow = [&](const std::vector<float>& src, std::vector<float>& dst) {
        std::memcpy(dst.data() + nl.E, src.data() + ol.E, sizeof(float) * (size_t)V * dim);
        copyBlock(src.data() + ol.W1, oin, oh, dst.data() + nl.W1, nh, shiftRows);   // nuevas filas y columnas = 0
        std::memcpy(dst.data() + nl.b1, src.data() + ol.b1, sizeof(float) * oh);
        copyBlock(src.data() + ol.W2, oh, oh, dst.data() + nl.W2, nh, 0);
        std::memcpy(dst.data() + nl.b2, src.data() + ol.b2, sizeof(float) * oh);
        copyBlock(src.data() + ol.W3, oh, V, dst.data() + nl.W3, V, 0);               // filas nuevas = 0
        std::memcpy(dst.data() + nl.b3, src.data() + ol.b3, sizeof(float) * V);
    };
    grow(s.p, t.p);
    grow(s.m, t.m);
    grow(s.v, t.v);

    // Inicialización aleatoria SOLO donde no cambia la función:
    //  - W1: columnas nuevas (unidades nuevas de la capa 1) -> sus salidas entran a W2 con filas nuevas = 0
    //  - W2: columnas nuevas (unidades nuevas del bloque residual); las filas nuevas siguen a 0
    // Las filas nuevas de W3 y las filas nuevas de contexto en W1 quedan a 0 y aprenden por gradiente.
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    if (nh > oh) {
        const float sd1 = std::sqrt(2.0f / nin);
        for (int r = 0; r < nin; ++r)
            for (int c = oh; c < nh; ++c) t.p[nl.W1 + (size_t)r * nh + c] = nd(rng) * sd1;
        const float sd2 = 0.5f * std::sqrt(2.0f / nh);
        for (int r = 0; r < nh; ++r)
            for (int c = oh; c < nh; ++c) t.p[nl.W2 + (size_t)r * nh + c] = nd(rng) * sd2;
    }
    return t;
}

void saveState(const State& s, const std::string& path) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) throw std::runtime_error("no se puede escribir " + tmp);
        const char magic[4] = {'O', 'R', 'N', '2'};
        out.write(magic, 4);
        int32_t g[4] = {s.genome.vocab, s.genome.dim, s.genome.ctx, s.genome.hidden};
        out.write(reinterpret_cast<const char*>(g), sizeof(g));
        out.write(reinterpret_cast<const char*>(&s.hp), sizeof(HyperParams));
        int32_t st = s.adamStep;
        out.write(reinterpret_cast<const char*>(&st), sizeof(st));
        uint64_t n = s.p.size();
        out.write(reinterpret_cast<const char*>(&n), sizeof(n));
        out.write(reinterpret_cast<const char*>(s.p.data()), n * sizeof(float));
        out.write(reinterpret_cast<const char*>(s.m.data()), n * sizeof(float));
        out.write(reinterpret_cast<const char*>(s.v.data()), n * sizeof(float));
        if (!out) throw std::runtime_error("error escribiendo " + tmp);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);  // reemplaza el destino también en Windows
    if (ec) throw std::runtime_error("no se puede renombrar " + tmp + ": " + ec.message());
}

bool loadState(State& s, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4];
    in.read(magic, 4);
    if (!in || std::memcmp(magic, "ORN2", 4) != 0) return false;
    int32_t g[4];
    in.read(reinterpret_cast<char*>(g), sizeof(g));
    State t;
    t.genome.vocab = g[0]; t.genome.dim = g[1]; t.genome.ctx = g[2]; t.genome.hidden = g[3];
    in.read(reinterpret_cast<char*>(&t.hp), sizeof(HyperParams));
    int32_t st;
    in.read(reinterpret_cast<char*>(&st), sizeof(st));
    t.adamStep = st;
    uint64_t n;
    in.read(reinterpret_cast<char*>(&n), sizeof(n));
    if (!in || n != Layout(t.genome).total) return false;
    t.p.resize(n); t.m.resize(n); t.v.resize(n);
    in.read(reinterpret_cast<char*>(t.p.data()), n * sizeof(float));
    in.read(reinterpret_cast<char*>(t.m.data()), n * sizeof(float));
    in.read(reinterpret_cast<char*>(t.v.data()), n * sizeof(float));
    if (!in) return false;
    s = std::move(t);
    return true;
}

// ---------------------------------------------------------------- modelo

Model::Model(Backend& backend, const State& initial) : be_(backend) { configure(initial); }

void Model::configure(const State& s) {
    state_.genome = s.genome; state_.hp = s.hp; state_.adamStep = s.adamStep;
    layout_ = std::make_unique<Layout>(s.genome);
    const size_t n = layout_->total;
    P_ = be_.alloc(n); G_ = be_.alloc(n); M_ = be_.alloc(n); V_ = be_.alloc(n);
    be_.upload(*P_, 0, s.p.data(), n);
    be_.upload(*M_, 0, s.m.data(), n);
    be_.upload(*V_, 0, s.v.data(), n);
    cap_ = 0;
    ensureBatch(std::max(1, s.hp.batch));
}

void Model::ensureBatch(int B) {
    if (B <= cap_) return;
    cap_ = B;
    const Genome& g = state_.genome;
    const size_t in = (size_t)g.inputs(), h = g.hidden, V = g.vocab;
    idsB_ = be_.alloc((size_t)B * g.ctx);
    tgtB_ = be_.alloc(B);
    X_ = be_.alloc(B * in);
    H1_ = be_.alloc(B * h); A2_ = be_.alloc(B * h); H2_ = be_.alloc(B * h);
    L_ = be_.alloc(B * V); dL_ = be_.alloc(B * V);
    dH2_ = be_.alloc(B * h); dA2_ = be_.alloc(B * h); dH1_ = be_.alloc(B * h);
    dX_ = be_.alloc(B * in);
    loss_ = be_.alloc(B);
}

State Model::snapshot() {
    State s = state_;
    const size_t n = layout_->total;
    s.p.resize(n); s.m.resize(n); s.v.resize(n);
    be_.download(*P_, 0, s.p.data(), n);
    be_.download(*M_, 0, s.m.data(), n);
    be_.download(*V_, 0, s.v.data(), n);
    return s;
}

void Model::restore(const State& s) {
    if (s.genome == state_.genome) {
        state_.hp = s.hp; state_.adamStep = s.adamStep;
        const size_t n = layout_->total;
        be_.upload(*P_, 0, s.p.data(), n);
        be_.upload(*M_, 0, s.m.data(), n);
        be_.upload(*V_, 0, s.v.data(), n);
    } else {
        configure(s);
    }
}

std::vector<float> Model::getGrads() {
    std::vector<float> g(layout_->total);
    be_.download(*G_, 0, g.data(), g.size());
    return g;
}

void Model::forward(const std::vector<int>& ids, int B) {
    const Genome& g = state_.genome;
    const Layout& L = *layout_;
    const int in = g.inputs(), h = g.hidden, V = g.vocab;
    ensureBatch(B);
    be_.upload(*idsB_, 0, ids.data(), (size_t)B * g.ctx);
    auto p = [&](size_t off) { return Span(P_.get(), off); };

    be_.gather(p(L.E), *idsB_, Span(X_.get()), B * g.ctx, g.dim);
    be_.gemm(false, false, B, h, in, 1.0f, Span(X_.get()), p(L.W1), 0.0f, Span(H1_.get()));
    be_.biasAct(Span(H1_.get()), p(L.b1), B, h, true);
    be_.gemm(false, false, B, h, h, 1.0f, Span(H1_.get()), p(L.W2), 0.0f, Span(A2_.get()));
    be_.biasAct(Span(A2_.get()), p(L.b2), B, h, true);
    be_.copy(Span(H1_.get()), Span(H2_.get()), (size_t)B * h);
    be_.axpy(1.0f, Span(A2_.get()), Span(H2_.get()), (size_t)B * h);
    be_.gemm(false, false, B, V, h, 1.0f, Span(H2_.get()), p(L.W3), 0.0f, Span(L_.get()));
    be_.biasAct(Span(L_.get()), p(L.b3), B, V, false);
}

float Model::evalLoss(const std::vector<int>& ids, const std::vector<int>& targets, int B) {
    forward(ids, B);
    be_.upload(*tgtB_, 0, targets.data(), B);
    be_.softmaxXent(Span(L_.get()), *tgtB_, Span(dL_.get()), Span(loss_.get()), B, state_.genome.vocab, 1.0f, false);
    std::vector<float> l(B);
    be_.download(*loss_, 0, l.data(), B);
    double s = 0;
    for (float v : l) s += v;
    return (float)(s / B);
}

void Model::logits(const std::vector<int>& ctxIds, std::vector<float>& out) {
    forward(ctxIds, 1);
    out.resize(state_.genome.vocab);
    be_.download(*L_, 0, out.data(), out.size());
}

float Model::forwardBackward(const std::vector<int>& ids, const std::vector<int>& targets, int B) {
    const Genome& g = state_.genome;
    const Layout& L = *layout_;
    const int in = g.inputs(), h = g.hidden, V = g.vocab;
    forward(ids, B);
    be_.upload(*tgtB_, 0, targets.data(), B);
    be_.softmaxXent(Span(L_.get()), *tgtB_, Span(dL_.get()), Span(loss_.get()), B, V, 1.0f / B, true);

    auto p = [&](size_t off) { return Span(P_.get(), off); };
    auto gr = [&](size_t off) { return Span(G_.get(), off); };
    be_.fill(Span(G_.get()), L.total, 0.0f);

    // capa de salida
    be_.gemm(true, false, h, V, B, 1.0f, Span(H2_.get()), Span(dL_.get()), 0.0f, gr(L.W3));
    be_.colSum(Span(dL_.get()), gr(L.b3), B, V);
    be_.gemm(false, true, B, h, V, 1.0f, Span(dL_.get()), p(L.W3), 0.0f, Span(dH2_.get()));
    // bloque residual: H2 = H1 + relu(H1*W2+b2)
    be_.copy(Span(dH2_.get()), Span(dA2_.get()), (size_t)B * h);
    be_.reluGrad(Span(dA2_.get()), Span(A2_.get()), (size_t)B * h);
    be_.gemm(true, false, h, h, B, 1.0f, Span(H1_.get()), Span(dA2_.get()), 0.0f, gr(L.W2));
    be_.colSum(Span(dA2_.get()), gr(L.b2), B, h);
    be_.copy(Span(dH2_.get()), Span(dH1_.get()), (size_t)B * h);
    be_.gemm(false, true, B, h, h, 1.0f, Span(dA2_.get()), p(L.W2), 1.0f, Span(dH1_.get()));
    // capa 1
    be_.reluGrad(Span(dH1_.get()), Span(H1_.get()), (size_t)B * h);
    be_.gemm(true, false, in, h, B, 1.0f, Span(X_.get()), Span(dH1_.get()), 0.0f, gr(L.W1));
    be_.colSum(Span(dH1_.get()), gr(L.b1), B, h);
    be_.gemm(false, true, B, in, h, 1.0f, Span(dH1_.get()), p(L.W1), 0.0f, Span(dX_.get()));
    be_.embedGrad(Span(dX_.get()), *idsB_, gr(L.E), B * g.ctx, g.dim, g.vocab);

    std::vector<float> l(B);
    be_.download(*loss_, 0, l.data(), B);
    double s = 0;
    for (float v : l) s += v;
    return (float)(s / B);
}

void Model::applyAdam() {
    const HyperParams& hp = state_.hp;
    const size_t n = layout_->total;
    float norm = std::sqrt(be_.sumSq(Span(G_.get()), n));
    float gscale = 1.0f;
    if (!std::isfinite(norm)) {
        // gradiente corrupto: no tocar los pesos
        return;
    }
    if (hp.clip > 0 && norm > hp.clip) gscale = hp.clip / (norm + 1e-12f);
    state_.adamStep += 1;
    be_.adam(Span(P_.get()), Span(G_.get()), Span(M_.get()), Span(V_.get()), n, hp.lr, hp.beta1, hp.beta2,
             hp.eps, hp.wd, state_.adamStep, gscale);
}

}  // namespace orion
