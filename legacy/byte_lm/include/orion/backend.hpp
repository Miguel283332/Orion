// Capa de cómputo de Orion: la misma interfaz para CPU (OpenMP/SIMD) y GPU (OpenCL).
// El modelo solo habla con `Backend`; el resto del programa no sabe en qué hardware corre.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace orion {

// Bloque de memoria del dispositivo (elementos de 4 bytes: float o int32).
struct Buffer {
    size_t count = 0;
    virtual ~Buffer() = default;
};

// Vista (buffer + desplazamiento en elementos) sobre un Buffer de floats.
struct Span {
    Buffer* buf;
    size_t off;
    Span(Buffer* b, size_t o = 0) : buf(b), off(o) {}
    Span at(size_t extra) const { return Span(buf, off + extra); }
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;
    virtual std::string kind() const = 0;  // "cpu" | "gpu" | "igpu" | "cpu(opencl)"

    virtual std::unique_ptr<Buffer> alloc(size_t count) = 0;  // inicializado a cero
    virtual void upload(Buffer& dst, size_t dstOff, const void* src, size_t count) = 0;
    virtual void download(const Buffer& src, size_t srcOff, void* dst, size_t count) = 0;
    virtual void finish() {}

    // --- operaciones (todas row-major) ---
    virtual void fill(Span x, size_t n, float v) = 0;
    virtual void copy(Span src, Span dst, size_t n) = 0;
    virtual void axpy(float a, Span x, Span y, size_t n) = 0;  // y += a*x
    // C[MxN] = alpha*op(A)[MxK]*op(B)[KxN] + beta*C ; A es KxM si tA ; B es NxK si tB
    virtual void gemm(bool tA, bool tB, int M, int N, int K, float alpha, Span A, Span B,
                      float beta, Span C) = 0;
    // out[i,:] = table[ids[i],:]
    virtual void gather(Span table, const Buffer& ids, Span out, int count, int dim) = 0;
    // z[r,c] = act(z[r,c] + bias[c])
    virtual void biasAct(Span z, Span bias, int rows, int cols, bool relu) = 0;
    virtual void reluGrad(Span g, Span act, size_t n) = 0;  // g *= (act > 0)
    virtual void colSum(Span g, Span out, int rows, int cols) = 0;
    // loss[r] = -log softmax(logits[r])[target[r]] ; dlogits = (p - onehot) * scale
    virtual void softmaxXent(Span logits, const Buffer& targets, Span dlogits, Span loss, int rows,
                             int cols, float scale, bool wantGrad) = 0;
    // dtable[ids[i],:] += dx[i,:]
    virtual void embedGrad(Span dx, const Buffer& ids, Span dtable, int count, int dim,
                           int vocab) = 0;
    virtual float sumSq(Span x, size_t n) = 0;
    virtual void adam(Span p, Span g, Span m, Span v, size_t n, float lr, float b1, float b2,
                      float eps, float wd, int t, float gscale) = 0;
};

struct ClDeviceInfo {
    int platform = 0, device = 0;
    std::string name, vendor, kind;  // kind: gpu | igpu | cpu(opencl)
    unsigned computeUnits = 0;
    uint64_t memBytes = 0;
};

std::unique_ptr<Backend> makeCpuBackend();
bool openclCompiledIn();
std::vector<ClDeviceInfo> listOpenCLDevices();                         // vacío si no hay OpenCL
std::unique_ptr<Backend> makeOpenCLBackend(int platform, int device);  // lanza si falla

// Mide GFLOP/s de un gemm representativo del modelo.
double benchmarkBackend(Backend& be, int reps = 6);

// spec: "auto" | "cpu" | "gpu" | "cl:P:D". Escribe un informe legible en *report.
std::unique_ptr<Backend> selectBackend(const std::string& spec, std::string* report);

}  // namespace orion
