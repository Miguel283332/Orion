// orion_agent: agente autónomo en C++ con un LLM local (Ollama). Ver README.md.
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define ISATTY(fd) isatty(fd)
#endif

#include "orion/agent.hpp"

#ifndef ORION_SOURCE_DIR
#define ORION_SOURCE_DIR ""
#endif
#ifndef ORION_VERSION
#define ORION_VERSION "3.0.0"
#endif

using namespace orion;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

struct Opts {
    std::string cmd = "chat", home = env_or("ORION_HOME", "orion_home"), model, embed_model = "nomic-embed-text", ollama_url, autonomy = "supervised";
    int num_ctx = 6144, max_steps = 12, max_cycles = 0, maintenance_every = 10;
    double interval = 5.0;
    bool allow_private = false, free_mode = false;
    std::vector<std::string> upload_hosts, rest;
};

static void usage() {
    std::cout <<
R"(orion_agent - agente autónomo con LLM local (Ollama)

Uso: orion_agent [opciones] <comando> [argumentos]

Comandos:
  chat               conversa con el agente (él actúa con sus herramientas)
  goal "texto"       deja un objetivo en la bandeja de entrada
  run                bucle continuo: ejecuta objetivos y consolida memoria
  eval               mide al agente con tareas verificables
  sleep              consolida en pesos (LoRA): delega en `python orion_agent.py sleep` (necesita GPU)
  approvals | approve ID | deny ID   gestionar permisos pendientes (modo sin terminal)
  memory [consulta]  ver/buscar en la memoria
  stop | resume      crea/borra el archivo STOP
  setup              comprueba Ollama y descarga los modelos

Opciones:
  --home DIR --model M --embed-model M --ollama-url URL --num-ctx N --max-steps N
  --autonomy strict|supervised|full   (supervised por defecto; full = sin permisos ni confinamiento)
  --allow-private-network   --upload-host HOST (repetible)
  run: --free  --interval S  --max-cycles N  --maintenance-every N
)";
}

static Opts parse(int argc, char** argv) {
    Opts o;
    bool have = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::cerr << "falta valor para " << a << "\n"; std::exit(2); } return argv[++i]; };
        if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else if (a == "--home") o.home = next();
        else if (a == "--model") o.model = next();
        else if (a == "--embed-model") o.embed_model = next();
        else if (a == "--ollama-url") o.ollama_url = next();
        else if (a == "--num-ctx") o.num_ctx = std::stoi(next());
        else if (a == "--max-steps") o.max_steps = std::stoi(next());
        else if (a == "--autonomy") o.autonomy = next();
        else if (a == "--allow-private-network") o.allow_private = true;
        else if (a == "--upload-host") o.upload_hosts.push_back(lower(next()));
        else if (a == "--free") o.free_mode = true;
        else if (a == "--interval") o.interval = std::stod(next());
        else if (a == "--max-cycles") o.max_cycles = std::stoi(next());
        else if (a == "--maintenance-every") o.maintenance_every = std::stoi(next());
        else if (!a.empty() && a[0] != '-') { if (!have) { o.cmd = a; have = true; } else o.rest.push_back(a); }
        else { std::cerr << "opción desconocida: " << a << "\n"; usage(); std::exit(2); }
    }
    return o;
}

struct Runtime {
    Opts o;
    fs::path home, ws;
    std::unique_ptr<OllamaClient> llm;
    Policy pol;
    std::unique_ptr<Audit> audit;
    std::unique_ptr<Embedder> emb;
    std::unique_ptr<Memory> mem;
    std::unique_ptr<Agent> agent;
};

static std::string state_model(const fs::path& home, const std::string& def) {
    std::error_code ec;
    if (!fs::exists(home / "state.json", ec)) return def;
    json j = json::parse(read_file(home / "state.json"), nullptr, false);
    return j.is_object() && j.contains("model") && j["model"].is_string() ? j["model"].get<std::string>() : def;
}

