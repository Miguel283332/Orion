// Backend OpenCL: GPU dedicada (NVIDIA/AMD/Intel Arc), GPU integrada (Intel/AMD APU) o
// cualquier dispositivo OpenCL. Si Orion se compila sin OpenCL, este archivo solo aporta stubs.
#include "orion/backend.hpp"

#ifdef ORION_HAS_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace orion {
namespace {

const char* kKernelSource = R"CLC(
#define TS 16
__kernel void k_gemm(int tA, int tB, int M, int N, int K, float alpha,
                     __global const float* A, int oA, __global const float* B, int oB,
                     float beta, __global float* C, int oC) {
    __local float As[TS][TS];
    __local float Bs[TS][TS];
    const int tx = get_local_id(0), ty = get_local_id(1);
    const int row = get_group_id(1) * TS + ty;
    const int col = get_group_id(0) * TS + tx;
    float acc = 0.0f;
    for (int t = 0; t < K; t += TS) {
        const int ak = t + tx;
        float av = 0.0f;
        if (row < M && ak < K) av = tA ? A[oA + ak * M + row] : A[oA + row * K + ak];
        const int bk = t + ty;
        float bv = 0.0f;
        if (bk < K && col < N) bv = tB ? B[oB + col * K + bk] : B[oB + bk * N + col];
        As[ty][tx] = av;
        Bs[ty][tx] = bv;
        barrier(CLK_LOCAL_MEM_FENCE);
        #pragma unroll
        for (int k = 0; k < TS; ++k) acc += As[ty][k] * Bs[k][tx];
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (row < M && col < N) {
        float c = alpha * acc;
        if (beta != 0.0f) c += beta * C[oC + row * N + col];
        C[oC + row * N + col] = c;
    }
}
__kernel void k_fill(__global float* x, int off, int n, float v) {
    int i = get_global_id(0); if (i < n) x[off + i] = v;
}
__kernel void k_copy(__global const float* s, int so, __global float* d, int dof, int n) {
    int i = get_global_id(0); if (i < n) d[dof + i] = s[so + i];
}
__kernel void k_axpy(float a, __global const float* x, int xo, __global float* y, int yo, int n) {
    int i = get_global_id(0); if (i < n) y[yo + i] += a * x[xo + i];
}
__kernel void k_gather(__global const float* t, int to, __global const int* ids,
                       __global float* o, int oo, int count, int dim) {
    int i = get_global_id(0);
    if (i < count * dim) { int r = i / dim, j = i - r * dim; o[oo + i] = t[to + ids[r] * dim + j]; }
}
__kernel void k_biasact(__global float* z, int zo, __global const float* b, int bo,
                        int rows, int cols, int relu) {
    int i = get_global_id(0);
    if (i < rows * cols) {
        float v = z[zo + i] + b[bo + (i % cols)];
        if (relu && v < 0.0f) v = 0.0f;
        z[zo + i] = v;
    }
}
__kernel void k_relugrad(__global float* g, int go, __global const float* a, int ao, int n) {
    int i = get_global_id(0); if (i < n && a[ao + i] <= 0.0f) g[go + i] = 0.0f;
}
__kernel void k_colsum(__global const float* g, int go, __global float* o, int oo, int rows, int cols) {
    int c = get_global_id(0);
    if (c < cols) { float s = 0.0f; for (int r = 0; r < rows; ++r) s += g[go + r * cols + c]; o[oo + c] = s; }
}
__kernel void k_softmax(__global const float* l, int lo, __global const int* t,
                        __global float* dl, int dlo, __global float* loss, int lso,
                        int rows, int cols, float scale, int wantGrad) {
    int r = get_global_id(0);
    if (r >= rows) return;
    __global const float* row = l + lo + r * cols;
    float mx = row[0];
    for (int c = 1; c < cols; ++c) mx = fmax(mx, row[c]);
    float sum = 0.0f;
    for (int c = 0; c < cols; ++c) sum += exp(row[c] - mx);
    int tg = t[r];
    loss[lso + r] = log(sum) - (row[tg] - mx);
    if (wantGrad) {
        float inv = 1.0f / sum;
        for (int c = 0; c < cols; ++c) dl[dlo + r * cols + c] = exp(row[c] - mx) * inv * scale;
        dl[dlo + r * cols + tg] -= scale;
    }
}
__kernel void k_embedgrad(__global const float* dx, int dxo, __global const int* ids,
                          __global float* dt, int dto, int count, int dim, int vocab) {
    int i = get_global_id(0);
    if (i >= vocab * dim) return;
    int v = i / dim, j = i - v * dim;
    float s = 0.0f;
    for (int n = 0; n < count; ++n) if (ids[n] == v) s += dx[dxo + n * dim + j];
    dt[dto + i] += s;
}
__kernel void k_sumsq(__global const float* x, int xo, int n, __global float* part, __local float* sh) {
    int lid = get_local_id(0);
    float s = 0.0f;
    for (int i = get_global_id(0); i < n; i += get_global_size(0)) { float v = x[xo + i]; s += v * v; }
    sh[lid] = s;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int st = get_local_size(0) / 2; st > 0; st >>= 1) {
        if (lid < st) sh[lid] += sh[lid + st];
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (lid == 0) part[get_group_id(0)] = sh[0];
}
__kernel void k_adam(__global float* p, int po, __global const float* g, int go,
                     __global float* m, int mo, __global float* v, int vo, int n,
                     float lr, float b1, float b2, float eps, float wd, float c1, float c2, float gs) {
    int i = get_global_id(0);
    if (i >= n) return;
    float gr = g[go + i] * gs;
    float mm = b1 * m[mo + i] + (1.0f - b1) * gr;
    float vv = b2 * v[vo + i] + (1.0f - b2) * gr * gr;
    m[mo + i] = mm; v[vo + i] = vv;
    float pv = p[po + i];
    pv -= lr * ((mm * c1) / (sqrt(vv * c2) + eps) + wd * pv);
    p[po + i] = pv;
}
)CLC";

void check(cl_int e, const char* what) {
    if (e != CL_SUCCESS) throw std::runtime_error(std::string("OpenCL: ") + what + " (código " + std::to_string(e) + ")");
}

std::string devStr(cl_device_id d, cl_device_info p) {
    size_t n = 0;
    clGetDeviceInfo(d, p, 0, nullptr, &n);
    std::string s(n, '\0');
    clGetDeviceInfo(d, p, n, s.data(), nullptr);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

struct ClBuffer : Buffer {
    cl_mem mem = nullptr;
    ClBuffer(cl_context ctx, size_t n) {
        count = n;
        cl_int e;
        mem = clCreateBuffer(ctx, CL_MEM_READ_WRITE, std::max<size_t>(n, 1) * 4, nullptr, &e);
        check(e, "clCreateBuffer");
    }
    ~ClBuffer() override { if (mem) clReleaseMemObject(mem); }
};

struct Found { cl_platform_id plat; cl_device_id dev; ClDeviceInfo info; };

std::vector<Found> enumerate() {
    std::vector<Found> out;
    cl_uint np = 0;
    if (clGetPlatformIDs(0, nullptr, &np) != CL_SUCCESS || np == 0) return out;
    std::vector<cl_platform_id> plats(np);
    clGetPlatformIDs(np, plats.data(), nullptr);
    for (cl_uint pi = 0; pi < np; ++pi) {
        cl_uint nd = 0;
        if (clGetDeviceIDs(plats[pi], CL_DEVICE_TYPE_ALL, 0, nullptr, &nd) != CL_SUCCESS || nd == 0) continue;
        std::vector<cl_device_id> devs(nd);
        clGetDeviceIDs(plats[pi], CL_DEVICE_TYPE_ALL, nd, devs.data(), nullptr);
        for (cl_uint di = 0; di < nd; ++di) {
            Found f;
            f.plat = plats[pi]; f.dev = devs[di];
            f.info.platform = (int)pi; f.info.device = (int)di;
            f.info.name = devStr(devs[di], CL_DEVICE_NAME);
            f.info.vendor = devStr(devs[di], CL_DEVICE_VENDOR);
            cl_device_type type = 0; cl_bool unified = CL_FALSE;
            cl_uint cu = 0; cl_ulong mem = 0;
            clGetDeviceInfo(devs[di], CL_DEVICE_TYPE, sizeof(type), &type, nullptr);
            clGetDeviceInfo(devs[di], CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof(unified), &unified, nullptr);
            clGetDeviceInfo(devs[di], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cu), &cu, nullptr);
            clGetDeviceInfo(devs[di], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(mem), &mem, nullptr);
            f.info.computeUnits = cu; f.info.memBytes = mem;
            if (type & CL_DEVICE_TYPE_GPU) f.info.kind = unified ? "igpu" : "gpu";
            else f.info.kind = "cpu(opencl)";
            out.push_back(f);
        }
    }
    return out;
}

class ClBackend final : public Backend {
public:
    ClBackend(const Found& f) : info_(f.info) {
        cl_int e;
        dev_ = f.dev;
        size_t maxWg = 0;
        clGetDeviceInfo(dev_, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(maxWg), &maxWg, nullptr);
        if (maxWg < 256) throw std::runtime_error("dispositivo con work-group < 256");
        ctx_ = clCreateContext(nullptr, 1, &dev_, nullptr, nullptr, &e);
        check(e, "clCreateContext");
        q_ = clCreateCommandQueue(ctx_, dev_, 0, &e);
        check(e, "clCreateCommandQueue");
        prog_ = clCreateProgramWithSource(ctx_, 1, &kKernelSource, nullptr, &e);
        check(e, "clCreateProgramWithSource");
        e = clBuildProgram(prog_, 1, &dev_, "-cl-fast-relaxed-math", nullptr, nullptr);
        if (e != CL_SUCCESS) {
            size_t n = 0;
            clGetProgramBuildInfo(prog_, dev_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &n);
            std::string log(n, '\0');
            clGetProgramBuildInfo(prog_, dev_, CL_PROGRAM_BUILD_LOG, n, log.data(), nullptr);
            throw std::runtime_error("OpenCL: fallo al compilar kernels:\n" + log);
        }
        auto mk = [&](const char* n) { cl_kernel k = clCreateKernel(prog_, n, &e); check(e, n); return k; };
        gemm_ = mk("k_gemm"); fill_ = mk("k_fill"); copy_ = mk("k_copy"); axpy_ = mk("k_axpy");
        gather_ = mk("k_gather"); bias_ = mk("k_biasact"); relug_ = mk("k_relugrad");
        colsum_ = mk("k_colsum"); soft_ = mk("k_softmax"); embg_ = mk("k_embedgrad");
        sumsq_ = mk("k_sumsq"); adam_ = mk("k_adam");
        part_ = std::make_unique<ClBuffer>(ctx_, kGroups);
    }
    ~ClBackend() override {
        part_.reset();
        for (cl_kernel k : {gemm_, fill_, copy_, axpy_, gather_, bias_, relug_, colsum_, soft_, embg_, sumsq_, adam_})
            if (k) clReleaseKernel(k);
        if (prog_) clReleaseProgram(prog_);
        if (q_) clReleaseCommandQueue(q_);
        if (ctx_) clReleaseContext(ctx_);
    }

