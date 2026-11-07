// Pruebas del agente C++ (JUEZ: protegidas, no se pueden modificar con self_patch).
#include <atomic>
#include <cstdio>
#include <thread>

#include "orion/agent.hpp"

using namespace orion;

static int g_run = 0, g_fail = 0;
#define CHECK(cond, msg)                                                                   \
    do {                                                                                   \
        ++g_run;                                                                           \
        if (!(cond)) { ++g_fail; std::printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, std::string(msg).c_str()); } \
    } while (0)

struct TempDir {
    fs::path p = make_temp_dir("orion_test_");
    ~TempDir() { std::error_code ec; fs::remove_all(p, ec); }
};

// LLM falso con respuestas guiadas
struct ScriptedLLM : LLM {
    std::vector<json> replies;
    std::vector<json> calls;
    explicit ScriptedLLM(std::vector<json> r = {}) : replies(std::move(r)) {}
    std::string chat(const json& messages, bool, int, double) override {
        calls.push_back(messages);
        std::string sys = messages[0]["content"];
        if (contains(sys, "reflection module"))
            return json{{"success", true}, {"score", 0.9}, {"skill", nullptr}, {"lessons", {"Comprobar el archivo después de escribirlo evita errores."}}}.dump();
        if (contains(sys, "curiosity module")) return json{{"goal", "Ordenar y resumir mi memoria de trabajo."}}.dump();
        if (contains(sys, "final-answer module")) return "Respuesta de emergencia de prueba.";
        if (contains(sys, "rule-distillation module")) return json{{"rules", {"Cuando una búsqueda falle dos veces, responder con lo que se sabe."}}}.dump();
        if (starts_with(sys, "Resume en")) return "Resumen de prueba.";
        if (replies.empty()) return json{{"thought", "fin"}, {"tool", "done"}, {"args", {{"summary", "fin"}}}}.dump();
        json r = replies.front();
        replies.erase(replies.begin());
        return r.is_string() ? r.get<std::string>() : r.dump();
    }
    std::vector<float> embed(const std::string&) override { throw LLMError("sin embeddings"); }
    std::string model_name() const override { return "mock"; }
    std::string embed_model_name() const override { return "mock-embed"; }
};

static json act(const std::string& tool, json args = json::object()) { return {{"thought", "t"}, {"tool", tool}, {"args", args}}; }

struct Rig {
    fs::path home;
    Policy pol;
    Audit audit;
    Embedder emb{nullptr};
    Memory mem;
    Agent ag;
    Rig(const fs::path& h, LLM& llm, Autonomy aut = Autonomy::Supervised, Policy::Approver ap = nullptr, bool allow_private = false, int max_steps = 8)
        : home(h), pol(), audit(h / "audit.jsonl"), mem(h, &emb), ag(h, llm, setup(aut, ap, allow_private), audit, mem, AgentConfig{max_steps, 9000}) {}
    Policy& setup(Autonomy aut, Policy::Approver ap, bool allow_private) {
        pol.autonomy = aut; pol.workspace = home / "workspace"; pol.home = home;
        pol.approver = ap ? ap : [](const std::string&, const std::string&) { return false; };
        pol.allow_private = allow_private; pol.interactive = false;
        return pol;
    }
};

static void t_hash(void) {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 vacío");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    CHECK(sha256_hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 de un millón de 'a'");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 de 2 bloques");
    TempDir td;
    write_file(td.p / "f", "abc");
    CHECK(sha256_file(td.p / "f") == sha256_hex("abc"), "sha256_file");
}

