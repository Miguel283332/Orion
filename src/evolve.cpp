#include "orion/evolve.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace orion {

enum OpId { FINETUNE, LR_UP, LR_DOWN, WD_UP, WD_DOWN, DROP_UP, DROP_DOWN, MUTATE, GROW_HIDDEN, GROW_CTX, BATCH_UP, N_OPS };

// ---------------------------------------------------------------- Learner

float Learner::train(int steps, const std::function<void()>& yield, const std::atomic<bool>* stop) {
    std::vector<int> ids, tgt;
    const int B = model_.hp().batch;
    double tail = 0;
    int tailN = 0;
    for (int s = 0; s < steps; ++s) {
        if (stop && stop->load()) break;
        data_.sample(Split::Train, B, model_.genome().ctx, rng_, ids, tgt);
        const float cd = model_.hp().ctxDrop;
        if (cd > 0) {
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            for (int& id : ids) if (u(rng_) < cd) id = 0;  // 0 = relleno
        }
        float loss = model_.trainStep(ids, tgt, B);
        if (s >= steps - std::max(1, steps / 5)) { tail += loss; ++tailN; }
        if (yield) yield();
    }
    return tailN ? (float)(tail / tailN) : 0.0f;
}

float Learner::evalSplit(Split s, size_t maxN) {
    std::vector<int> ids, tgt;
    const int ctx = model_.genome().ctx;
    data_.evalSet(s, ctx, maxN, ids, tgt);
    const int n = (int)tgt.size();
    if (n == 0) return 0.0f;
    const int chunk = 256;
    double total = 0;
    for (int i = 0; i < n; i += chunk) {
        const int b = std::min(chunk, n - i);
        std::vector<int> ci(ids.begin() + (size_t)i * ctx, ids.begin() + (size_t)(i + b) * ctx);
        std::vector<int> ct(tgt.begin() + i, tgt.begin() + i + b);
        total += (double)model_.evalLoss(ci, ct, b) * b;
    }
    return (float)(total / n);
}

// ---------------------------------------------------------------- Evolver

Evolver::Evolver(Model& model, Dataset& data, Learner& learner, EvolveConfig cfg, std::string homeDir)
    : model_(model), data_(data), learner_(learner), cfg_(cfg), home_(std::move(homeDir)), rng_(0xC0FFEEULL) {
    const char* names[N_OPS] = {"finetune", "lr_up", "lr_down", "wd_up", "wd_down", "drop_up", "drop_down", "mutate", "grow_hidden", "grow_ctx", "batch_up"};
    for (int i = 0; i < N_OPS; ++i) {
        Op o;
        o.name = names[i];
        o.growth = (i == GROW_HIDDEN || i == GROW_CTX);
        ops_.push_back(o);
    }
    ops_[FINETUNE].gain = 5e-3;  // el entrenamiento normal es el candidato por defecto
    rebaseline();
}

void Evolver::rebaseline() { bestVal_ = learner_.evalVal(cfg_.evalMax); }

int Evolver::chooseOp(bool stagnating) {
    std::vector<double> w(N_OPS, 0.0);
    const Genome& g = model_.genome();
    for (int i = 0; i < N_OPS; ++i) {
        double gain = ops_[i].gain + 1e-5;
        if (i == GROW_HIDDEN && (!stagnating || g.hidden >= cfg_.maxHidden)) gain = 0;
        if (i == GROW_CTX && (!stagnating || g.ctx >= cfg_.maxCtx)) gain = 0;
        if (i == BATCH_UP && model_.hp().batch >= cfg_.maxBatch) gain = 0;
        w[i] = gain;
    }
    std::uniform_real_distribution<double> u(0, 1);
    if (u(rng_) < 0.25) {  // exploración uniforme entre los operadores habilitados
        for (double& x : w) x = x > 0 ? 1.0 : 0.0;
    }
    std::discrete_distribution<int> d(w.begin(), w.end());
    return d(rng_);
}