    std::string name() const override { return info_.name + " [" + info_.vendor + ", OpenCL, " + std::to_string(info_.computeUnits) + " CU]"; }
    std::string kind() const override { return info_.kind; }

    std::unique_ptr<Buffer> alloc(size_t count) override {
        auto b = std::make_unique<ClBuffer>(ctx_, count);
        float z = 0.0f;
        check(clEnqueueFillBuffer(q_, b->mem, &z, sizeof(z), 0, std::max<size_t>(count, 1) * 4, 0, nullptr, nullptr), "fill");
        return b;
    }
    void upload(Buffer& dst, size_t off, const void* src, size_t n) override {
        if (n == 0) return;
        check(clEnqueueWriteBuffer(q_, M(dst), CL_TRUE, off * 4, n * 4, src, 0, nullptr, nullptr), "write");
    }
    void download(const Buffer& src, size_t off, void* dst, size_t n) override {
        if (n == 0) return;
        check(clEnqueueReadBuffer(q_, M(src), CL_TRUE, off * 4, n * 4, dst, 0, nullptr, nullptr), "read");
    }
    void finish() override { clFinish(q_); }

    void fill(Span x, size_t n, float v) override {
        run1(fill_, n, M(x), I(x), (int)n, v);
    }
    void copy(Span s, Span d, size_t n) override { run1(copy_, n, M(s), I(s), M(d), I(d), (int)n); }
    void axpy(float a, Span x, Span y, size_t n) override { run1(axpy_, n, a, M(x), I(x), M(y), I(y), (int)n); }
    void gemm(bool tA, bool tB, int m, int n, int k, float alpha, Span A, Span B, float beta, Span C) override {
        size_t local[2] = {16, 16};
        size_t global[2] = {round(n, 16), round(m, 16)};
        setArgs(gemm_, (int)tA, (int)tB, m, n, k, alpha, M(A), I(A), M(B), I(B), beta, M(C), I(C));
        check(clEnqueueNDRangeKernel(q_, gemm_, 2, nullptr, global, local, 0, nullptr, nullptr), "gemm");
    }
    void gather(Span t, const Buffer& ids, Span o, int count, int dim) override {
        run1(gather_, (size_t)count * dim, M(t), I(t), M(ids), M(o), I(o), count, dim);
    }
    void biasAct(Span z, Span b, int rows, int cols, bool relu) override {
        run1(bias_, (size_t)rows * cols, M(z), I(z), M(b), I(b), rows, cols, (int)relu);
    }
    void reluGrad(Span g, Span a, size_t n) override { run1(relug_, n, M(g), I(g), M(a), I(a), (int)n); }
    void colSum(Span g, Span o, int rows, int cols) override {
        run1(colsum_, (size_t)cols, M(g), I(g), M(o), I(o), rows, cols);
    }
    void softmaxXent(Span l, const Buffer& t, Span dl, Span loss, int rows, int cols, float scale, bool wantGrad) override {
        run1(soft_, (size_t)rows, M(l), I(l), M(t), M(wantGrad ? dl : l), wantGrad ? I(dl) : 0,
             M(loss), I(loss), rows, cols, scale, (int)wantGrad);
    }
    void embedGrad(Span dx, const Buffer& ids, Span dt, int count, int dim, int vocab) override {
        run1(embg_, (size_t)vocab * dim, M(dx), I(dx), M(ids), M(dt), I(dt), count, dim, vocab);
    }
    float sumSq(Span x, size_t n) override {
        size_t local = 64, global = kGroups * local;
        setArgs(sumsq_, M(x), I(x), (int)n, M(*part_));
        check(clSetKernelArg(sumsq_, 4, local * sizeof(float), nullptr), "arg local");
        check(clEnqueueNDRangeKernel(q_, sumsq_, 1, nullptr, &global, &local, 0, nullptr, nullptr), "sumsq");
        float h[kGroups];
        download(*part_, 0, h, kGroups);
        double s = 0;
        for (float v : h) s += v;
        return (float)s;
    }
    void adam(Span p, Span g, Span m, Span v, size_t n, float lr, float b1, float b2, float eps,
              float wd, int t, float gscale) override {
        const float c1 = 1.0f / (1.0f - std::pow(b1, (float)t));
        const float c2 = 1.0f / (1.0f - std::pow(b2, (float)t));
        run1(adam_, n, M(p), I(p), M(g), I(g), M(m), I(m), M(v), I(v), (int)n, lr, b1, b2, eps, wd, c1, c2, gscale);
    }

private:
    static constexpr size_t kGroups = 64;
    static cl_mem M(const Buffer& b) { return static_cast<const ClBuffer&>(b).mem; }
    static cl_mem M(Span s) { return static_cast<ClBuffer*>(s.buf)->mem; }
    static int I(Span s) { return (int)s.off; }
    static size_t round(size_t v, size_t m) { return (v + m - 1) / m * m; }