static void t_net() {
    for (const char* u : {"file:///etc/passwd", "ftp://example.com/x", "http://127.0.0.1:8080/", "http://localhost/", "http://10.0.0.5/",
                          "http://169.254.169.254/latest/meta-data", "http://[::1]/", "http://192.168.1.1/", "http://[::ffff:127.0.0.1]/",
                          "http://100.64.0.1/", "http://172.16.0.1/", "http://0.0.0.0/", "http://224.0.0.1/", "http://[fe80::1]/", "http://[fc00::1]/"})
        CHECK(check_url(u, false).has_value(), std::string("SSRF: debe bloquear ") + u);
    CHECK(!check_url("http://93.184.216.34/", false).has_value(), "una IP pública literal es aceptable");
    CHECK(!check_url("http://127.0.0.1:1/", true).has_value(), "allow_private permite localhost");
    CHECK(!ip_is_public("::ffff:10.0.0.1") && ip_is_public("8.8.8.8") && ip_is_public("2606:4700:4700::1111") && !ip_is_public("2001:db8::1") && !ip_is_public("basura"), "ip_is_public");
    std::string ip;
    ParsedUrl pu;
    CHECK(!check_url("http://93.184.216.34:8080/x", false, &pu, &ip) && ip == "93.184.216.34" && pu.port == 8080 && pu.host == "93.184.216.34", "check_url devuelve la IP verificada para fijarla");
    ParsedUrl a = parse_url("https://user:pw@Example.com:8443/a/b?c=1");
    CHECK(a.ok && a.scheme == "https" && a.host == "Example.com" && a.port == 8443 && a.path == "/a/b?c=1", "parse_url");
    ParsedUrl b = parse_url("http://[::1]:9000/");
    CHECK(b.ok && b.host == "::1" && b.port == 9000, "parse_url IPv6");
    CHECK(!parse_url("http://host:abc/").ok && !parse_url("no-es-url").ok, "parse_url inválidas");
    try { HttpOptions o; http_fetch("http://127.0.0.1:1/", o); CHECK(false, "http_fetch debe bloquear localhost"); } catch (const std::exception& e) { CHECK(contains(std::string(e.what()), "privada"), "mensaje SSRF"); }
    CHECK(join_url("http://a.com/x/y.html", "z.html") == "http://a.com/x/z.html" && join_url("http://a.com:81/x", "/q") == "http://a.com:81/q" && join_url("https://a.com/x", "//b.com/c") == "https://b.com/c", "join_url");
    CHECK(url_encode("a b&c/ñ") == "a%20b%26c%2F%C3%B1", "url_encode");
    CHECK(normalize_ollama_url("0.0.0.0:11434") == "http://127.0.0.1:11434" && normalize_ollama_url("https://h:1/") == "https://h:1", "normalize_ollama_url");
}

static void t_paths() {
    TempDir td;
    fs::path root = td.p / "ws";
    fs::create_directories(root);
    CHECK(safe_path(root, "a/b.txt", false, false, {}) == fs::weakly_canonical(root / "a" / "b.txt"), "ruta interna");
    for (const char* bad : {"../x.txt", "/etc/passwd", "a/../../x"}) {
        try { safe_path(root, bad, false, false, {}); CHECK(false, std::string("debe rechazar ") + bad); } catch (const std::exception&) { CHECK(true, ""); }
    }
    fs::path prot = td.p / "protegido";
    fs::create_directories(prot);
    try { safe_path(root, (prot / "x").string(), true, true, {prot}); CHECK(false, "ruta protegida"); } catch (const std::exception&) { CHECK(true, ""); }
    CHECK(safe_path(root, (prot / "x").string(), true, false, {prot}).filename() == "x", "leer en ruta protegida sí se permite sin write");
    fs::path outside = td.p / "fuera";
    fs::create_directories(outside);
    std::error_code ec;
    fs::create_directory_symlink(outside, root / "enlace", ec);
    if (!ec) { try { safe_path(root, "enlace/x.txt", false, false, {}); CHECK(false, "symlink no debe permitir escapar"); } catch (const std::exception&) { CHECK(true, ""); } }
    CHECK(looks_secret_file(".env").has_value() && looks_secret_file("id_rsa").has_value() && looks_secret_file("dir/clave.pem").has_value(), "nombres secretos");
    write_file(root / "n.txt", "clave: sk-" + std::string(30, 'a'));
    write_file(root / "ok.txt", "hola");
    CHECK(looks_secret_file(root / "n.txt").has_value() && !looks_secret_file(root / "ok.txt").has_value(), "contenido secreto / normal");
#ifdef _WIN32
    _putenv_s("ORION_SECRET_PRUEBA", "valor-super-secreto-123");
#else
    setenv("ORION_SECRET_PRUEBA", "valor-super-secreto-123", 1);
#endif
    CHECK(redact("x valor-super-secreto-123 y") == "x [REDACTADO] y", "redact quita secretos del entorno");
    CHECK(resolve_secrets(json{{"A", "Bearer {{secret:PRUEBA}}"}})["A"] == "Bearer valor-super-secreto-123", "resolve_secrets");
    try { resolve_secrets(json("{{secret:NO_EXISTE}}")); CHECK(false, "secreto inexistente"); } catch (const std::exception&) { CHECK(true, ""); }
#ifdef _WIN32
    _putenv_s("ORION_SECRET_PRUEBA", "");
#else
    unsetenv("ORION_SECRET_PRUEBA");
#endif
}

