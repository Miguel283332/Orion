// Orion: IA de texto que se evalúa, se muta y se mejora a sí misma. Ver README.md.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include "orion/backend.hpp"
#include "orion/chat.hpp"
#include "orion/dataset.hpp"
#include "orion/evolve.hpp"
#include "orion/model.hpp"

#ifndef ORION_DATA_DIR
#define ORION_DATA_DIR "data"
#endif
#ifndef ORION_SOURCE_DIR
#define ORION_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;
using namespace orion;

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

struct Options {
    std::string home = "orion_home";
    std::string device = "auto";
    std::string command = "chat";
    std::vector<std::string> args;
    uint64_t seed = 1234;
    Genome genome;
    HyperParams hp;
    EvolveConfig evo;
    long long steps = 500;
    long long generations = 0;  // 0 = hasta Ctrl-C
    bool autoEvolve = false;
    bool withSource = false;
};

static void usage() {
    std::cout <<
R"(Uso: orion [opciones] [comando] [argumentos]

Comandos:
  chat               Conversación interactiva (por defecto). Con --auto se mejora en segundo plano.
  train              Entrena --steps N pasos y guarda el checkpoint.
  evolve             Bucle de auto-mejora: --generations N (0 = hasta Ctrl-C).
  gen "texto"        Continúa un texto una vez.
  bench              Lista los dispositivos y mide su rendimiento.
  info               Resumen del estado (modelo, corpus, dispositivo).

Opciones:
  --home DIR         Directorio de datos/memoria/checkpoints (por defecto ./orion_home)
  --device SPEC      auto | cpu | gpu | cl:PLATAFORMA:DISPOSITIVO   (por defecto auto)
  --auto             En chat: evoluciona en segundo plano mientras conversas
  --with-source      Añade al corpus el código fuente de Orion (para que "conozca" su propio código)
  --steps-per-gen N  Pasos de entrenamiento por generación (150)
  --hidden N --ctx N --dim N   Arquitectura inicial (solo si no hay checkpoint)
  --lr X --batch N   Hiperparámetros iniciales
  --wd X --ctx-drop X  Regularización inicial
  --max-hidden N --max-ctx N   Límite de crecimiento automático
  --seed N           Semilla
)";
}

static Options parse(int argc, char** argv) {
    Options o;
    bool haveCmd = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "falta valor para " << a << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else if (a == "--home") o.home = next();
        else if (a == "--device") o.device = next();
        else if (a == "--seed") o.seed = std::stoull(next());
        else if (a == "--steps") o.steps = std::stoll(next());
        else if (a == "--generations") o.generations = std::stoll(next());
        else if (a == "--steps-per-gen") o.evo.stepsPerGen = std::stoi(next());
        else if (a == "--hidden") o.genome.hidden = std::stoi(next());
        else if (a == "--ctx") o.genome.ctx = std::stoi(next());
        else if (a == "--dim") o.genome.dim = std::stoi(next());
        else if (a == "--lr") o.hp.lr = std::stof(next());
        else if (a == "--batch") o.hp.batch = std::stoi(next());
        else if (a == "--wd") o.hp.wd = std::stof(next());
        else if (a == "--ctx-drop") o.hp.ctxDrop = std::stof(next());
        else if (a == "--max-hidden") o.evo.maxHidden = std::stoi(next());
        else if (a == "--max-ctx") o.evo.maxCtx = std::stoi(next());
        else if (a == "--auto") o.autoEvolve = true;
        else if (a == "--with-source") o.withSource = true;
        else if (!haveCmd && a[0] != '-') { o.command = a; haveCmd = true; }
        else if (a[0] != '-') o.args.push_back(a);
        else { std::cerr << "opción desconocida: " << a << "\n"; usage(); std::exit(2); }
    }
    return o;
}

struct Runtime {
    Options opt;
    std::unique_ptr<Backend> backend;
    std::string deviceReport;
    Dataset data;
    std::unique_ptr<Model> model;
    std::unique_ptr<Learner> learner;
    std::unique_ptr<Evolver> evolver;
    bool resumed = false;
};

