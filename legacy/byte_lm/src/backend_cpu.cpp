// Backend de CPU: OpenMP + vectorización SIMD. Es la referencia numérica del proyecto.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "orion/backend.hpp"

#ifdef _OPENMP
#include <omp.h>
#endif

// MSVC solo implementa OpenMP 2.0 (sin `simd`): allí se usan solo `parallel for`/`reduction` y se deja
// la vectorización al compilador (/fp:fast). Con ORION_OMP2 se fuerza ese modo en cualquier compilador.
#if defined(_MSC_VER) && !defined(__clang__)
#define ORION_PRAGMA(x) __pragma(x)
#define ORION_OMP_HAS_SIMD 0
#else
#define ORION_PRAGMA(x) _Pragma(#x)
#ifdef ORION_OMP2
#define ORION_OMP_HAS_SIMD 0
#else
#define ORION_OMP_HAS_SIMD 1
#endif
#endif

#if ORION_OMP_HAS_SIMD
#define ORION_OMP_SIMD ORION_PRAGMA(omp simd)
#define ORION_OMP_SIMD_RED(...) ORION_PRAGMA(omp simd reduction(+ : __VA_ARGS__))
#define ORION_OMP_PARFOR_SIMD(cond) ORION_PRAGMA(omp parallel for simd schedule(static) if (cond))
#define ORION_OMP_PARFOR_RED(var, cond) ORION_PRAGMA(omp parallel for simd reduction(+ : var) schedule(static) if (cond))
#else
#define ORION_OMP_SIMD
#define ORION_OMP_SIMD_RED(...)
#define ORION_OMP_PARFOR_SIMD(cond) ORION_PRAGMA(omp parallel for schedule(static) if (cond))
#define ORION_OMP_PARFOR_RED(var, cond) ORION_PRAGMA(omp parallel for reduction(+ : var) schedule(static) if (cond))
#endif
#define ORION_OMP_PARFOR(cond) ORION_PRAGMA(omp parallel for schedule(static) if (cond))