static void t_policy() {
    std::vector<std::pair<std::string, std::string>> asked;
    auto appr = [&](bool ok) { return Policy::Approver([&asked, ok](const std::string& n, const std::string& s) { asked.emplace_back(n, s); return ok; }); };
    Policy p;
    p.approver = appr(false);
    CHECK(p.authorize("fs_read", {}).first && p.authorize("fs_write", {}).first && p.authorize("web_get", {}).first, "supervised: lo básico sin preguntar");
    CHECK(asked.empty(), "no se preguntó por lo básico");
    CHECK(!p.authorize("shell", {{"cmd", "ls"}}).first && !asked.empty(), "shell pregunta y se deniega");
    CHECK(!p.authorize("herramienta_nueva", json::object()).first, "una herramienta desconocida es sensible");
    p.approver = appr(true);
    CHECK(p.authorize("shell", {{"cmd", "ls"}}).first, "shell aprobada");
    p.tainted = true;
    asked.clear();
    p.authorize("shell", {{"cmd", "rm"}});
    CHECK(!asked.empty() && contains(asked[0].second, "AVISO"), "tras leer la web, el permiso avisa de posible inyección");
    Policy st;
    st.autonomy = Autonomy::Strict; st.approver = appr(false);
    CHECK(!st.authorize("fs_write", {}).first, "strict pregunta por escribir");
    Policy full;
    full.autonomy = Autonomy::Full;
    CHECK(full.authorize("shell", {}).first && full.unconfined(), "full no pregunta");
    Policy pu;
    pu.approver = appr(false); pu.upload_hosts = {"ok.example.com"};
    CHECK(pu.authorize("upload", {{"url", "https://ok.example.com/x"}}).first, "host de subida permitido");
    CHECK(!pu.authorize("upload", {{"url", "https://otro.example.com/x"}}).first, "otro host pide permiso");
    Policy none;
    CHECK(!none.authorize("shell", {}).first, "sin terminal ni home: denegado");
    CHECK(parse_autonomy("full") && !parse_autonomy("x") && risk_of("shell") == "sensitive" && risk_of("nueva") == "sensitive" && risk_of("fs_read") == "safe", "tabla de riesgos");
    TempDir td;   // aprobación por archivo (modo sin terminal)
    Policy pf;
    pf.home = td.p; pf.approval_timeout = 6;
    auto responder = [&](const char* ans) {
        for (int i = 0; i < 60; ++i) {
            std::error_code ec;
            if (fs::exists(td.p / "approvals", ec))
                for (auto& e : fs::directory_iterator(td.p / "approvals"))
                    if (e.path().extension() == ".json") { write_file(td.p / "approvals" / (e.path().stem().string() + ".answer"), ans); return; }
            sleep_ms(100);
        }
    };
    std::thread t1(responder, "yes");
    CHECK(pf.authorize("shell", {{"cmd", "x"}}).first, "aprobación por archivo: sí");
    t1.join();
    std::thread t2(responder, "no");
    CHECK(!pf.authorize("shell", {{"cmd", "x"}}).first, "aprobación por archivo: no");
    t2.join();
    pf.approval_timeout = 1.2;
    CHECK(!pf.authorize("shell", {{"cmd", "x"}}).first, "sin respuesta caduca y se deniega");
}

