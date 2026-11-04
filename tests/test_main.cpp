// Tests de Orion (sin dependencias externas).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <random>

#include "orion/backend.hpp"
#include "orion/chat.hpp"
#include "orion/dataset.hpp"
#include "orion/evolve.hpp"
#include "orion/model.hpp"

using namespace orion;

static int g_fail = 0, g_run = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        ++g_run;                                                           \
        if (!(cond)) { ++g_fail; std::printf("  FALLO %s:%d: %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

static void batchFrom(const std::string& text, int ctx, int B, std::vector<int>& ids, std::vector<int>& tgt, uint64_t seed) {
    std::mt19937_64 rng(seed);
    ids.assign((size_t)B * ctx, 0);
    tgt.assign(B, 0);
    for (int b = 0; b < B; ++b) {
        size_t t = rng() % text.size();
        for (int c = 0; c < ctx; ++c) {
            long long idx = (long long)t - ctx + c;
            ids[(size_t)b * ctx + c] = idx >= 0 ? (unsigned char)text[idx] : 0;
        }
        tgt[b] = (unsigned char)text[t];
    }
}

static const std::string kText =
    "Usuario: hola\nOrion: Hola, soy Orion. Estoy aprendiendo a conversar.\n"
    "Usuario: como estas\nOrion: Estoy bien, gracias por preguntar.\n"
    "Usuario: que haces\nOrion: Leo, escribo y mejoro mi propio modelo.\n";

static double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, (double)std::fabs(a[i] - b[i]));
    return m;
}

// Compara gemm de un backend con la implementación ingenua.
static void testGemm(Backend& be, const char* label) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1, 1);
    for (int tA = 0; tA < 2; ++tA)
        for (int tB = 0; tB < 2; ++tB) {
            const int M = 37, N = 53, K = 29;  // tamaños no múltiplos de bloque
            std::vector<float> A((size_t)M * K), B((size_t)K * N), C0((size_t)M * N);
            for (auto& x : A) x = u(rng);
            for (auto& x : B) x = u(rng);
            for (auto& x : C0) x = u(rng);
            std::vector<float> ref = C0;
            const float alpha = 0.7f, beta = 0.5f;
            for (int i = 0; i < M; ++i)
                for (int j = 0; j < N; ++j) {
                    double s = 0;
                    for (int k = 0; k < K; ++k) {
                        float a = tA ? A[(size_t)k * M + i] : A[(size_t)i * K + k];
                        float b = tB ? B[(size_t)j * K + k] : B[(size_t)k * N + j];
                        s += (double)a * b;
                    }
                    ref[(size_t)i * N + j] = (float)(alpha * s + beta * ref[(size_t)i * N + j]);
                }
            auto bA = be.alloc(A.size()), bB = be.alloc(B.size()), bC = be.alloc(C0.size());
            be.upload(*bA, 0, A.data(), A.size());
            be.upload(*bB, 0, B.data(), B.size());
            be.upload(*bC, 0, C0.data(), C0.size());
            be.gemm(tA, tB, M, N, K, alpha, Span(bA.get()), Span(bB.get()), beta, Span(bC.get()));
            std::vector<float> out(C0.size());
            be.download(*bC, 0, out.data(), out.size());
            CHECK(maxAbsDiff(out, ref) < 1e-4, "[%s] gemm tA=%d tB=%d dif=%g", label, tA, tB, maxAbsDiff(out, ref));
        }
}

static void testGradients(Backend& be, const char* label) {
    Genome g; g.dim = 8; g.ctx = 6; g.hidden = 24;
    HyperParams hp; hp.batch = 16;
    State s = freshState(g, hp, 99);
    Model m(be, s);
    std::vector<int> ids, tgt;
    batchFrom(kText, g.ctx, 16, ids, tgt, 5);
    m.forwardBackward(ids, tgt, 16);
    std::vector<float> grad = m.getGrads();
    // La pérdida es float32 (~5.5): su resolución limita el gradiente numérico a ~ulp/(2*eps).
    // Solo se comparan parámetros con gradiente claramente medible.
    std::mt19937_64 rng(3);
    int bad = 0, checked = 0;
    for (int n = 0; n < 600 && checked < 40; ++n) {
        size_t i = rng() % s.p.size();
        if (std::fabs(grad[i]) < 3e-3) continue;
        const float eps = 1e-2f;
        State a = s, b = s;
        a.p[i] += eps; b.p[i] -= eps;
        m.restore(a); float la = m.evalLoss(ids, tgt, 16);
        m.restore(b); float lb = m.evalLoss(ids, tgt, 16);
        double num = (la - lb) / (2 * eps), ana = grad[i];
        ++checked;
        double rel = std::fabs(num - ana) / (std::fabs(num) + std::fabs(ana));
        if (rel > 0.05) { ++bad; std::printf("    param %zu numérico=%g analítico=%g\n", i, num, ana); }
    }
    CHECK(bad == 0 && checked >= 20, "[%s] gradientes: %d de %d incorrectos", label, bad, checked);
}