static void seedHome(const std::string& home) {
    fs::create_directories(fs::path(home) / "data");
    fs::create_directories(fs::path(home) / "memory");
    fs::create_directories(fs::path(home) / "checkpoints");
    std::error_code ec;
    if (fs::exists(ORION_DATA_DIR, ec)) {
        for (auto& e : fs::directory_iterator(ORION_DATA_DIR, ec)) {
            fs::path dst = fs::path(home) / "data" / e.path().filename();
            if (e.is_regular_file() && !fs::exists(dst)) fs::copy_file(e.path(), dst, ec);
        }
    }
}

static bool boot(Runtime& rt, bool quiet = false) {
    Options& o = rt.opt;
    seedHome(o.home);
    rt.backend = selectBackend(o.device, &rt.deviceReport);
    if (!quiet) std::cout << "[dispositivo]\n" << rt.deviceReport;

    rt.data.addPath(o.home + "/data");
    rt.data.addPath(o.home + "/memory/aprendido.txt");  // solo lo enseñado/aprobado por una persona
    if (o.withSource) rt.data.addPath(ORION_SOURCE_DIR, true);
    if (rt.data.size() < 4096 || rt.data.count(Split::Val) < 64) {
        std::cerr << "El corpus es demasiado pequeño (" << rt.data.size() << " bytes). Añade textos a " << o.home << "/data\n";
        return false;
    }

    State init = freshState(o.genome, o.hp, o.seed);
    rt.model = std::make_unique<Model>(*rt.backend, init);
    rt.learner = std::make_unique<Learner>(*rt.model, rt.data, o.seed + 1);
    rt.evolver = std::make_unique<Evolver>(*rt.model, rt.data, *rt.learner, o.evo, o.home);
    rt.resumed = rt.evolver->loadCheckpoint();
    // la arquitectura cargada puede ser mayor que los límites por defecto
    rt.evolver->config().maxHidden = std::max(rt.evolver->config().maxHidden, rt.model->genome().hidden);
    rt.evolver->config().maxCtx = std::max(rt.evolver->config().maxCtx, rt.model->genome().ctx);
    if (!quiet) {
        const Genome& g = rt.model->genome();
        std::cout << "[modelo] " << (rt.resumed ? "checkpoint cargado" : "modelo nuevo") << ": hidden=" << g.hidden
                  << " ctx=" << g.ctx << " dim=" << g.dim << " parámetros=" << rt.model->paramCount()
                  << " | corpus=" << rt.data.size() << " bytes (train " << rt.data.count(Split::Train) << ", val "
                  << rt.data.count(Split::Val) << ", test " << rt.data.count(Split::Test) << ")\n";
    }
    return true;
}

static void printStats(Runtime& rt) {
    const Genome& g = rt.model->genome();
    const float val = rt.learner->evalVal(rt.evolver->config().evalMax);
    const float test = rt.learner->evalSplit(Split::Test, rt.evolver->config().evalMax);
    std::cout << "generación " << rt.evolver->generation() << " | val=" << val << " (ppl " << std::exp(val)
              << ") test=" << test << " (ppl " << std::exp(test) << ") | hidden=" << g.hidden << " ctx=" << g.ctx
              << " params=" << rt.model->paramCount() << " lr=" << rt.model->hp().lr << " wd=" << rt.model->hp().wd << " drop=" << rt.model->hp().ctxDrop << " batch=" << rt.model->hp().batch << "\n"
              << rt.evolver->opTable();
}

static void printGen(const GenResult& r) {
    std::cout << "gen " << r.gen << " " << r.op << (r.detail.empty() ? "" : " [" + r.detail + "]") << ": val " << r.before
              << " -> " << r.after << (r.accepted ? "  ACEPTADA" : "  rechazada (rollback)") << "  (" << r.secs << "s)\n";
}

// ---------------------------------------------------------------- comandos