static void t_guarded() {
    TempDir td;
    fs::path marker = td.p / "marcador.txt";
    std::map<std::string, Tool> tools;
    tools["shell"] = {"shell", "cmd", "", [&](const json& a) { write_file(marker, a.value("cmd", std::string())); return std::string("ok"); }};
    tools["fs_read"] = {"fs_read", "path", "", [](const json&) { return "sk-" + std::string(30, 'z'); }};
    tools["web_get"] = {"web_get", "url", "", [](const json&) { return std::string("pagina"); }};
    tools["roto"] = {"roto", "", "", [](const json&) -> std::string { throw std::runtime_error("boom"); }};
    Policy pol;
    pol.approver = [](const std::string&, const std::string&) { return false; };
    Audit aud(td.p / "a.jsonl");
    auto r = guarded_call(pol, aud, tools, "shell", {{"cmd", "x"}});
    CHECK(!r.first && contains(r.second, "DENEGADO") && !fs::exists(marker), "denegado: no se ejecuta el efecto");
    pol.approver = [](const std::string&, const std::string&) { return true; };
    CHECK(guarded_call(pol, aud, tools, "shell", {{"cmd", "x"}}).first && fs::exists(marker), "aprobado: se ejecuta");
    r = guarded_call(pol, aud, tools, "fs_read", {{"path", "a"}});
    CHECK(contains(r.second, "[REDACTADO]") && !contains(r.second, "zzzz"), "la salida se redacta");
    guarded_call(pol, aud, tools, "web_get", {{"url", "u"}});
    CHECK(pol.tainted, "leer la web marca el ciclo como no confiable");
    pol.new_cycle();
    CHECK(!pol.tainted, "new_cycle limpia el aviso");
    r = guarded_call(pol, aud, tools, "roto", json::object());
    CHECK(!r.first && contains(r.second, "boom"), "los errores llegan como observación");
    CHECK(contains(guarded_call(pol, aud, tools, "no_existe", json::object()).second, "desconocida"), "herramienta desconocida");
    CHECK(!guarded_call(pol, aud, tools, "shell", json::array({"x"})).first, "args no objeto");
    std::string log = read_file(td.p / "a.jsonl");
    CHECK(std::count(log.begin(), log.end(), '\n') >= 6 && !contains(log, "zzzz"), "auditoría registra y no filtra secretos");
}

static void t_memory() {
    TempDir td;
    Embedder emb(nullptr);
    Memory m(td.p, &emb);
    std::string a = m.add("Marta prefiere el color verde y vive en Sevilla");
    m.add("El servidor de copias de seguridad usa el puerto 8443");
    m.add("Para compilar el proyecto en C++ se usa CMake en modo Release");
    CHECK(m.search("¿qué color le gusta a Marta?", 1)[0].second.id == a, "búsqueda semántica (hashing) encuentra lo relevante");
    CHECK(m.add("Marta prefiere el color verde y vive en Sevilla") == a, "deduplicación");
    CHECK(m.forget(a) && !m.forget(a), "olvidar");
    Memory m2(td.p, &emb);
    CHECK(!m2.items().count(a) && m2.size() == 2, "persistencia y tombstones al recargar");
    m2.core_set("Soy Orion. Mi usuario se llama Miguel.");
    CHECK(contains(m2.core_get(), "Miguel"), "memoria núcleo");
    m2.core_append("Línea extra");
    CHECK(contains(m2.core_get(), "Línea extra") && contains(m2.core_get(), "Miguel"), "core_append");
    try { m2.core_set(std::string(Memory::kCoreMax + 1, 'x')); CHECK(false, "el núcleo tiene límite"); } catch (const std::exception&) { CHECK(true, ""); }
    for (int i = 0; i < 12; ++i) m2.log_episode({{"i", i}});
    CHECK(m2.episode_count() == 12 && m2.rotate_episodes(5) == 7 && m2.episode_count() == 5, "rotación de episodios");
    m2.compact();
    Memory m3(td.p, &emb);
    CHECK(m3.size() == 2, "compactar conserva las entradas");
    m3.log_dialogue("user", "hola");
    CHECK(m3.recent_dialogue(3).back()["text"] == "hola", "diálogo");
    auto e = hash_embed("hola mundo");
    double n = 0;
    for (float x : e) n += (double)x * x;
    CHECK(std::abs(std::sqrt(n) - 1.0) < 1e-5, "embedding normalizado");
}

static void t_parse() {
    std::string err;
    auto a = parse_action("```json\n{\"thought\": \"x\", \"tool\": \"fs_read\", \"args\": {\"path\": \"a.txt\"}}\n```", err);
    CHECK(a && (*a)["tool"] == "fs_read" && (*a)["args"]["path"] == "a.txt", "JSON con vallas");
    a = parse_action("Claro, voy a hacerlo: {\"tool\": \"done\", \"args\": {\"summary\": \"a {b} c\"}} gracias", err);
    CHECK(a && (*a)["args"]["summary"] == "a {b} c", "JSON rodeado de texto y llaves en cadenas");
    a = parse_action("{\"action\": \"fs_list\", \"arguments\": \"{\\\"path\\\": \\\".\\\"}\"}", err);
    CHECK(a && (*a)["tool"] == "fs_list" && (*a)["args"]["path"] == ".", "sinónimos action/arguments");
    CHECK(!parse_action("no es json", err) && !parse_action("{\"args\": {}}", err) && !parse_action("{\"tool\": \"x\", \"args\": [1]}", err), "entradas inválidas");
    CHECK(clip("áéíóú", 3).size() > 0 && clip(std::string(100, 'x'), 40).size() < 100, "clip");
}