    template <class T> void arg(cl_kernel k, cl_uint i, const T& v) { check(clSetKernelArg(k, i, sizeof(T), &v), "clSetKernelArg"); }
    template <class... A> void setArgs(cl_kernel k, const A&... a) { cl_uint i = 0; (arg(k, i++, a), ...); }
    template <class... A> void run1(cl_kernel k, size_t n, const A&... a) {
        if (n == 0) return;
        setArgs(k, a...);
        size_t local = 64, global = round(n, local);
        check(clEnqueueNDRangeKernel(q_, k, 1, nullptr, &global, &local, 0, nullptr, nullptr), "enqueue");
    }

    ClDeviceInfo info_;
    cl_device_id dev_ = nullptr;
    cl_context ctx_ = nullptr;
    cl_command_queue q_ = nullptr;
    cl_program prog_ = nullptr;
    cl_kernel gemm_ = nullptr, fill_ = nullptr, copy_ = nullptr, axpy_ = nullptr, gather_ = nullptr,
              bias_ = nullptr, relug_ = nullptr, colsum_ = nullptr, soft_ = nullptr, embg_ = nullptr,
              sumsq_ = nullptr, adam_ = nullptr;
    std::unique_ptr<ClBuffer> part_;
};

}  // namespace

bool openclCompiledIn() { return true; }

std::vector<ClDeviceInfo> listOpenCLDevices() {
    std::vector<ClDeviceInfo> v;
    for (auto& f : enumerate()) v.push_back(f.info);
    return v;
}

std::unique_ptr<Backend> makeOpenCLBackend(int platform, int device) {
    for (auto& f : enumerate())
        if (f.info.platform == platform && f.info.device == device) return std::make_unique<ClBackend>(f);
    throw std::runtime_error("OpenCL: dispositivo " + std::to_string(platform) + ":" + std::to_string(device) + " no encontrado");
}

}  // namespace orion

#else  // sin OpenCL

#include <stdexcept>
namespace orion {
bool openclCompiledIn() { return false; }
std::vector<ClDeviceInfo> listOpenCLDevices() { return {}; }
std::unique_ptr<Backend> makeOpenCLBackend(int, int) {
    throw std::runtime_error("Orion se compiló sin OpenCL (-DORION_OPENCL=ON y OpenCL instalado)");
}
}  // namespace orion

#endif