static bool boot(Runtime& rt, bool interactive) {
    Opts& o = rt.o;
    auto aut = parse_autonomy(o.autonomy);
    if (!aut) { std::cerr << "--autonomy debe ser strict, supervised o full\n"; return false; }
    rt.home = fs::absolute(o.home);
    fs::create_directories(rt.home / "workspace");
    rt.ws = rt.home / "workspace";
    std::string model = !o.model.empty() ? o.model : state_model(rt.home, "llama3.2:3b");
    rt.llm = std::make_unique<OllamaClient>(o.ollama_url, model, o.embed_model, o.num_ctx);
    bool has_embed = false;
    try {
        auto names = rt.llm->list_models();
        auto base = [](const std::string& n) { return n.substr(0, n.find(':')); };
        has_embed = std::any_of(names.begin(), names.end(), [&](const std::string& n) { return base(n) == base(o.embed_model); });
        if (!std::any_of(names.begin(), names.end(), [&](const std::string& n) { return n == model || base(n) == base(model); }))
            say("[aviso] el modelo '" + model + "' no aparece en Ollama; ejecuta:  ollama pull " + model);
    } catch (const std::exception& e) { say(std::string("[aviso] ") + e.what()); }
    if (!has_embed) say("[aviso] sin modelo de embeddings ('ollama pull " + o.embed_model + "'): la memoria usa búsqueda por hashing (peor).");
    rt.pol.autonomy = *aut;
    rt.pol.workspace = rt.ws; rt.pol.home = rt.home; rt.pol.allow_private = o.allow_private; rt.pol.upload_hosts = o.upload_hosts;
    rt.pol.interactive = interactive;
    fs::path src = ORION_SOURCE_DIR;
    if (!src.empty() && fs::exists(src / "CMakeLists.txt")) rt.pol.extra_protected.push_back(src);
    if (*aut == Autonomy::Full) say("[!] AUTONOMÍA TOTAL: sin permisos y sin confinamiento. Úsala solo en una VM/contenedor.");
    rt.audit = std::make_unique<Audit>(rt.home / "audit.jsonl");
    rt.emb = std::make_unique<Embedder>(has_embed ? static_cast<LLM*>(rt.llm.get()) : nullptr);
    rt.mem = std::make_unique<Memory>(rt.home, rt.emb.get());
    rt.agent = std::make_unique<Agent>(rt.home, *rt.llm, rt.pol, *rt.audit, *rt.mem, AgentConfig{o.max_steps, 9000}, src);
    return true;
}

static int cmd_chat(Runtime& rt) {
    say(std::string("Orion ") + ORION_VERSION + " | modelo " + rt.llm->model_name() + " | autonomía " + rt.o.autonomy + " | home " + rt.home.string() + "\nEscribe un mensaje. Comandos: /memoria /auditoria /salir");
    std::string line;
    while (!g_stop) {
        std::cout << "\ntú> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        line = trim(line);
        if (line.empty()) continue;
        if (line == "/salir" || line == "/exit") break;
        if (line == "/memoria") { say(rt.mem->stats() + "\n--- núcleo ---\n" + rt.mem->core_get()); continue; }
        if (line == "/auditoria") {
            std::error_code ec;
            if (!fs::exists(rt.home / "audit.jsonl", ec)) { say("(vacía)"); continue; }
            auto lines = split(read_file(rt.home / "audit.jsonl"), '\n');
            for (size_t i = lines.size() > 16 ? lines.size() - 16 : 0; i < lines.size(); ++i) if (!lines[i].empty()) say(lines[i]);
            continue;
        }
        try { say("\norion> " + rt.agent->chat_turn(line)); } catch (const LLMError& e) { say(std::string("error: ") + e.what()); }
        if (rt.agent->gate.restart_requested) { say("[código fuente actualizado] recompila (cmake --build) y reinicia."); break; }
    }
    return 0;
}

static int cmd_goal(Runtime& rt) {
    fs::path inbox = fs::absolute(rt.o.home) / "inbox";
    fs::create_directories(inbox);
    std::string text;
    for (auto& w : rt.o.rest) text += (text.empty() ? "" : " ") + w;
    if (text.empty()) { std::cerr << "uso: orion_agent goal \"texto\"\n"; return 2; }
    fs::path f = inbox / (now_str().substr(0, 10) + "_" + rand_hex(8) + ".txt");
    write_file(f, text);
    say("objetivo en la bandeja: " + f.filename().string());
    return 0;
}

static int cmd_run(Runtime& rt) {
    fs::path inbox = rt.home / "inbox", done = rt.home / "inbox_done";
    fs::create_directories(inbox);
    fs::create_directories(done);
    if (rt.agent->stop_requested()) { say("Existe el archivo STOP. Bórralo con:  orion_agent resume"); return 0; }
    say(std::string("Orion ") + ORION_VERSION + " en marcha continua (Ctrl-C o `stop` para parar). modelo=" + rt.llm->model_name() + " autonomía=" + rt.o.autonomy);
    int cycles = 0, goals = 0;
    while (!g_stop && !rt.agent->stop_requested() && (rt.o.max_cycles == 0 || cycles < rt.o.max_cycles)) {
        ++cycles;
        std::vector<fs::path> pending;
        for (auto& e : fs::directory_iterator(inbox)) if (e.path().extension() == ".txt") pending.push_back(e.path());
        std::sort(pending.begin(), pending.end());
        std::string goal;
        try {
            if (!pending.empty()) {
                goal = trim(read_file(pending[0]));
                fs::rename(pending[0], done / pending[0].filename());
            } else if (rt.o.free_mode) {
                auto g = rt.agent->free_goal();
                if (g) goal = *g;
            }
            if (!goal.empty()) {
                GoalResult r = rt.agent->run_goal(goal);
                ++goals;
                char sc[16];
                std::snprintf(sc, sizeof(sc), "%.2f", r.score);
                say("--> " + clip(r.result, 300) + "  (éxito=" + (r.success ? "true" : "false") + ", puntuación=" + sc + ")");
                if (rt.agent->gate.restart_requested) { say("[código fuente actualizado] recompila (cmake --build) y reinicia."); break; }
                if (rt.o.maintenance_every && goals % rt.o.maintenance_every == 0) say("[mantenimiento] " + rt.agent->maintenance());
            } else {
                sleep_ms((int)(rt.o.interval * 1000));
            }
        } catch (const LLMError& e) {
            say(std::string("[aviso] ") + e.what());
            sleep_ms((int)(rt.o.interval * 1000));
        }
    }
    return 0;
}