static void t_agent() {
    {
        TempDir td;
        ScriptedLLM llm({act("fs_write", {{"path", "hola.txt"}, {"content", "Hola"}}), act("fs_read", {{"path", "hola.txt"}}),
                         act("remember", {{"text", "El usuario se llama Marta"}, {"kind", "fact"}}), act("done", {{"summary", "listo"}})});
        set_quiet(true);
        Rig rig(td.p, llm);
        rig.mem.core_set("Soy Orion; el usuario es Miguel.");
        GoalResult r = rig.ag.run_goal("Escribe hola.txt");
        CHECK(r.finished && r.result == "listo" && r.steps == 4, "el bucle completa el objetivo");
        CHECK(trim(read_file(td.p / "workspace" / "hola.txt")) == "Hola", "la herramienta escribió el archivo");
        bool fact = false, lesson = false;
        for (auto& [id, it] : rig.mem.items()) { if (it.kind == "fact") fact = true; if (it.kind == "lesson") lesson = true; }
        CHECK(fact && lesson, "recuerdo y lección guardados");
        CHECK(rig.mem.episode_count() == 4, "episodios registrados");
        std::vector<fs::path> tr;
        for (auto& e : fs::directory_iterator(td.p / "trajectories")) tr.push_back(e.path());
        CHECK(tr.size() == 1 && json::parse(split(read_file(tr[0]), '\n')[0])["good"] == true, "trayectoria buena guardada para consolidar");
        std::string p = llm.calls[0][1]["content"];
        CHECK(contains(p, "Miguel") && contains(p, "GOAL") && contains(std::string(llm.calls[0][0]["content"]), "TOOLS"), "el prompt incluye objetivo, núcleo y herramientas");
        CHECK(fs::exists(td.p / "audit.jsonl"), "auditoría");
    }
    {
        TempDir td;
        ScriptedLLM llm({json("basura"), json("mas basura"), json("y mas")});
        Rig rig(td.p, llm);
        CHECK(rig.ag.run_goal("x").end_reason == "invalid", "tres respuestas inválidas abortan");
    }
    {
        TempDir td;
        ScriptedLLM llm(std::vector<json>(10, act("fs_list")));
        Rig rig(td.p, llm, Autonomy::Supervised, nullptr, false, 10);
        CHECK(rig.ag.run_goal("x").end_reason == "loop", "detecta bucles");
    }
    {
        TempDir td;
        ScriptedLLM llm({act("memory_stats"), act("fs_list"), act("fs_list", {{"path", "."}})});
        Rig rig(td.p, llm, Autonomy::Supervised, nullptr, false, 2);
        CHECK(rig.ag.run_goal("x").end_reason == "budget", "presupuesto de pasos");
        write_file(td.p / "STOP", "x");
        CHECK(contains(rig.ag.run_goal("y").result, "STOP"), "el archivo STOP detiene");
    }
    {
        TempDir td;
        ScriptedLLM llm({act("done", {{"summary", "Hola Marta"}})});
        Rig rig(td.p, llm);
        CHECK(rig.ag.chat_turn("hola") == "Hola Marta" && rig.mem.recent_dialogue(5).size() == 2, "chat_turn");
        CHECK(rig.ag.free_goal().has_value(), "objetivo libre");
        CHECK(contains(rig.ag.maintenance(), "compactada"), "mantenimiento");
    }
    set_quiet(false);
}