void Evolver::apply(int op, std::string& detail, const std::function<void()>& yield, const std::atomic<bool>* stop) {
    HyperParams& hp = model_.hp();
    int steps = cfg_.stepsPerGen;
    std::ostringstream os;
    switch (op) {
        case FINETUNE: break;
        case LR_UP: hp.lr = std::min(hp.lr * 1.6f, 1e-2f); os << "lr=" << hp.lr; break;
        case LR_DOWN: hp.lr = std::max(hp.lr * 0.6f, 2e-5f); os << "lr=" << hp.lr; break;
        case WD_UP: hp.wd = std::min(std::max(hp.wd, 1e-5f) * 3.0f, 0.1f); os << "wd=" << hp.wd; break;
        case WD_DOWN: hp.wd = hp.wd * 0.3f; os << "wd=" << hp.wd; break;
        case DROP_UP: hp.ctxDrop = std::min(0.6f, hp.ctxDrop + 0.05f); os << "ctxDrop=" << hp.ctxDrop; break;
        case DROP_DOWN: hp.ctxDrop = std::max(0.0f, hp.ctxDrop - 0.05f); os << "ctxDrop=" << hp.ctxDrop; break;
        case BATCH_UP: hp.batch = std::min(cfg_.maxBatch, hp.batch * 2); os << "batch=" << hp.batch; break;
        case MUTATE: {
            // Toca directamente una fracción de los pesos (como editar parámetros de un modelo
            // existente) y luego repara con un entrenamiento corto. Si no mejora -> rollback.
            static const double fracs[] = {0.002, 0.02, 0.1};
            static const double sigmas[] = {0.05, 0.2};
            std::uniform_int_distribution<int> fi(0, 2), si(0, 1);
            const double frac = fracs[fi(rng_)], sig = sigmas[si(rng_)];
            State s = model_.snapshot();
            Layout L(s.genome);
            const size_t starts[] = {L.E, L.W1, L.b1, L.W2, L.b2, L.W3, L.b3, L.total};
            std::normal_distribution<float> nd(0.0f, 1.0f);
            std::uniform_real_distribution<double> u(0, 1);
            size_t touched = 0;
            for (int seg = 0; seg < 7; ++seg) {
                double ss = 0;
                for (size_t i = starts[seg]; i < starts[seg + 1]; ++i) ss += (double)s.p[i] * s.p[i];
                double rms = std::sqrt(ss / std::max<size_t>(1, starts[seg + 1] - starts[seg]));
                if (rms < 1e-3) rms = 0.01;
                for (size_t i = starts[seg]; i < starts[seg + 1]; ++i)
                    if (u(rng_) < frac) { s.p[i] += (float)(nd(rng_) * sig * rms); ++touched; }
            }
            model_.restore(s);
            steps = std::max(10, steps / 3);
            os << "tocados=" << touched << " frac=" << frac << " sigma=" << sig;
            break;
        }
        case GROW_HIDDEN: {
            State s = model_.snapshot();
            int nh = std::min(cfg_.maxHidden, ((int)(s.genome.hidden * 1.25) + 15) / 16 * 16);
            model_.restore(growState(s, nh, s.genome.ctx, rng_()));
            os << "hidden " << s.genome.hidden << "->" << nh;
            break;
        }
        case GROW_CTX: {
            State s = model_.snapshot();
            int nc = std::min(cfg_.maxCtx, s.genome.ctx + 16);
            model_.restore(growState(s, s.genome.hidden, nc, rng_()));
            os << "ctx " << s.genome.ctx << "->" << nc;
            break;
        }
    }
    detail = os.str();
    learner_.train(steps, yield, stop);
}

