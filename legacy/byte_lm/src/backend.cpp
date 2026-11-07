// Detección y selección automática del dispositivo de cómputo.
#include <chrono>
#include <stdexcept>
#include <cstdio>
#include <sstream>

#include "orion/backend.hpp"

namespace orion {

double benchmarkBackend(Backend& be, int reps) {
    // Forma típica del modelo: [batch x (ctx*dim)] * [(ctx*dim) x hidden]
    const int M = 256, K = 1536, N = 512;
    auto A = be.alloc((size_t)M * K), B = be.alloc((size_t)K * N), C = be.alloc((size_t)M * N);
    std::vector<float> init((size_t)M * K, 0.01f);
    be.upload(*A, 0, init.data(), init.size());
    std::vector<float> initB((size_t)K * N, 0.02f);
    be.upload(*B, 0, initB.data(), initB.size());
    for (int i = 0; i < 2; ++i) be.gemm(false, false, M, N, K, 1.0f, Span(A.get()), Span(B.get()), 0.0f, Span(C.get()));  // calentamiento
    be.finish();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) be.gemm(false, false, M, N, K, 1.0f, Span(A.get()), Span(B.get()), 0.0f, Span(C.get()));
    be.finish();
    float probe = 0;
    be.download(*C, 0, &probe, 1);  // fuerza la sincronización
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return 2.0 * M * N * K * reps / secs / 1e9;
}

std::unique_ptr<Backend> selectBackend(const std::string& spec, std::string* report) {
    std::ostringstream rep;
    auto finish = [&](std::unique_ptr<Backend> b) {
        if (report) *report = rep.str();
        return b;
    };

    if (spec == "cpu") {
        auto b = makeCpuBackend();
        rep << "Dispositivo (forzado): " << b->name() << "\n";
        return finish(std::move(b));
    }
    if (spec.rfind("cl:", 0) == 0) {
        int p = 0, d = 0;
        if (std::sscanf(spec.c_str(), "cl:%d:%d", &p, &d) != 2) throw std::runtime_error("formato: cl:PLATAFORMA:DISPOSITIVO");
        auto b = makeOpenCLBackend(p, d);
        rep << "Dispositivo (forzado): " << b->name() << " [" << b->kind() << "]\n";
        return finish(std::move(b));
    }

    // auto / gpu: probar CPU y todas las GPU (dedicadas e integradas) y quedarse con la más rápida.
    struct Cand { std::unique_ptr<Backend> be; double gflops; };
    std::vector<Cand> cands;
    if (spec != "gpu") {
        auto cpu = makeCpuBackend();
        double g = benchmarkBackend(*cpu);
        rep << "  - " << cpu->name() << " [cpu]: " << g << " GFLOP/s\n";
        cands.push_back({std::move(cpu), g});
    }
    if (openclCompiledIn()) {
        for (const auto& d : listOpenCLDevices()) {
            if (d.kind == "cpu(opencl)") {
                rep << "  - " << d.name << " [cpu(opencl)]: omitido (la CPU nativa lo cubre; usa --device cl:" << d.platform << ":" << d.device << " para forzarlo)\n";
                continue;
            }
            try {
                auto be = makeOpenCLBackend(d.platform, d.device);
                double g = benchmarkBackend(*be);
                rep << "  - " << be->name() << " [" << d.kind << "]: " << g << " GFLOP/s\n";
                cands.push_back({std::move(be), g});
            } catch (const std::exception& e) {
                rep << "  - " << d.name << " [" << d.kind << "]: no utilizable (" << e.what() << ")\n";
            }
        }
    } else {
        rep << "  (compilado sin OpenCL: solo CPU)\n";
    }
    if (cands.empty()) {
        // "gpu" sin GPU disponible: caer a la CPU
        auto cpu = makeCpuBackend();
        rep << "  Sin GPU utilizable; se usa la CPU.\n";
        return finish(std::move(cpu));
    }
    size_t best = 0;
    for (size_t i = 1; i < cands.size(); ++i)
        if (cands[i].gflops > cands[best].gflops) best = i;
    rep << "Dispositivo elegido: " << cands[best].be->name() << " [" << cands[best].be->kind() << "]\n";
    return finish(std::move(cands[best].be));
}

}  // namespace orion