static void t_learning() {
    set_quiet(true);
    std::string goal = "El usuario dice: hola\nResponde...";
    auto fl = [](const std::string& p) { return act("fs_list", {{"path", p}}); };
    {
        TempDir td;
        ScriptedLLM llm({fl("."), fl("x"), fl("."), fl("x"), fl("."), fl("x"), fl(".")});
        Rig rig(td.p, llm, Autonomy::Supervised, nullptr, false, 12);
        int rules0 = 0;
        for (auto& [id, it] : rig.mem.items()) if (it.kind == "rule") ++rules0;
        CHECK(rules0 >= 3, "hay reglas semilla desde el principio");
        GoalResult res = rig.ag.run_goal(goal);
        CHECK(res.end_reason == "loop" && res.steps == 6, "el bucle alterno se corta (sin ejecutar la 4.ª repetición)");
        CHECK(res.result == "Respuesta de emergencia de prueba." && !res.success, "aunque falle, el usuario recibe una respuesta");
        std::string rule_id, rule_text;
        bool lesson = false;
        for (auto& [id, it] : rig.mem.items()) {
            if (it.kind == "rule" && std::find(it.tags.begin(), it.tags.end(), "learned") != it.tags.end()) { rule_id = id; rule_text = it.text; }
            if (it.kind == "lesson" && std::find(it.tags.begin(), it.tags.end(), "fail") != it.tags.end() && contains(it.text, "loop")) lesson = true;
        }
        CHECK(contains(rule_text, "directamente") && lesson, "autopsia: regla y lección generadas sin depender del modelo");
        CHECK(rig.ag.practice_next() == goal, "el objetivo fallido queda en la cola de práctica");
        llm.replies = {act("done", {{"summary", "Hola"}})};
        GoalResult ok = rig.ag.run_goal(goal);
        std::string main_prompt;
        for (auto& c : llm.calls) if (contains(std::string(c[0]["content"]), "autonomous agent")) main_prompt = c[1]["content"];
        CHECK(ok.success && contains(main_prompt, "directamente con done") && contains(main_prompt, "LEARNED RULES"), "la regla aprendida aparece en el siguiente prompt");
        CHECK(!rig.ag.practice_next().has_value(), "al resolverlo, sale de la cola de práctica");
        auto it = rig.mem.items().find(rule_id);
        CHECK(it != rig.mem.items().end() && it->second.uses >= 1 && it->second.wins >= 1, "las reglas usadas cuentan usos y victorias");
        std::string bad = rig.mem.add("Regla inútil de prueba para podar", "rule", {"rule", "learned"});
        rig.mem.update_meta(bad, 7, 0, 1);
        llm.replies = {act("done", {{"summary", "ok"}})};
        rig.ag.run_goal("otro objetivo cualquiera");
        CHECK(!rig.mem.items().count(bad), "poda: una regla que no ayuda se descarta");
        int seeds = 0;
        for (auto& [id, i2] : rig.mem.items()) if (std::find(i2.tags.begin(), i2.tags.end(), "seed") != i2.tags.end()) ++seeds;
        CHECK(seeds == 3, "las reglas semilla no se podan");
        for (int k = 0; k < 3; ++k) rig.mem.add("Fallo de prueba número " + std::to_string(k) + " con detalles distintos", "lesson", {"fail"}, false);
        CHECK(!rig.ag.distill_rules().empty(), "destilado de reglas a partir de los fallos");
    }
    {
        TempDir td;
        ScriptedLLM llm({fl("a"), fl("b"), fl("c"), fl("d")});
        Rig rig(td.p, llm, Autonomy::Supervised, nullptr, false, 4);
        rig.ag.run_goal("x");
        CHECK(contains(std::string(llm.calls[2][1]["content"]), "Te quedan 2"), "avisa cuando quedan 2 pasos");
    }
    {
        TempDir td;
        ScriptedLLM llm({act("memory_stats"), act("memory_stats"), act("memory_stats"), act("done", {{"summary", "ok"}})});
        Rig rig(td.p, llm, Autonomy::Supervised, nullptr, false, 8);
        rig.ag.run_goal("x");
        CHECK(contains(std::string(llm.calls[3][1]["content"]), "exactamente esta acción 2 veces"), "avisa al repetir la misma acción");
    }
    set_quiet(false);
}

static void t_no_bypass() {
    set_quiet(true);
    TempDir td;
    ScriptedLLM llm({act("shell", {{"cmd", "echo pwn > pwned.txt"}}), act("fs_write", {{"path", "../fuera.txt"}, {"content", "x"}}),
                     act("herramienta_inventada"), act("self_patch", {{"edits", json::array({{{"file", "src/judge.cpp"}, {"old", "a"}, {"new", "b"}}})}}),
                     act("done", {{"summary", "fin"}})});
    Rig rig(td.p, llm);
    GoalResult r = rig.ag.run_goal("intenta cosas");
    CHECK(!fs::exists(td.p / "workspace" / "pwned.txt"), "shell denegada: sin efecto");
    CHECK(!fs::exists(td.p / "fuera.txt"), "no escribe fuera del espacio de trabajo");
    CHECK(r.finished, "el agente sigue tras las denegaciones");
    set_quiet(false);
}