static int cmdTrain(Runtime& rt) {
    // Entrena y se queda con el MEJOR punto de validación (parada temprana), no con el último.
    const long long total = rt.opt.steps, chunk = 50;
    auto t0 = std::chrono::steady_clock::now();
    float bestVal = rt.learner->evalVal(rt.evolver->config().evalMax);
    State best = rt.model->snapshot();
    std::cout << "val inicial=" << bestVal << "\n";
    for (long long done = 0; done < total && !g_stop; done += chunk) {
        float tl = rt.learner->train((int)std::min(chunk, total - done), {}, &g_stop);
        float vl = rt.learner->evalVal(rt.evolver->config().evalMax);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const bool better = vl < bestVal;
        if (better) { bestVal = vl; best = rt.model->snapshot(); }
        std::cout << "paso " << done + chunk << "/" << total << " train=" << tl << " val=" << vl << (better ? " *" : "") << " (" << s << "s)\n";
    }
    rt.model->restore(best);
    rt.evolver->rebaseline();
    rt.evolver->saveCheckpoint();
    std::cout << "mejor val=" << bestVal << " (ppl " << std::exp(bestVal) << "); checkpoint guardado en " << rt.opt.home << "/checkpoints/best.orn\n";
    return 0;
}

static int cmdEvolve(Runtime& rt) {
    std::cout << "línea base: val=" << rt.evolver->bestVal() << "\n";
    for (long long i = 0; (rt.opt.generations == 0 || i < rt.opt.generations) && !g_stop; ++i) {
        GenResult r = rt.evolver->step({}, &g_stop);
        if (!r.aborted) printGen(r);
    }
    rt.evolver->saveCheckpoint();
    printStats(rt);
    return 0;
}

// Registro bruto de todo lo hablado. NO se usa para entrenar: aprender de la propia salida
// (a veces ruido) degradaría el modelo. Lo que sí se aprende es lo que una persona aprueba.
static void logConversation(const std::string& home, const std::string& user, const std::string& answer) {
    std::ofstream out(home + "/memory/conversations.log", std::ios::app);
    out << "Usuario: " << user << "\nOrion: " << answer << "\n\n";
}

static void learnPair(const std::string& home, const std::string& user, const std::string& answer) {
    std::ofstream out(home + "/memory/aprendido.txt", std::ios::app);
    out << "Usuario: " << user << "\nOrion: " << answer << "\n\n";
}