static int cmd_eval(Runtime& rt) {
    json r = run_eval(*rt.llm, eval_tasks());
    for (auto& x : r["results"]) say(std::string("  ") + (x["passed"].get<bool>() ? "OK   " : "FALLA ") + x["name"].get<std::string>());
    char sc[16];
    std::snprintf(sc, sizeof(sc), "%.2f", r["score"].get<double>());
    say("puntuación: " + std::to_string(r["passed"].get<int>()) + "/" + std::to_string(r["total"].get<int>()) + " = " + sc + "  (modelo " + rt.llm->model_name() + ")");
    return 0;
}

static int cmd_approvals(const Opts& o) {
    fs::path d = fs::absolute(o.home) / "approvals";
    bool any = false;
    std::error_code ec;
    if (fs::exists(d, ec))
        for (auto& e : fs::directory_iterator(d))
            if (e.path().extension() == ".json") {
                json j = json::parse(read_file(e.path()), nullptr, false);
                if (j.is_object()) { say("[" + j.value("id", "") + "] " + j.value("ts", "") + "  " + j.value("summary", "")); any = true; }
            }
    if (!any) say("(no hay peticiones pendientes)");
    return 0;
}

static int cmd_answer(const Opts& o, bool yes) {
    if (o.rest.empty()) { std::cerr << "falta el id\n"; return 2; }
    fs::path d = fs::absolute(o.home) / "approvals";
    if (!fs::exists(d / (o.rest[0] + ".json"))) { say("no existe esa petición (¿ya caducó o fue respondida?)"); return 1; }
    write_file(d / (o.rest[0] + ".answer"), yes ? "yes" : "no");
    say(yes ? "aprobada" : "denegada");
    return 0;
}

static int cmd_memory(Runtime& rt) {
    std::string q;
    for (auto& w : rt.o.rest) q += (q.empty() ? "" : " ") + w;
    if (!q.empty()) {
        char b[16];
        for (auto& [s, it] : rt.mem->search(q, 10)) { std::snprintf(b, sizeof(b), "%.2f", s); say("[" + it.id + "|" + it.kind + "|" + b + "] " + it.text); }
    } else {
        say(rt.mem->stats());
        size_t i = 0, total = rt.mem->items().size();
        for (auto& [id, it] : rt.mem->items()) if (++i + 20 > total) say("[" + id + "|" + it.kind + "] " + clip(it.text, 160));
    }
    return 0;
}

static int cmd_setup(const Opts& o) {
    OllamaClient c(o.ollama_url, o.model.empty() ? "llama3.2:3b" : o.model, o.embed_model);
    say("Comprobando Ollama en " + c.base() + " ...");
    try {
        auto names = c.list_models();
        say("Ollama responde. Modelos instalados: " + std::to_string(names.size()));
    } catch (const std::exception& e) {
        say(std::string(e.what()) + "\nInstala Ollama desde https://ollama.com/download y arráncalo (ollama serve).");
        return 1;
    }
    for (const std::string& m : {c.model_name(), c.embed_model_name()}) {
        say("Descargando " + m + " (la primera vez tarda)...");
        if (run_cmd("ollama pull " + shell_quote(m)).code != 0) { say("No se pudo descargar " + m); return 1; }
    }
    say("Listo. Prueba:  orion_agent chat");
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    Opts o = parse(argc, argv);
    try {
        if (o.cmd == "setup") return cmd_setup(o);
        if (o.cmd == "approvals") return cmd_approvals(o);
        if (o.cmd == "approve" || o.cmd == "deny") return cmd_answer(o, o.cmd == "approve");
        if (o.cmd == "stop" || o.cmd == "resume") {
            fs::path f = fs::absolute(o.home) / "STOP";
            if (o.cmd == "stop") { write_file(f, now_str()); say("STOP creado: el agente se detendrá tras el paso actual."); }
            else { std::error_code ec; fs::remove(f, ec); say("STOP borrado."); }
            return 0;
        }
        if (o.cmd == "sleep") {
            std::string args;
            for (const char* a : {"--home", "--model", "--embed-model", "--ollama-url"}) (void)a;
            say("La consolidación en pesos (LoRA) usa PyTorch: ejecuta\n  python orion_agent.py --home " + shell_quote(o.home) + " sleep");
            return 0;
        }
        Runtime rt;
        rt.o = o;
        bool interactive = ISATTY(0) != 0;
        if (!boot(rt, interactive)) return 2;
        if (o.cmd == "chat") return cmd_chat(rt);
        if (o.cmd == "goal") return cmd_goal(rt);
        if (o.cmd == "run") return cmd_run(rt);
        if (o.cmd == "eval") return cmd_eval(rt);
        if (o.cmd == "memory") return cmd_memory(rt);
        usage();
        return 2;
    } catch (const LLMError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