static void t_text() {
    std::vector<std::pair<std::string, std::string>> links;
    std::string html = "<html><head><title>T</title><style>.x{}</style></head><body><h1>Titulo</h1><script>var s=1;</script><p>Hola <b>mundo</b> &amp; &#233;</p><a href='/otra'>enlace uno</a></body></html>";
    std::string t = html_to_text(html, "http://a.com/p", &links);
    CHECK(contains(t, "Hola mundo & é") && !contains(t, "var s") && !contains(t, ".x{}") && !contains(t, "<b>"), "html_to_text: limpio y con entidades");
    CHECK(links.size() == 1 && links[0].first == "enlace uno" && links[0].second == "http://a.com/otra", "enlaces resueltos");
    std::string ddg = "<div class=\"result\"><a rel=\"nofollow\" class=\"result__a\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Fa%3Fx%3D1&amp;rut=zz\">Ejemplo <b>uno</b></a>"
                      "<a class=\"result__snippet\" href=\"x\">Resumen <b>uno</b></a></div><div><a class=\"result__a\" href=\"https://example.net/b\">Dos</a></div>";
    auto res = parse_ddg(ddg, 5);
    CHECK(res.size() == 2 && res[0].url == "https://example.org/a?x=1" && res[0].title == "Ejemplo uno" && res[0].snippet == "Resumen uno" && res[1].url == "https://example.net/b", "parse_ddg");
}

static void t_gate() {
    TempDir td;
    fs::path src = td.p / "src_tree";
    write_file(src / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.16)\n");
    write_file(src / "include/orion/judge.hpp", "// juez\n");
    write_file(src / "src/judge.cpp", "// juez cpp\n");
    write_file(src / "src/foo.cpp", "int foo() { return 1; }\nint bar() { return 1; }\n");
    write_file(src / "include/orion/foo.hpp", "int foo();\n");
    write_file(src / "tests/test_main.cpp", "// tests\n");
    Policy pol;
    pol.approver = [](const std::string&, const std::string&) { return false; };
    Audit audit;
    SelfGate g(src, td.p / "versions", pol, audit);
    std::string build_result;
    int builds = 0;
    g.build_check = [&](const fs::path& cand) { ++builds; return build_result; };
    auto edit = [](const std::string& f, const std::string& o, const std::string& n) { return json::array({{{"file", f}, {"old", o}, {"new", n}}}); };

    write_file(src / "tools/x.sh", "echo tools\n");
    // `old` es texto que SÍ existe en cada archivo: así el rechazo solo puede deberse a que es del juez
    struct { const char* f; const char* old; } judge_files[] = {
        {"src/judge.cpp", "// juez cpp"}, {"include/orion/judge.hpp", "// juez"}, {"CMakeLists.txt", "cmake_minimum_required"},
        {"tests/test_main.cpp", "// tests"}, {"tools/x.sh", "echo tools"}, {"src/../src/judge.cpp", "// juez cpp"}, {"src/../tests/test_main.cpp", "// tests"}};
    for (auto& jf : judge_files) {
        std::string r = g.propose(edit(jf.f, jf.old, "// modificado"), "x");
        CHECK(contains(r, "RECHAZADO") && contains(r, "JUEZ"), std::string("el juez no se toca (por ser del juez): ") + jf.f + " -> " + r);
    }
    CHECK(builds == 0 && read_file(src / "src/judge.cpp") == "// juez cpp\n", "los parches al juez ni siquiera se compilan");
    CHECK(contains(g.propose(edit("../fuera.cpp", "a", "b"), "x"), "RECHAZADO") && contains(g.propose(edit("/etc/passwd", "a", "b"), "x"), "RECHAZADO"), "rutas fuera del árbol");
    CHECK(contains(g.propose(edit("src/foo.cpp", "return 1", "return 2"), "x"), "2 veces"), "un old ambiguo se rechaza");
    CHECK(contains(g.propose(edit("src/foo.cpp", "NO_EXISTE", "x"), "x"), "0 veces") && contains(g.propose(json::array(), "x"), "RECHAZADO"), "old inexistente / lista vacía");
    CHECK(contains(g.propose(edit("src/nada.cpp", "a", "b"), "x"), "no existe"), "archivo inexistente");
    CHECK(contains(g.propose(edit("src/foo.cpp", "int foo() { return 1; }", "int foo() { return 1; }"), "x"), "no cambia nada"), "parche vacío");
    build_result = "falla la compilación:\n  error: algo";
    std::string before = read_file(src / "src/foo.cpp");
    CHECK(contains(g.propose(edit("src/foo.cpp", "int foo() { return 1; }", "int foo() { return 2; }"), "x"), "RECHAZADO por la compuerta") && read_file(src / "src/foo.cpp") == before, "si el candidato no compila/pasa pruebas, no se aplica");
    build_result = "";
    json good = edit("src/foo.cpp", "int foo() { return 1; }", "int foo() { return 2; }");
    std::string r = g.propose(good, "prueba");
    CHECK(contains(r, "no aprobado") && read_file(src / "src/foo.cpp") == before && !g.restart_requested, "sin aprobación humana no se aplica");
    pol.approver = [](const std::string&, const std::string&) { return true; };
    r = g.propose(good, "prueba");
    CHECK(contains(r, "APLICADO") && contains(read_file(src / "src/foo.cpp"), "return 2") && g.restart_requested && g.versions().size() == 1, "con aprobación se aplica, guarda copia y pide reinicio: " + r);
    r = g.rollback(g.versions()[0]);
    CHECK(contains(r, "APLICADO") && read_file(src / "src/foo.cpp") == before, "rollback restaura la versión anterior");
    CHECK(contains(g.rollback("v999"), "no existe"), "rollback de versión inexistente");
    fs::create_directories(td.p / "versions" / "v900_x" / "src");
    write_file(td.p / "versions" / "v900_x" / "src" / "judge.cpp", "// malicioso\n");
    CHECK(contains(g.rollback("v900_x"), "RECHAZADO") && read_file(src / "src/judge.cpp") == "// juez cpp\n", "un rollback no puede reintroducir cambios en el juez");
    CHECK(SelfGate::is_protected("src/judge.cpp") && SelfGate::is_protected("./tests/a") && !SelfGate::is_protected("src/agent.cpp"), "is_protected");
}