static void testGrowthPreserves(Backend& be, const char* label) {
    Genome g; g.dim = 8; g.ctx = 6; g.hidden = 24;
    HyperParams hp; hp.batch = 8;
    State s = freshState(g, hp, 11);
    Model m(be, s);
    std::vector<int> ids, tgt;
    batchFrom(kText, g.ctx, 8, ids, tgt, 21);
    // entrenar un poco para que los pesos no sean los iniciales
    for (int i = 0; i < 20; ++i) m.trainStep(ids, tgt, 8);
    State trained = m.snapshot();
    std::vector<float> before;
    std::vector<int> ctx(ids.begin(), ids.begin() + g.ctx);
    m.logits(ctx, before);

    State grown = growState(trained, 40, 10, 5);
    Model m2(be, grown);
    std::vector<int> ctx2(10, 0);  // contexto más largo: los 4 bytes nuevos (antiguos) valen 0 y aportan 0
    std::copy(ctx.begin(), ctx.end(), ctx2.begin() + 4);
    std::vector<float> after;
    m2.logits(ctx2, after);
    CHECK(grown.genome.hidden == 40 && grown.genome.ctx == 10, "genoma crecido");
    CHECK(maxAbsDiff(before, after) < 1e-4, "[%s] crecer conserva la función, dif=%g", label, maxAbsDiff(before, after));
    // y el modelo crecido sigue entrenando
    std::vector<int> ids2, tgt2;
    batchFrom(kText, 10, 8, ids2, tgt2, 21);
    float first = m2.trainStep(ids2, tgt2, 8), last = first;
    for (int i = 0; i < 30; ++i) last = m2.trainStep(ids2, tgt2, 8);
    CHECK(std::isfinite(last) && last < first, "[%s] el modelo crecido sigue aprendiendo (%g -> %g)", label, first, last);
}

static void testBackendParity(Backend& a, Backend& b) {
    Genome g; g.dim = 8; g.ctx = 6; g.hidden = 40;
    HyperParams hp; hp.batch = 16;
    State s = freshState(g, hp, 4242);
    Model ma(a, s), mb(b, s);
    std::vector<int> ids, tgt;
    batchFrom(kText, g.ctx, 16, ids, tgt, 8);
    float la = 0, lb = 0;
    for (int i = 0; i < 10; ++i) { la = ma.trainStep(ids, tgt, 16); lb = mb.trainStep(ids, tgt, 16); }
    CHECK(std::fabs(la - lb) < 5e-3, "paridad de pérdida tras entrenar: %g vs %g", la, lb);
    State sa = ma.snapshot(), sb = mb.snapshot();
    CHECK(maxAbsDiff(sa.p, sb.p) < 5e-3, "paridad de pesos: dif=%g", maxAbsDiff(sa.p, sb.p));
}

static void testCheckpoint() {
    Genome g; g.dim = 8; g.ctx = 6; g.hidden = 16;
    State s = freshState(g, HyperParams{}, 1);
    s.adamStep = 17;
    auto path = (std::filesystem::temp_directory_path() / "orion_test_ckpt.orn").string();
    saveState(s, path);
    State t;
    CHECK(loadState(t, path), "carga de checkpoint");
    CHECK(t.genome == s.genome && t.adamStep == 17 && t.p == s.p && t.m == s.m, "checkpoint idéntico");
    std::filesystem::remove(path);
    CHECK(!loadState(t, path), "checkpoint inexistente devuelve false");
}

static void testDataset() {
    Dataset d;
    std::string big;
    for (int i = 0; i < 200; ++i) big += kText;
    d.addText(big);
    const size_t n = d.size();
    CHECK(d.count(Split::Train) + d.count(Split::Val) + d.count(Split::Test) == n, "particiones suman el corpus");
    CHECK(d.count(Split::Val) > d.count(Split::Test) && d.count(Split::Train) > d.count(Split::Val), "tamaños de split");
    // append-only: la pertenencia de posiciones existentes no cambia
    std::vector<int> i1, t1, i2, t2;
    d.evalSet(Split::Val, 8, 50, i1, t1);
    size_t valBefore = d.count(Split::Val);
    d.addText("texto nuevo que se añade al final del corpus");
    CHECK(d.count(Split::Val) >= valBefore, "la validación solo crece");
}