GenResult Evolver::step(const std::function<void()>& yield, const std::atomic<bool>* stop) {
    GenResult r;
    r.gen = ++gen_;
    if (data_.refresh()) {  // datos nuevos: la validación cambia, hay que recalcular la línea base
        rebaseline();
    }
    const bool stagnating = stagnation_ >= 2;
    const int op = chooseOp(stagnating);
    r.op = ops_[op].name;
    r.before = bestVal_;

    auto t0 = std::chrono::steady_clock::now();
    State snap = model_.snapshot();
    apply(op, r.detail, yield, stop);
    r.aborted = stop && stop->load();
    if (!r.aborted) r.after = learner_.evalVal(cfg_.evalMax);
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Crecer cuesta cómputo: exigir que se pague solo con una mejora mayor.
    const double need = cfg_.minRelGain * (ops_[op].growth || op == BATCH_UP ? 3.0 : 1.0);
    r.accepted = !r.aborted && std::isfinite(r.after) && r.after < bestVal_ * (1.0 - need);

    Op& o = ops_[op];
    o.tries++;
    if (r.accepted) {
        o.wins++;
        const double gainPerSec = (bestVal_ - r.after) / std::max(0.05, r.secs);
        o.gain = 0.6 * o.gain + 0.4 * gainPerSec;
        bestVal_ = r.after;
        saveCheckpoint();
    } else {
        o.gain = 0.7 * o.gain;  // decae; la exploración ε lo vuelve a probar de vez en cuando
        model_.restore(snap);   // rollback exacto
        if (!r.aborted) stagnation_++;
    }
    if (r.accepted) stagnation_ = 0;
    if (!r.aborted) log(r);
    return r;
}

std::string Evolver::opTable() const {
    std::ostringstream os;
    os << std::left << std::setw(13) << "operador" << std::setw(8) << "pruebas" << std::setw(8) << "éxitos"
       << "ganancia/s\n";
    for (const auto& o : ops_)
        os << std::left << std::setw(13) << o.name << std::setw(8) << o.tries << std::setw(8) << o.wins << o.gain << "\n";
    return os.str();
}

void Evolver::log(const GenResult& r) const {
    std::error_code ec;
    fs::create_directories(fs::path(home_) / "memory", ec);
    std::ofstream out(fs::path(home_) / "memory" / "evolution.log", std::ios::app);
    const Genome& g = model_.genome();
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    out << ts << " gen=" << r.gen << " op=" << r.op << " [" << r.detail << "] val " << r.before << " -> "
        << r.after << (r.accepted ? " ACEPTADA" : " rechazada") << " hidden=" << g.hidden << " ctx=" << g.ctx
        << " params=" << model_.paramCount() << " lr=" << model_.hp().lr << " wd=" << model_.hp().wd << " drop=" << model_.hp().ctxDrop << " " << r.secs << "s\n";
}

void Evolver::saveCheckpoint() const {
    std::error_code ec;
    fs::create_directories(fs::path(home_) / "checkpoints", ec);
    State s = const_cast<Model&>(model_).snapshot();
    saveState(s, (fs::path(home_) / "checkpoints" / "best.orn").string());
    std::ofstream out(fs::path(home_) / "checkpoints" / "evolution.state");
    out << "gen " << gen_ << "\nbest " << bestVal_ << "\n";
    for (const auto& o : ops_) out << "op " << o.name << " " << o.tries << " " << o.wins << " " << o.gain << "\n";
}

bool Evolver::loadCheckpoint() {
    State s;
    const fs::path ck = fs::path(home_) / "checkpoints" / "best.orn";
    if (!loadState(s, ck.string())) return false;
    model_.restore(s);
    std::ifstream in(fs::path(home_) / "checkpoints" / "evolution.state");
    std::string key;
    while (in >> key) {
        if (key == "gen") in >> gen_;
        else if (key == "best") { double dummy; in >> dummy; }
        else if (key == "op") {
            std::string name; int tries, wins; double gain;
            in >> name >> tries >> wins >> gain;
            for (auto& o : ops_) if (o.name == name) { o.tries = tries; o.wins = wins; o.gain = gain; }
        }
    }
    rebaseline();  // el corpus puede haber cambiado desde la última vez
    return true;
}

}  // namespace orion