static int cmdChat(Runtime& rt) {
    // La evolución trabaja en un hilo. Cuando el usuario escribe, la generación en curso se
    // INTERRUMPE (rollback al último estado aceptado) y el chat usa siempre pesos aceptados.
    std::mutex modelMutex;
    std::atomic<int> waiting{0};
    std::atomic<bool> preempt{false}, bgStop{false}, bgOn{rt.opt.autoEvolve};
    std::thread bg([&] {
        while (!bgStop && !g_stop) {
            while (waiting.load() > 0 && !bgStop) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (!bgOn) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            std::unique_lock<std::mutex> lk(modelMutex);
            if (preempt) continue;
            GenResult r = rt.evolver->step({}, &preempt);
            lk.unlock();
            if (!r.aborted) {
                std::cout << "\n  [evolución] ";
                printGen(r);
                std::cout << "> " << std::flush;
            }
        }
    });
    auto acquire = [&]() {
        waiting++;
        preempt = true;
        std::unique_lock<std::mutex> lk(modelMutex);
        waiting--;
        return lk;
    };

    Chat chat(*rt.model, rt.opt.seed + 7);
    std::string lastUser, lastAnswer;
    std::cout << "\nOrion listo. Escribe un mensaje o /help.\n";
    std::string line;
    while (!g_stop) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line[0] == '/') {
            std::istringstream is(line);
            std::string cmd, arg;
            is >> cmd;
            std::getline(is, arg);
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            auto lk = acquire();
            if (cmd == "/exit" || cmd == "/quit") { preempt = false; break; }
            else if (cmd == "/help")
                std::cout << "/good              aprueba la última respuesta (entra al corpus de aprendizaje)\n"
                             "/teach P | R       enseña un par pregunta | respuesta\n"
                             "/auto on|off       mejora en segundo plano\n"
                             "/stats  /save  /grow  /ingest RUTA  /temp X  /reset  /device  /exit\n";
            else if (cmd == "/auto") { bgOn = (arg != "off"); std::cout << "auto-mejora " << (bgOn ? "activada" : "desactivada") << "\n"; }
            else if (cmd == "/stats") printStats(rt);
            else if (cmd == "/save") { rt.evolver->saveCheckpoint(); std::cout << "guardado\n"; }
            else if (cmd == "/temp") { try { chat.config().temperature = std::stof(arg); } catch (...) { std::cout << "uso: /temp 0.5\n"; } }
            else if (cmd == "/reset") { chat.reset(); }
            else if (cmd == "/device") std::cout << rt.deviceReport;
            else if (cmd == "/good") {
                if (lastUser.empty()) std::cout << "nada que aprobar todavía\n";
                else { learnPair(rt.opt.home, lastUser, lastAnswer); std::cout << "aprendido (se usará en la próxima generación)\n"; }
            } else if (cmd == "/teach") {
                size_t bar = arg.find('|');
                if (bar == std::string::npos) std::cout << "uso: /teach pregunta | respuesta\n";
                else {
                    auto trim = [](std::string x) { while (!x.empty() && x.front() == ' ') x.erase(x.begin()); while (!x.empty() && x.back() == ' ') x.pop_back(); return x; };
                    learnPair(rt.opt.home, trim(arg.substr(0, bar)), trim(arg.substr(bar + 1)));
                    std::cout << "aprendido\n";
                }
            } else if (cmd == "/ingest") {
                size_t n = rt.data.addPath(arg, true);
                rt.evolver->rebaseline();
                std::cout << "ingeridos " << n << " bytes de " << arg << " (corpus " << rt.data.size() << ")\n";
            } else if (cmd == "/grow") {
                State st = rt.model->snapshot();
                rt.model->restore(growState(st, st.genome.hidden * 5 / 4 / 16 * 16, st.genome.ctx, rt.opt.seed + st.adamStep));
                rt.evolver->saveCheckpoint();
                std::cout << "red ampliada a hidden=" << rt.model->genome().hidden << " (misma función, más capacidad)\n";
            } else std::cout << "comando desconocido (/help)\n";
            preempt = false;
            continue;
        }
        std::string answer;
        {
            auto lk = acquire();
            answer = chat.reply(line);
            preempt = false;
        }
        std::cout << "Orion: " << answer << "\n";
        logConversation(rt.opt.home, line, answer);
        lastUser = line;
        lastAnswer = answer;
    }
    bgStop = true;
    preempt = true;
    bg.join();
    rt.evolver->saveCheckpoint();
    return 0;
}

static int cmdBench(Runtime& rt) {
    std::cout << "OpenCL compilado: " << (openclCompiledIn() ? "sí" : "no") << "\n";
    for (auto& d : listOpenCLDevices())
        std::cout << "  cl:" << d.platform << ":" << d.device << "  " << d.name << " [" << d.kind << ", " << d.computeUnits
                  << " CU, " << d.memBytes / (1024 * 1024) << " MB]\n";
    std::string rep;
    auto be = selectBackend("auto", &rep);
    std::cout << rep;
    (void)rt;
    return 0;
}

int main(int argc, char** argv) {
    Runtime rt;
    rt.opt = parse(argc, argv);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        if (rt.opt.command == "bench") {
            rt.opt.device = "auto";
            return cmdBench(rt);
        }
        if (!boot(rt, rt.opt.command == "gen")) return 1;
        if (rt.opt.command == "chat") return cmdChat(rt);
        if (rt.opt.command == "train") return cmdTrain(rt);
        if (rt.opt.command == "evolve") return cmdEvolve(rt);
        if (rt.opt.command == "info") { printStats(rt); return 0; }
        if (rt.opt.command == "gen") {
            Chat chat(*rt.model, rt.opt.seed);
            std::string prompt = rt.opt.args.empty() ? "Usuario: hola\nOrion:" : rt.opt.args[0];
            std::cout << prompt << chat.continueText(prompt, false) << "\n";
            return 0;
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