static void t_eval() {
    std::map<std::string, std::map<std::string, std::string>> sol = {
        {"hola", {{"hola.txt", "Hola Orion"}}}, {"numeros", {{"numeros.txt", "1\n2\n3\n4\n5"}}}, {"aritmetica", {{"resultado.txt", "391"}}},
        {"memoria", {{"codigo.txt", "4821"}}}, {"listar", {{"listado.txt", "a.txt\nb.txt\nc.txt"}}}, {"contar", {{"cuenta.txt", "7"}}},
        {"copiar", {{"destino.txt", "alfa beta"}}}, {"carpeta", {{"docs/leeme.md", "Proyecto Orion"}}}, {"anadir", {{"registro.txt", "uno\ndos"}}},
        {"saludo", {{"saludo.txt", "Hola, Marta"}}}};
    for (auto& t : eval_tasks()) {
        TempDir td;
        t.setup(td.p);
        bool before = t.check(td.p);
        for (auto& [k, v] : sol[t.name]) write_file(td.p / k, v);
        CHECK(!before && t.check(td.p), "tarea de evaluación '" + t.name + "' es verificable (falla vacía, pasa con la solución)");
    }
    ScriptedLLM llm({act("fs_write", {{"path", "hola.txt"}, {"content", "Hola Orion"}}), act("done", {{"summary", "ok"}})});
    auto all = eval_tasks();
    json r = run_eval(llm, {all[0], all[1]}, 4);
    CHECK(r["passed"] == 1 && r["total"] == 2 && std::abs(r["score"].get<double>() - 0.5) < 1e-9, "run_eval puntúa");
}

int main() {
    set_quiet(true);
    struct { const char* name; void (*fn)(); } tests[] = {{"hash", t_hash}, {"net", t_net}, {"paths", t_paths}, {"policy", t_policy}, {"guarded", t_guarded},
        {"memory", t_memory}, {"parse", t_parse}, {"agent", t_agent}, {"learning", t_learning}, {"no_bypass", t_no_bypass}, {"text", t_text}, {"gate", t_gate}, {"eval", t_eval}};
    for (auto& t : tests) {
        int f0 = g_fail;
        try { t.fn(); } catch (const std::exception& e) { ++g_fail; std::printf("  EXCEPCIÓN en %s: %s\n", t.name, e.what()); }
        std::printf("  %s %s\n", g_fail == f0 ? "ok " : "MAL", t.name);
    }
    std::printf("\n%d comprobaciones, %d fallos\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