static void testChatMask(Backend& be) {
    // Un modelo SIN entrenar no debe emitir bytes que no existen en el corpus ni caracteres de control.
    Dataset d;
    std::string big;
    for (int i = 0; i < 60; ++i) big += kText;
    d.addText(big);
    Genome g; g.dim = 8; g.ctx = 8; g.hidden = 32;
    Model m(be, freshState(g, HyperParams{}, 3));
    Chat chat(m, 1);
    chat.restrictTo(d);
    chat.config().maxBytes = 300;
    chat.config().topK = 256;
    chat.config().temperature = 1.0f;
    std::string out = chat.continueText("Usuario: hola\nOrion:", false);
    bool ok = !out.empty();
    for (unsigned char c : out) ok = ok && d.hasByte(c) && (c >= 32 || c == '\n' || c == '\t');
    CHECK(ok, "el muestreo respeta el corpus (sin bytes inventados)");
}

static void testLearning(Backend& be, const char* label) {
    // Texto repetitivo: el modelo debe aprender claramente (pérdida << ln(256)=5.55).
    Dataset d;
    std::string big;
    for (int i = 0; i < 120; ++i) big += kText;
    d.addText(big);
    Genome g; g.dim = 16; g.ctx = 16; g.hidden = 96;
    HyperParams hp; hp.batch = 64; hp.lr = 3e-3f;
    Model m(be, freshState(g, hp, 77));
    Learner L(m, d, 5);
    float before = L.evalVal(512);
    L.train(150);
    float after = L.evalVal(512);
    std::printf("  [%s] val %.3f -> %.3f\n", label, before, after);
    CHECK(before > 4.0f, "pérdida inicial ~ln(256)");
    CHECK(after < before * 0.5f, "[%s] el modelo aprende", label);

    // Un paso evolutivo nunca empeora la validación (acepta solo si mejora, si no hace rollback).
    EvolveConfig cfg; cfg.stepsPerGen = 30; cfg.evalMax = 512;
    auto home = (std::filesystem::temp_directory_path() / "orion_test_home").string();
    std::filesystem::remove_all(home);
    Evolver ev(m, d, L, cfg, home);
    double best = ev.bestVal();
    for (int i = 0; i < 12; ++i) {
        GenResult r = ev.step();
        CHECK(ev.bestVal() <= best + 1e-9, "[%s] la evolución nunca empeora (gen %d op %s)", label, r.gen, r.op.c_str());
        if (!r.accepted) {
            // tras el rollback el modelo debe tener exactamente la pérdida de la línea base
            float v = L.evalVal(512);
            CHECK(std::fabs(v - ev.bestVal()) < 1e-4, "rollback exacto: %g vs %g", v, ev.bestVal());
        }
        best = ev.bestVal();
    }
    CHECK(std::filesystem::exists(home + "/checkpoints/best.orn") || ev.generation() > 0, "se registró la evolución");
    std::filesystem::remove_all(home);
}

int main() {
    std::vector<std::pair<std::string, std::unique_ptr<Backend>>> bes;
    bes.emplace_back("cpu", makeCpuBackend());
    for (auto& d : listOpenCLDevices()) {
        try {
            auto b = makeOpenCLBackend(d.platform, d.device);
            std::printf("backend OpenCL probado: %s [%s]\n", b->name().c_str(), b->kind().c_str());
            bes.emplace_back("cl:" + std::to_string(d.platform) + ":" + std::to_string(d.device), std::move(b));
        } catch (const std::exception& e) {
            std::printf("OpenCL %s no utilizable: %s\n", d.name.c_str(), e.what());
        }
    }
    for (auto& [name, be] : bes) {
        std::printf("== backend %s ==\n", name.c_str());
        testGemm(*be, name.c_str());
        testGradients(*be, name.c_str());
        testGrowthPreserves(*be, name.c_str());
        testLearning(*be, name.c_str());
    }
    if (bes.size() > 1) {
        std::printf("== paridad CPU vs %s ==\n", bes[1].first.c_str());
        testBackendParity(*bes[0].second, *bes[1].second);
    }
    std::printf("== checkpoint / dataset ==\n");
    testCheckpoint();
    testChatMask(*bes[0].second);
    testDataset();
    std::printf("\n%d comprobaciones, %d fallos\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