namespace orion {
namespace {

struct CpuBuffer : Buffer {
    float* p = nullptr;
    explicit CpuBuffer(size_t n) {
        count = n;
        size_t bytes = ((n * sizeof(float) + 63) / 64) * 64;
        if (bytes == 0) bytes = 64;
        p = static_cast<float*>(::operator new(bytes, std::align_val_t(64)));
        std::memset(p, 0, bytes);
    }
    ~CpuBuffer() override { ::operator delete(p, std::align_val_t(64)); }
};

inline float* P(Span s) { return static_cast<CpuBuffer*>(s.buf)->p + s.off; }
inline const float* P(const Buffer& b) { return static_cast<const CpuBuffer&>(b).p; }
inline const int* I(const Buffer& b) { return reinterpret_cast<const int*>(P(b)); }

// C = alpha*op(A)*B + beta*C con B (KxN) row-major. Bloques de 4 filas de C.
void gemmKN(bool tA, int M, int N, int K, float alpha, const float* __restrict a,
            const float* __restrict b, float beta, float* __restrict c) {
    const int MB = 4;
ORION_OMP_PARFOR(1)
    for (int i0 = 0; i0 < M; i0 += MB) {
        const int mb = std::min(MB, M - i0);
        for (int r = 0; r < mb; ++r) {
            float* cr = c + (size_t)(i0 + r) * N;
            if (beta == 0.0f) {
                std::memset(cr, 0, sizeof(float) * N);
            } else if (beta != 1.0f) {
                for (int j = 0; j < N; ++j) cr[j] *= beta;
            }
        }
        if (mb == MB) {
            float* __restrict c0 = c + (size_t)i0 * N;
            float* __restrict c1 = c0 + N;
            float* __restrict c2 = c1 + N;
            float* __restrict c3 = c2 + N;
            for (int k = 0; k < K; ++k) {
                float a0, a1, a2, a3;
                if (tA) {
                    const float* ak = a + (size_t)k * M + i0;
                    a0 = alpha * ak[0]; a1 = alpha * ak[1]; a2 = alpha * ak[2]; a3 = alpha * ak[3];
                } else {
                    a0 = alpha * a[(size_t)(i0 + 0) * K + k];
                    a1 = alpha * a[(size_t)(i0 + 1) * K + k];
                    a2 = alpha * a[(size_t)(i0 + 2) * K + k];
                    a3 = alpha * a[(size_t)(i0 + 3) * K + k];
                }
                const float* __restrict bk = b + (size_t)k * N;
ORION_OMP_SIMD
                for (int j = 0; j < N; ++j) {
                    const float bv = bk[j];
                    c0[j] += a0 * bv; c1[j] += a1 * bv; c2[j] += a2 * bv; c3[j] += a3 * bv;
                }
            }
        } else {
            for (int r = 0; r < mb; ++r) {
                const int i = i0 + r;
                float* __restrict cr = c + (size_t)i * N;
                for (int k = 0; k < K; ++k) {
                    const float av = alpha * (tA ? a[(size_t)k * M + i] : a[(size_t)i * K + k]);
                    const float* __restrict bk = b + (size_t)k * N;
ORION_OMP_SIMD
                    for (int j = 0; j < N; ++j) cr[j] += av * bk[j];
                }
            }
        }
    }
}

// C = alpha*A*B^T + beta*C con B (NxK) row-major: producto punto fila·fila.
void gemmNT(int M, int N, int K, float alpha, const float* __restrict a, const float* __restrict b,
            float beta, float* __restrict c) {
    const int MB = 4;
ORION_OMP_PARFOR(1)
    for (int i0 = 0; i0 < M; i0 += MB) {
        const int mb = std::min(MB, M - i0);
        if (mb == MB) {
            const float* __restrict a0 = a + (size_t)i0 * K;
            const float* __restrict a1 = a0 + K;
            const float* __restrict a2 = a1 + K;
            const float* __restrict a3 = a2 + K;
            for (int j = 0; j < N; ++j) {
                const float* __restrict br = b + (size_t)j * K;
                float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
ORION_OMP_SIMD_RED(s0, s1, s2, s3)
                for (int k = 0; k < K; ++k) {
                    const float bv = br[k];
                    s0 += a0[k] * bv; s1 += a1[k] * bv; s2 += a2[k] * bv; s3 += a3[k] * bv;
                }
                float* cc = c + (size_t)i0 * N + j;
                const float s[4] = {s0, s1, s2, s3};
                for (int r = 0; r < 4; ++r) {
                    cc[(size_t)r * N] = alpha * s[r] + (beta == 0.0f ? 0.0f : beta * cc[(size_t)r * N]);
                }
            }
        } else {
            for (int r = 0; r < mb; ++r) {
                const float* __restrict ar = a + (size_t)(i0 + r) * K;
                for (int j = 0; j < N; ++j) {
                    const float* __restrict br = b + (size_t)j * K;
                    float s = 0;
ORION_OMP_SIMD_RED(s)
                    for (int k = 0; k < K; ++k) s += ar[k] * br[k];
                    float& cc = c[(size_t)(i0 + r) * N + j];
                    cc = alpha * s + (beta == 0.0f ? 0.0f : beta * cc);
                }
            }
        }
    }
}

class CpuBackend final : public Backend {
public:
    std::string name() const override {
#ifdef _OPENMP
        return "CPU nativa (" + std::to_string(omp_get_max_threads()) + " hilos, OpenMP+SIMD)";
#else
        return "CPU nativa (1 hilo, sin OpenMP)";
#endif
    }
    std::string kind() const override { return "cpu"; }

    std::unique_ptr<Buffer> alloc(size_t count) override { return std::make_unique<CpuBuffer>(count); }
    void upload(Buffer& dst, size_t off, const void* src, size_t n) override {
        std::memcpy(static_cast<CpuBuffer&>(dst).p + off, src, n * sizeof(float));
    }
    void download(const Buffer& src, size_t off, void* dst, size_t n) override {
        std::memcpy(dst, P(src) + off, n * sizeof(float));
    }

    void fill(Span x, size_t n, float v) override {
        float* p = P(x);
        if (v == 0.0f) { std::memset(p, 0, n * sizeof(float)); return; }
        for (size_t i = 0; i < n; ++i) p[i] = v;
    }
    void copy(Span src, Span dst, size_t n) override {
        std::memmove(P(dst), P(src), n * sizeof(float));
    }
    void axpy(float a, Span x, Span y, size_t n) override {
        const float* __restrict xp = P(x);
        float* __restrict yp = P(y);
ORION_OMP_PARFOR_SIMD(n > 65536)
        for (long long i = 0; i < (long long)n; ++i) yp[i] += a * xp[i];
    }
    void gemm(bool tA, bool tB, int M, int N, int K, float alpha, Span A, Span B, float beta,
              Span C) override {
        if (tB && tA) {  // no usado por el modelo; ruta simple
            const float* a = P(A); const float* b = P(B); float* c = P(C);
            for (int i = 0; i < M; ++i)
                for (int j = 0; j < N; ++j) {
                    float s = 0;
                    for (int k = 0; k < K; ++k) s += a[(size_t)k * M + i] * b[(size_t)j * K + k];
                    float& cc = c[(size_t)i * N + j];
                    cc = alpha * s + (beta == 0.0f ? 0.0f : beta * cc);
                }
        } else if (tB) {
            gemmNT(M, N, K, alpha, P(A), P(B), beta, P(C));
        } else {
            gemmKN(tA, M, N, K, alpha, P(A), P(B), beta, P(C));
        }
    }
    void gather(Span table, const Buffer& ids, Span out, int count, int dim) override {
        const float* t = P(table);
        float* o = P(out);
        const int* id = I(ids);
ORION_OMP_PARFOR(count > 256)
        for (int i = 0; i < count; ++i)
            std::memcpy(o + (size_t)i * dim, t + (size_t)id[i] * dim, sizeof(float) * dim);
    }
    void biasAct(Span z, Span bias, int rows, int cols, bool relu) override {
        float* zp = P(z);
        const float* bp = P(bias);
ORION_OMP_PARFOR((size_t)rows * cols > 32768)
        for (int r = 0; r < rows; ++r) {
            float* row = zp + (size_t)r * cols;
            if (relu) {
ORION_OMP_SIMD
                for (int c = 0; c < cols; ++c) { const float v = row[c] + bp[c]; row[c] = v > 0 ? v : 0; }
            } else {
ORION_OMP_SIMD
                for (int c = 0; c < cols; ++c) row[c] += bp[c];
            }
        }
    }
    void reluGrad(Span g, Span act, size_t n) override {
        float* gp = P(g);
        const float* ap = P(act);
ORION_OMP_PARFOR_SIMD(n > 65536)
        for (long long i = 0; i < (long long)n; ++i) if (ap[i] <= 0) gp[i] = 0;
    }
    void colSum(Span g, Span out, int rows, int cols) override {
        const float* gp = P(g);
        float* op = P(out);
        std::memset(op, 0, sizeof(float) * cols);
        for (int r = 0; r < rows; ++r) {
            const float* row = gp + (size_t)r * cols;
ORION_OMP_SIMD
            for (int c = 0; c < cols; ++c) op[c] += row[c];
        }
    }
    void softmaxXent(Span logits, const Buffer& targets, Span dlogits, Span loss, int rows,
                     int cols, float scale, bool wantGrad) override {
        const float* lp = P(logits);
        const int* tp = I(targets);
        float* dp = wantGrad ? P(dlogits) : nullptr;
        float* lo = P(loss);
ORION_OMP_PARFOR(rows > 16)
        for (int r = 0; r < rows; ++r) {
            const float* row = lp + (size_t)r * cols;
            float mx = row[0];
            for (int c = 1; c < cols; ++c) mx = std::max(mx, row[c]);
            float sum = 0;
            for (int c = 0; c < cols; ++c) sum += std::exp(row[c] - mx);
            const int t = tp[r];
            lo[r] = std::log(sum) - (row[t] - mx);
            if (dp) {
                float* d = dp + (size_t)r * cols;
                const float inv = 1.0f / sum;
                for (int c = 0; c < cols; ++c) d[c] = std::exp(row[c] - mx) * inv * scale;
                d[t] -= scale;
            }
        }
    }
    void embedGrad(Span dx, const Buffer& ids, Span dtable, int count, int dim, int vocab) override {
        (void)vocab;
        const float* d = P(dx);
        float* t = P(dtable);
        const int* id = I(ids);
        for (int i = 0; i < count; ++i) {
            float* row = t + (size_t)id[i] * dim;
            const float* src = d + (size_t)i * dim;
            for (int j = 0; j < dim; ++j) row[j] += src[j];
        }
    }
    float sumSq(Span x, size_t n) override {
        const float* p = P(x);
        double s = 0;
ORION_OMP_PARFOR_RED(s, n > 65536)
        for (long long i = 0; i < (long long)n; ++i) s += (double)p[i] * p[i];
        return (float)s;
    }
    void adam(Span p, Span g, Span m, Span v, size_t n, float lr, float b1, float b2, float eps,
              float wd, int t, float gscale) override {
        float* pp = P(p); const float* gp = P(g); float* mp = P(m); float* vp = P(v);
        const float c1 = 1.0f / (1.0f - std::pow(b1, (float)t));
        const float c2 = 1.0f / (1.0f - std::pow(b2, (float)t));
ORION_OMP_PARFOR_SIMD(n > 65536)
        for (long long i = 0; i < (long long)n; ++i) {
            const float gr = gp[i] * gscale;
            const float mm = b1 * mp[i] + (1.0f - b1) * gr;
            const float vv = b2 * vp[i] + (1.0f - b2) * gr * gr;
            mp[i] = mm; vp[i] = vv;
            const float upd = (mm * c1) / (std::sqrt(vv * c2) + eps);
            pp[i] -= lr * (upd + wd * pp[i]);
        }
    }
};

}  // namespace

std::unique_ptr<Backend> makeCpuBackend() { return std::make_unique<CpuBackend>(); }

}  // namespace orion
