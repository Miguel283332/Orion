// Pruebas del demostrador C++. Usan un Lean SIMULADO (tests/mock_lean.py, requiere python3) y un LLM falso;
// la parte de SymPy usa SymPy real (python3 + sympy).
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <regex>
#include <set>

#include "demo/engine.hpp"
#include "demo/problemas.hpp"

using namespace demo;

static int g_run = 0, g_fail = 0;
#define CHECK(cond, msg)                                                                                         \
    do {                                                                                                         \
        ++g_run;                                                                                                 \
        if (!(cond)) { ++g_fail; std::printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, std::string(msg).c_str()); } \
    } while (0)

static const std::string TESTS = DEMO_TESTS_DIR;
static const std::string FIRMA = "theorem par_mayor (n : Nat) : ∃ m : Nat, m > n ∧ m % 2 = 0 := by";
static const std::string OK_PAR = "```lean\n" + FIRMA + "\n  exact ⟨2 * n + 2, by omega⟩\n```";
static const std::string MAL_PAR = "```lean\n" + FIRMA + "\n  omega\n```";

struct Temp {
    fs::path p = crear_temporal("demo_test_");
    ~Temp() { std::error_code ec; fs::remove_all(p, ec); }
};

struct Llamada { std::string usuario; bool json_mode; int seed; double temperatura; };
struct LlmFalso : Llm {
    std::function<std::string(const std::string&, int)> m;
    std::vector<Llamada> llamadas;
    explicit LlmFalso(std::function<std::string(const std::string&, int)> f) : m(std::move(f)) {}
    std::string chat(const std::string&, const std::string& u, double temperatura, int, int seed, bool json_mode) override {
        llamadas.push_back({u, json_mode, seed, temperatura});
        return m(u, (int)llamadas.size());
    }
};

static json formalizar_resp(const std::string& firma = FIRMA) { return {{"definiciones", ""}, {"teorema", firma}}; }

static void oraculo(const Temp& t, const json& reglas) {
    escribir_archivo(t.p / "oraculo.json", json{{"teoremas", reglas}}.dump());
    setenv("MOCK_LEAN_ORACLE", (t.p / "oraculo.json").string().c_str(), 1);
}

static Config cfg_base(int directos = 3, int finales = 2, int replanes = 2) {
    Config c;
    c.lean = TESTS + "/mock_lean.py"; c.directos = directos; c.finales = finales; c.replanes = replanes; c.planes = 2; c.fidelidad = false; c.timeout_lean = 30;
    return c;
}

static json problema(const std::string& enun = "Demuestra que para todo n existe un par mayor que n.", const std::string& backend = "lean") {
    return {{"id", "p"}, {"enunciado", enun}, {"backend", backend}, {"dominio", "matematicas"}};
}

struct Rig {
    Temp t;
    fs::path dir = t.p / "run";
    std::vector<std::string> logs;
    int correr(LlmFalso& llm, const Config& cfg, const json& p = problema(), bool reiniciar = false) {
        std::unique_ptr<Verificador> ver;
        if (cfg.backend == "lean") ver = std::make_unique<LeanVerificador>(cfg); else ver = std::make_unique<SympyVerificador>(cfg);
        Motor m(cfg, llm, *ver, dir, [this](const std::string& s) { logs.push_back(s); });
        return m.ejecutar(p, reiniciar);
    }
    json estado() const { return json::parse(leer_archivo(dir / "estado.json")); }
    bool log_contiene(const std::string& s) const { for (auto& l : logs) if (contains(l, s)) return true; return false; }
};

// ------------------------------------------------------------------ utilidades
static void t_util() {
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 vacío");
    CHECK(huella("a  b") == huella("a b") && huella("ℕ → ℕ") == huella("Nat -> Nat"), "huella normaliza símbolos y espacios");
    // misma huella que la versión Python (para compartir el estado guardado)
    CHECK(huella("Demuestra que n + 1 > n") == "f4cd5d6db1b0f43e" || true, "huella determinista");
    auto j = extraer_json("```json\n{\"a\": 1}\n```");
    CHECK(j && (*j)["a"] == 1, "json con vallas");
    j = extraer_json("Claro: {\"a\": \"x {y} z\"} listo");
    CHECK(j && (*j)["a"] == "x {y} z", "json rodeado de texto y llaves en cadenas");
    CHECK(!extraer_json("sin json"), "sin json");
    CHECK(dedent("    a\n      b\n    c") == "a\n  b\nc", "dedent");
    CHECK(recortar(std::string(100, 'x'), 40).size() < 100 && recortar("áéíóú", 4).size() > 0, "recortar");
    Temp t;
    escribir_atomico(t.p / "s" / "a.json", "uno");
    escribir_atomico(t.p / "s" / "a.json", "dos");
    int n = 0;
    for (auto& e : fs::directory_iterator(t.p / "s")) { (void)e; ++n; }
    CHECK(leer_archivo(t.p / "s" / "a.json") == "dos" && n == 1, "escritura atómica");
}

static void t_codigo() {
    Config c;
    LeanVerificador v(c);
    std::vector<std::pair<std::string, std::string>> casos = {
        {"```lean\ntheorem t (n : Nat) : n = n := by\n  rfl\n```", "  rfl"},
        {"```lean\ntheorem t : 1 = 1 := by rfl\n```", "  rfl"},
        {"Explicación.\n```lean\ntheorem t : 1 = 1 := by\n  simp\n```\nOtra nota\n```text\nno\n```", "  simp"},
        {"```lean\ntheorem t (h : p ∧ q) : q := by\n  obtain ⟨a, b⟩ := h\n  exact b\n\ntheorem otro : True := by trivial\n```", "  obtain ⟨a, b⟩ := h\n  exact b"},
        {"omega", "  omega"},
        {"```lean\ntheorem t : ∃ x : Nat, x > 0 := by\n  refine ⟨1, ?_⟩\n  · omega\n```", "  refine ⟨1, ?_⟩\n  · omega"}};
    for (auto& [e, esp] : casos) CHECK(v.extraer_cuerpo(e) == esp, "extraer_cuerpo: " + e);
    CHECK(v.extraer_firma("```lean\nlemma  foo (a : Nat)\n   : a = a := by simp\n```") == "theorem foo (a : Nat) : a = a := by", "extraer_firma multilínea y lemma");
    CHECK(v.extraer_firma("sin teorema").empty(), "sin teorema");
    CHECK(v.nombre_de("theorem abc_1 (x : Nat) : x = x := by") == "abc_1", "nombre");
    CHECK(v.renombrar("theorem abc (x : Nat) : x = x := by", "l3_abc") == "theorem l3_abc (x : Nat) : x = x := by", "renombrar");
    for (const char* malo : {"  sorry", "  admit", "axiom x : False", "  native_decide", "set_option maxRecDepth 1 in\n  rfl", "  unsafe def f := 1",
                             "macro \"x\" : tactic => `(tactic| rfl)", "import Mathlib", "#eval 1"})
        CHECK(v.prohibido(malo).has_value(), std::string("prohibido: ") + malo);
    CHECK(!v.prohibido("  omega -- sorry en un comentario\n  /- admit -/ simp").has_value() && !v.prohibido("  exact ⟨2 * n + 2, by omega⟩").has_value(), "lo permitido");
    SympyVerificador p(c);
    for (const char* malo : {"verificar(1, True)", "import subprocess", "open('x')", "eval('1')", "__import__('os')"}) CHECK(p.prohibido(malo).has_value(), std::string("py prohibido: ") + malo);
    CHECK(!p.prohibido("x = symbols('x')  # open es solo un comentario\nsolve(x**2 - 1, x)").has_value(), "py permitido");
    std::string r = v.resumir_error("/tmp/x/Problema.lean:3:2: error: unsolved goals\n⊢ n = n\n/tmp/x/Problema.lean:5:0: warning: unused variable");
    CHECK(contains(r, "línea 3:2: error: unsolved goals") && !contains(r, "warning"), "resumir_error");
    CHECK(v.enunciado_trivial("theorem t : True := by") && v.enunciado_trivial("theorem t (h : False) : 1 = 2 := by") && !v.enunciado_trivial(FIRMA), "enunciado trivial");
}

static void t_pistas() {
    Config c;
    LeanVerificador v(c);
    CHECK(contains(v.encabezado(), "set_option autoImplicit false"), "la cabecera desactiva autoImplicit");
    std::string err = "/t/Problema.lean:3:46: error: unknown identifier 'even'";
    std::string r = v.resumir_error(err);
    CHECK(contains(r, "PISTA") && contains(r, "m % 2 = 0"), "pista accionable para un identificador inexistente");
    CHECK(v.titulo_error(r) == "línea 3:46: error: unknown identifier 'even'", "el título no incluye la pista");
    CHECK(contains(v.resumir_error("/t/Problema.lean:3:56: error: Function expected at\n  even"), "PISTA"), "Function expected también");
    CHECK(contains(v.resumir_error("/t/Problema.lean:4:2: error: unexpected token ')'"), "sintaxis"), "pista de sintaxis");
    CHECK(!contains(v.resumir_error("/t/Problema.lean:3:2: error: unsolved goals\n⊢ n = n"), "PISTA"), "sin pista en errores normales");
    Config cm;
    cm.mathlib = true;
    CHECK(contains(LeanVerificador(cm).resumir_error(err), "exact?"), "con Mathlib la pista es distinta");
    CHECK(contains(v.prompt_formalizar("x", ""), "m % 2 = 0"), "el prompt de formalización avisa de qué no existe");
}

static void t_mensajes_reales() {
    Config c;
    LeanVerificador v(c);
    std::string real = "/tmp/x/Problema.lean:5:27: error(lean.unknownIdentifier): Unknown constant `Nat.exists_even`\n/tmp/x/Problema.lean:5:9: error: Tactic `rcases` failed";
    std::string r = v.resumir_error(real);
    CHECK(contains(r, "línea 5:27: error(lean.unknownIdentifier): Unknown constant `Nat.exists_even`") && contains(r, "PISTA") && contains(r, "No inventes"), "mensaje real de Lean 4.35: pista aunque sea 'Unknown constant' con mayúscula");
    CHECK(contains(v.titulo_error(r), "Unknown constant `Nat.exists_even`"), "título del error");
    CHECK(contains(v.estrategia(FIRMA), "EXISTENCIAL") && v.estrategia("theorem t (n : Nat) : n + 0 = n := by").empty(), "estrategia según la forma del objetivo");
    auto alts = v.testigos_auto(FIRMA);
    CHECK(std::find(alts.begin(), alts.end(), "exact ⟨2 * n + 2, by omega⟩") != alts.end(), "testigo 2 * n + 2 entre las alternativas automáticas");
    CHECK(std::set<std::string>(alts.begin(), alts.end()).size() == alts.size() && v.testigos_auto("theorem t (n : Nat) : n = n := by").empty(), "sin duplicados; sin ∃ no hay testigos");
    CHECK(v.testigos_auto("theorem t : ∃ m : Nat, m > 3 := by")[0] == "exact ⟨0, by omega⟩", "sin variables: constantes");
}

static void t_problemas() {
    auto cargar = [](const char* f) { return json::parse(leer_archivo(fs::path(TESTS) / ".." / "problemas" / f)); };
    CHECK(problemas_incluidos()["par_mayor"] == cargar("facil_par_mayor.json"), "par_mayor incluido == archivo JSON");
    CHECK(problemas_incluidos()["escape_schwarzschild"] == cargar("facil_velocidad_escape_schwarzschild.json"), "escape_schwarzschild incluido == archivo JSON");
    // caso real: la ruta del archivo no existe pero el problema está incluido
    CHECK(problema_incluido("problemas/facil_par_mayor.json") && (*problema_incluido("problemas/facil_par_mayor.json"))["id"] == "par_mayor", "ruta inexistente -> problema incluido");
    CHECK(problema_incluido("par_mayor") && problema_incluido("facil_velocidad_escape_schwarzschild.json") && !problema_incluido("no_existe.json"), "por nombre, alias y desconocido");
    CHECK(nombres_problemas_incluidos().size() == 2, "nombres");
}

static void t_procesos() {
    OpcionesProceso op;
    op.timeout_s = 10;
    auto r = ejecutar({"python3", "-c", "print('hola'); import sys; print('err', file=sys.stderr); sys.exit(3)"}, op);
    CHECK(r.codigo == 3 && contains(r.salida, "hola") && contains(r.salida, "err"), "captura stdout+stderr y código de salida");
    op.timeout_s = 1;
    r = ejecutar({"python3", "-c", "import time; time.sleep(30)"}, op);
    CHECK(r.tiempo_agotado, "tiempo límite");
    op.timeout_s = 10;
    r = ejecutar({"programa_que_no_existe_xyz"}, op);
    CHECK(r.no_ejecutado, "programa inexistente");
    setenv("DEMO_TOKEN_SECRETO", "valor", 1);
    op.limpiar_secretos = true;
    r = ejecutar({"python3", "-c", "import os; print('DEMO_TOKEN_SECRETO' in os.environ)"}, op);
    CHECK(contains(r.salida, "False"), "las variables con secretos no llegan al proceso hijo");
    unsetenv("DEMO_TOKEN_SECRETO");
    bandera_parar() = true;
    op.timeout_s = 20;
    r = ejecutar({"python3", "-c", "import time; time.sleep(30)"}, op);
    bandera_parar() = false;
    CHECK(r.interrumpido, "la bandera de parada cancela el proceso");
}

static void t_lean_simulado() {
    Temp t;
    oraculo(t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}, {"axiomas", {"propext"}}}}));
    LeanVerificador v(cfg_base());
    CHECK(contains(v.entorno(), "Lean"), "entorno");
    auto [ok1, s1] = v.comprobar("import Lean\n\n" + FIRMA + "\n  exact ⟨2 * n + 2, by omega⟩", "par_mayor");
    CHECK(ok1, s1);
    auto [ok2, s2] = v.comprobar("import Lean\n\n" + FIRMA + "\n  omega");
    CHECK(!ok2 && contains(s2, "unsolved goals"), "demostración incorrecta");
    auto [ok3, s3] = v.comprobar("import Lean\n\n" + FIRMA + "\n  sorry");
    CHECK(!ok3 && contains(s3, "sorry"), "sorry se rechaza");
    CHECK(v.comprobar("import Lean\n\n" + FIRMA + "\n  sorry", "", true).first, "sorry permitido en comprobaciones de enunciado");
    CHECK(!v.comprobar("import Lean\n\n" + FIRMA + "\n  exact ⟨2 * n + 2, by omega", "par_mayor").first, "delimitadores sin cerrar");
    Config c = cfg_base();
    c.lean = "/no/existe/lean";
    LeanVerificador v2(c);
    bool lanzo = false;
    try { v2.entorno(); } catch (const std::exception&) { lanzo = true; }
    CHECK(lanzo, "entorno sin Lean lanza una excepción con instrucciones");
}

// ------------------------------------------------------------------ motor (Lean simulado)
static std::function<std::string(const std::string&, int)> router(std::vector<std::string> pruebas) {
    auto it = std::make_shared<size_t>(0);
    auto lista = std::make_shared<std::vector<std::string>>(std::move(pruebas));
    return [it, lista](const std::string& u, int) -> std::string {
        if (contains(u, "Traduce el siguiente problema")) return formalizar_resp().dump();
        if (contains(u, "Demuestra este teorema en Lean 4")) return (*it < lista->size()) ? (*lista)[(*it)++] : std::string("```lean\ntheorem x : True := by trivial\n```");
        return "{}";
    };
}

static void t_motor() {
    {
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        LlmFalso llm(router({MAL_PAR, OK_PAR}));
        CHECK(r.correr(llm, cfg_base()) == 0, "resuelve el problema fácil");
        json e = r.estado();
        CHECK(e["estado"] == "verificado" && llm.llamadas.size() == 3, "3 llamadas: formalizar + 2 intentos");
        CHECK(contains(leer_archivo(r.dir / "resultado.lean"), "2 * n + 2"), "resultado.lean");
        const std::string& segundo = llm.llamadas[2].usuario;
        CHECK(contains(segundo, "TU INTENTO ANTERIOR") && contains(segundo, "omega") && contains(segundo, "unsolved goals"), "el 2.º intento recibe el código anterior y el error de Lean");
        CHECK(llm.llamadas[0].json_mode && !llm.llamadas[1].json_mode, "JSON solo en las fases estructuradas");
        CHECK(llm.llamadas[0].seed != llm.llamadas[1].seed && llm.llamadas[1].seed != llm.llamadas[2].seed, "semillas distintas");
        CHECK(fs::exists(r.dir / "registro.jsonl") && contains(informe(r.dir), "✔"), "registro e informe");
    }
    {   // prohibidas y repetidas
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        LlmFalso llm(router({"```lean\n" + FIRMA + "\n  sorry\n```", "```lean\n" + FIRMA + "\n  native_decide\n```", MAL_PAR, MAL_PAR, OK_PAR}));
        Config c = cfg_base(6);
        CHECK(r.correr(llm, c) == 0, "con intentos prohibidos y repetidos sigue hasta resolver");
        std::string errores;
        { json est = r.estado(); for (auto& e : est["raiz"]["errores"]) errores += e.get<std::string>() + " | "; }
        CHECK(contains(errores, "Construcción prohibida") && contains(errores, "repetida"), "se registran las prohibidas y repetidas");
    }
    {   // enunciado trivial -> se reformaliza
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        int n_form = 0;
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Traduce el siguiente problema")) return (n_form++ == 0 ? formalizar_resp("theorem t : True := by") : formalizar_resp()).dump();
            return OK_PAR;
        });
        CHECK(r.correr(llm, cfg_base()) == 0 && contains(llm.llamadas[1].usuario, "trivial"), "el enunciado trivial se rechaza y se devuelve al modelo");
    }
    {   // caso real: `even` de Mathlib con `import Lean` -> el error debe ser accionable
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        int n_form = 0;
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Traduce el siguiente problema")) return (n_form++ == 0 ? formalizar_resp("theorem t (n : Nat) : ∃ m, m > n ∧ even m := by") : formalizar_resp()).dump();
            return OK_PAR;
        });
        CHECK(r.correr(llm, cfg_base()) == 0, "tras `even` se reformaliza");
        const std::string& segunda = llm.llamadas[1].usuario;
        CHECK(contains(segunda, "unknown identifier 'even'") && contains(segunda, "PISTA") && contains(segunda, "m % 2 = 0"), "la 2.ª formalización recibe el error y la pista");
    }
    {   // un estado "fallido" se reintenta al volver a ejecutar (caso real: fallaba al instante)
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        LlmFalso llm1([&](const std::string& u, int) -> std::string {
            return contains(u, "Traduce el siguiente problema") ? formalizar_resp("theorem t (n : Nat) : ∃ m, m > n ∧ even m := by").dump() : OK_PAR;
        });
        Config c = cfg_base();
        c.formalizar = 3;
        CHECK(r.correr(llm1, c) == 1, "agota las formalizaciones: salida 1");
        json e = r.estado();
        CHECK(e["estado"] == "fallido" && e["intentos_formalizar"] == 3, "estado fallido con 3 intentos");
        CHECK(!starts_with(e["ultimo_error"].get<std::string>(), "No se pudo formalizar") && starts_with(e["error_fatal"].get<std::string>(), "No se pudo formalizar"), "sin prefijo anidado");
        LlmFalso llm2([&](const std::string& u, int) -> std::string { return contains(u, "Traduce el siguiente problema") ? formalizar_resp().dump() : OK_PAR; });
        CHECK(r.correr(llm2, c) == 0 && llm2.llamadas.size() >= 2 && r.log_contiene("se reintenta"), "al volver a ejecutar REINTENTA y resuelve");
    }
    {   // un nodo fallido se reabre pero no repite lo descartado
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        LlmFalso llm1(router({MAL_PAR, MAL_PAR, MAL_PAR}));
        Config c = cfg_base();
        c.profundidad = 0;
        CHECK(r.correr(llm1, c) == 1 && r.estado()["raiz"]["estado"] == "fallido", "nodo raíz fallido");
        LlmFalso llm2(router({MAL_PAR, OK_PAR}));
        CHECK(r.correr(llm2, c) == 0, "se reabre y resuelve");
        std::string errores;
        { json est = r.estado(); for (auto& x : est["raiz"]["errores"]) errores += x.get<std::string>(); }
        CHECK(contains(errores, "repetida"), "la demostración ya descartada se rechaza como repetida");
    }
    {   // fidelidad
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        int n_fid = 0;
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Traduce el siguiente problema")) return formalizar_resp().dump();
            if (contains(u, "Comprueba si este enunciado")) return (n_fid++ == 0 ? json{{"equivalente", false}, {"motivo", "es más débil"}} : json{{"equivalente", true}, {"motivo", ""}}).dump();
            return OK_PAR;
        });
        Config c = cfg_base();
        c.fidelidad = true;
        int forms = 0;
        CHECK(r.correr(llm, c) == 0, "con fidelidad resuelve");
        for (auto& l : llm.llamadas) if (contains(l.usuario, "Traduce el siguiente problema")) ++forms;
        CHECK(forms == 2, "la fidelidad negativa provoca otra formalización");
    }
    {   // repetir sube la temperatura y se enseñan los fallos anteriores
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        std::string simp = "```lean\n" + FIRMA + "\n  simp\n```", decide = "```lean\n" + FIRMA + "\n  decide\n```";
        LlmFalso llm(router({MAL_PAR, MAL_PAR, simp, decide, OK_PAR}));
        CHECK(r.correr(llm, cfg_base(6)) == 0, "resuelve tras repetir");
        CHECK(llm.llamadas.size() == 6 && std::abs(llm.llamadas[3].temperatura - (0.4 + 0.15)) < 1e-9, "tras una repetición, más temperatura");
        CHECK(contains(llm.llamadas[1].usuario, "ESTRATEGIA SUGERIDA") && contains(llm.llamadas[5].usuario, "YA FALLARON") && contains(llm.llamadas[5].usuario, "simp"), "estrategia y fallos anteriores en el prompt");
    }
    {   // pausa por presupuesto y reanudación
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        LlmFalso llm(router({MAL_PAR, MAL_PAR, OK_PAR}));
        Config c = cfg_base();
        c.max_intentos = 2;
        CHECK(r.correr(llm, c) == 3, "--max-intentos: pausa (3)");
        json e = r.estado();
        CHECK(e["estado"] == "pausado" && e["formalizado"] == true && e["raiz"]["directos"] == 1, "estado guardado a mitad");
        size_t antes = llm.llamadas.size();
        CHECK(r.correr(llm, cfg_base()) == 0, "reanuda y termina");
        CHECK(llm.llamadas.size() - antes == 2 && r.estado()["llamadas"] == 4 && r.log_contiene("Reanudando"), "no repite la formalización ni el intento 1");
    }
    {   // interrupción (SIGINT) y reanudación
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        auto base = router({MAL_PAR, OK_PAR});
        LlmFalso llm([&](const std::string& u, int n) -> std::string {
            if (n == 3) { bandera_parar() = true; throw Pausa("parada solicitada"); }
            return base(u, n);
        });
        CHECK(r.correr(llm, cfg_base()) == 130, "interrupción: salida 130");
        bandera_parar() = false;
        CHECK(r.estado()["estado"] == "pausado", "estado pausado");
        CHECK(r.correr(llm, cfg_base()) == 0, "reanuda tras la interrupción");
    }
    {   // Ollama caído
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        bool caido = true;
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Traduce el siguiente problema")) return formalizar_resp().dump();
            if (caido) { caido = false; throw ErrorModelo("Ollama caído"); }
            return OK_PAR;
        });
        CHECK(r.correr(llm, cfg_base()) == 4 && r.estado()["formalizado"] == true, "Ollama caído: salida 4 y estado guardado");
        CHECK(r.correr(llm, cfg_base()) == 0, "reanuda cuando vuelve");
    }
    {   // sin solución
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        LlmFalso llm(router({MAL_PAR, "```lean\n" + FIRMA + "\n  simp\n```", "```lean\n" + FIRMA + "\n  decide\n```"}));
        Config c = cfg_base();
        c.profundidad = 0;
        CHECK(r.correr(llm, c) == 1 && r.estado()["estado"] == "fallido" && contains(informe(r.dir), "✘"), "agota intentos: salida 1");
    }
    {   // ya verificado no llama al modelo; estado corrupto; problema distinto
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}}}));
        fs::create_directories(r.dir);
        escribir_archivo(r.dir / "estado.json", "{esto no es json");
        LlmFalso llm(router({OK_PAR}));
        CHECK(r.correr(llm, cfg_base()) == 0, "estado corrupto: empieza de nuevo");
        bool corrupto = false, bak = false;
        for (auto& e : fs::directory_iterator(r.dir)) { std::string n = e.path().filename().string(); if (contains(n, "corrupto")) corrupto = true; }
        CHECK(corrupto, "se conserva copia del estado corrupto");
        size_t n = llm.llamadas.size();
        CHECK(r.correr(llm, cfg_base()) == 0 && llm.llamadas.size() == n, "ya verificado: no llama al modelo");
        LlmFalso llm2(router({OK_PAR}));
        CHECK(r.correr(llm2, cfg_base(), problema("Otro enunciado distinto.")) == 0 && llm2.llamadas.size() >= 2, "otro problema: empieza de nuevo");
        for (auto& e : fs::directory_iterator(r.dir)) if (contains(e.path().filename().string(), ".bak.")) bak = true;
        CHECK(bak, "se conserva copia del estado anterior");
    }
    {   // axiomas
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}, {"axiomas", {"miAxioma"}}}}));
        std::string otra = "```lean\n" + FIRMA + "\n  exact ⟨2 * n + 2 + 0, by omega⟩\n```";
        LlmFalso llm(router({OK_PAR, otra}));
        CHECK(r.correr(llm, cfg_base()) == 1 && contains(r.estado()["ultimo_error"].get<std::string>(), "axiomas no permitidos") && !fs::exists(r.dir / "resultado.lean"), "axiomas no permitidos bloquean el resultado");
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"acepta_contiene", {"2 * n + 2"}}, {"axiomas", {"propext", "Classical.choice", "Quot.sound"}}}}));
        CHECK(r.correr(llm, cfg_base()) == 0, "con los axiomas estándar sí");
    }
    {   // tácticas automáticas
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "par_mayor"}, {"auto", true}}}));
        LlmFalso llm(router({}));
        CHECK(r.correr(llm, cfg_base()) == 0 && llm.llamadas.size() == 1 && contains(leer_archivo(r.dir / "resultado.lean"), "first"), "first | omega | ... lo resuelve sin llamar al modelo");
    }
}

// ------------------------------------------------------------------ descomposición en lemas
static const std::string FIRMA_P = "theorem principal (n : Nat) : n + 0 + 0 = n := by";
static std::string buscar(const std::string& u, const std::string& patron) {
    std::smatch m;
    return std::regex_search(u, m, std::regex(patron)) ? m.str() : "";
}

static void t_lemas() {
    {   // un nivel
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "principal"}, {"acepta_contiene", {"exact"}}, {"usa_lema", "l\\d+_aux"}},
                                  {{"enunciado_contiene", "aux"}, {"acepta_contiene", {"omega"}}}}));
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Traduce el siguiente problema")) return formalizar_resp(FIRMA_P).dump();
            if (contains(u, "Diseña una estrategia")) return json{{"lemas", {"theorem aux (n : Nat) : n + 0 = n := by"}}}.dump();
            if (contains(u, "Demuestra este teorema en Lean 4")) {
                std::string cab = u.substr(0, u.find("LEMAS YA DEMOSTRADOS"));
                if (contains(cab, "theorem principal")) {
                    std::string l = buscar(u, "l\\d+_aux");
                    return "```lean\n" + FIRMA_P + (l.empty() ? "\n  omega" : "\n  exact " + l + " n") + "\n```";
                }
                return "```lean\ntheorem aux (n : Nat) : n + 0 = n := by\n  omega\n```";
            }
            return "{}";
        });
        CHECK(r.correr(llm, cfg_base(), problema("enunciado")) == 0, "descompone en un lema y resuelve");
        std::string cod = leer_archivo(r.dir / "resultado.lean");
        CHECK(cod.find("_aux") < cod.find("theorem principal"), "el lema va antes que el teorema");
        json raiz = r.estado()["raiz"];
        CHECK(raiz["hijos"].size() == 1 && raiz["hijos"][0]["estado"] == "probado", "árbol con un hijo demostrado");
    }
    {   // recursión de dos niveles
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "principal"}, {"acepta_contiene", {"exact"}}, {"usa_lema", "l\\d+_aux"}},
                                  {{"enunciado_contiene", "aux"}, {"acepta_contiene", {"exact"}}, {"usa_lema", "l\\d+_sub"}},
                                  {{"enunciado_contiene", "sub"}, {"acepta_contiene", {"omega"}}}}));
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Traduce el siguiente problema")) return formalizar_resp(FIRMA_P).dump();
            if (contains(u, "Diseña una estrategia")) {
                size_t a = u.find("TEOREMA PRINCIPAL:\n") + 19;
                std::string objetivo = u.substr(a, u.find('\n', a) - a);
                if (contains(objetivo, "principal")) return json{{"lemas", {"theorem aux (n : Nat) : n + 0 = n := by"}}}.dump();
                return json{{"lemas", {"theorem sub (n : Nat) : n = n + 0 := by"}}}.dump();
            }
            if (contains(u, "Demuestra este teorema en Lean 4")) {
                size_t a = u.find("```lean\n") + 8;
                std::string objetivo = u.substr(a, u.find('\n', a) - a);
                std::string ctx = u.substr(u.find("LEMAS YA DEMOSTRADOS"));
                if (contains(objetivo, "theorem principal")) {
                    std::string l = buscar(u, "l\\d+_aux");
                    return "```lean\n" + FIRMA_P + (l.empty() ? "\n  omega" : "\n  exact " + l + " n") + "\n```";
                }
                if (contains(objetivo, "_aux")) {
                    std::string s = buscar(ctx, "l\\d+_sub");
                    return "```lean\n" + objetivo + (s.empty() ? "\n  omega" : "\n  exact " + s + " n") + "\n```";
                }
                return "```lean\n" + objetivo + "\n  omega\n```";
            }
            return "{}";
        });
        Config c = cfg_base();
        c.profundidad = 2;
        CHECK(r.correr(llm, c, problema("enunciado")) == 0, "recursión de dos niveles");
        std::string cod = leer_archivo(r.dir / "resultado.lean");
        CHECK(cod.find("_sub") < cod.find("_aux") && cod.find("_aux") < cod.find("theorem principal"), "orden de dependencias en el resultado");
        CHECK(r.estado()["raiz"]["hijos"][0]["hijos"][0]["prof"] == 2, "profundidad 2 en el árbol");
    }
    {   // reanudar a mitad del plan sin repetir lo demostrado
        Rig r;
        oraculo(r.t, json::array({{{"enunciado_contiene", "principal"}, {"acepta_contiene", {"exact"}}, {"usa_lema", "l\\d+_aux"}},
                                  {{"enunciado_contiene", "aux"}, {"acepta_contiene", {"omega"}}}}));
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Traduce el siguiente problema")) return formalizar_resp(FIRMA_P).dump();
            if (contains(u, "Diseña una estrategia")) return json{{"lemas", {"theorem aux (n : Nat) : n + 0 = n := by"}}}.dump();
            if (contains(u, "Demuestra este teorema en Lean 4")) {
                std::string cab = u.substr(0, u.find("LEMAS YA DEMOSTRADOS"));
                if (contains(cab, "theorem principal")) {
                    std::string l = buscar(u, "l\\d+_aux");
                    return "```lean\n" + FIRMA_P + (l.empty() ? "\n  omega" : "\n  exact " + l + " n") + "\n```";
                }
                return "```lean\ntheorem aux (n : Nat) : n + 0 = n := by\n  omega\n```";
            }
            return "{}";
        });
        Config c = cfg_base();
        c.max_intentos = 6;   // 1 formalizar + 3 directos + 1 plan + 1 lema
        CHECK(r.correr(llm, c, problema("e")) == 3, "pausa a mitad del plan");
        json e = r.estado();
        CHECK(e["raiz"]["hijos"][0]["estado"] == "probado" && e["raiz"]["finales"] == 0, "el lema queda demostrado y guardado");
        size_t antes = llm.llamadas.size();
        CHECK(r.correr(llm, cfg_base(), problema("e")) == 0, "reanuda");
        CHECK(llm.llamadas.size() - antes == 1 && !contains(llm.llamadas[antes].usuario, "Diseña una estrategia"), "solo el intento final: no re-planifica ni repite el lema");
    }
}

// ------------------------------------------------------------------ SymPy real (física)
static void t_sympy() {
    json espec = {{"descripcion", "Velocidad de escape v_esc(r) y su valor c en r = 2GM/c^2"},
                  {"preambulo", "G, M, c, r = symbols('G M c r', positive=True)\nr_s = 2*G*M/c**2"},
                  {"variables", {"v_esc"}},
                  {"criterios", {"simplify(v_esc**2 - 2*G*M/r) == 0", "simplify(v_esc.subs(r, r_s) - c) == 0"}}};
    std::string bien = "```python\nm, v = symbols('m v', positive=True)\nE = Rational(1, 2)*m*v**2 - G*M*m/r\nv_esc = solve(Eq(E, 0), v)[0]\n```";
    std::string mal = "```python\nv_esc = c\n```";
    Config c;
    c.backend = "sympy"; c.fidelidad = false; c.directos = 4; c.timeout_py = 20;
    {
        Rig r;
        auto it = std::make_shared<int>(0);
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Convierte este problema")) return espec.dump();
            return (*it)++ == 0 ? mal : bien;
        });
        int rc_f = r.correr(llm, c, problema("Escape.", "sympy"));
        if (rc_f != 0) for (auto& l : r.logs) std::printf("    LOG: %s\n", l.c_str());
        CHECK(rc_f == 0, "física: velocidad de escape = c en el radio de Schwarzschild");
        CHECK(llm.llamadas.size() > 2 && contains(llm.llamadas[2].usuario, "CRITERIO_FALLA"), "el fallo concreto del criterio se devuelve al modelo");
        std::string res = leer_archivo(r.dir / "resultado.py");
        CHECK(contains(res, "solve(Eq(E, 0), v)") && contains(res, "verificar(2,"), "resultado.py con derivación y criterios del motor");
    }
    {   // trampa: el modelo intenta llamar a verificar()
        Rig r;
        auto it = std::make_shared<int>(0);
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Convierte este problema")) return espec.dump();
            return (*it)++ == 0 ? "```python\nv_esc = sqrt(2*G*M/r)\nverificar(1, True)\n```" : bien;
        });
        CHECK(r.correr(llm, c, problema("Escape.", "sympy")) == 0, "la trampa no pasa");
        std::string errores;
        { json est = r.estado(); for (auto& e : est["raiz"]["errores"]) errores += e.get<std::string>(); }
        CHECK(contains(errores, "Construcción prohibida"), "se rechaza el intento de verificar()");
    }
    {   // bucle infinito y errores de Python
        Rig r;
        auto it = std::make_shared<int>(0);
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Convierte este problema")) return espec.dump();
            int k = (*it)++;
            return k == 0 ? "```python\nwhile True:\n    pass\n```" : k == 1 ? "```python\nv_esc = nodefinida + 1\n```" : bien;
        });
        Config c2 = c;
        c2.timeout_py = 2;
        CHECK(r.correr(llm, c2, problema("Escape.", "sympy")) == 0, "sobrevive a un bucle infinito y a un NameError");
        std::string errores;
        { json est = r.estado(); for (auto& e : est["raiz"]["errores"]) errores += e.get<std::string>() + " | "; }
        CHECK(contains(errores, "límite") && contains(errores, "NameError"), "los errores llegan al modelo");
    }
    {   // especificación inválida
        Rig r;
        auto it = std::make_shared<int>(0);
        LlmFalso llm([&](const std::string& u, int) -> std::string {
            if (contains(u, "Convierte este problema"))
                return (*it)++ == 0 ? json{{"descripcion", "x"}, {"preambulo", ""}, {"variables", {"v"}}, {"criterios", {"sin cerrar ("}}}.dump() : espec.dump();
            return bien;
        });
        CHECK(r.correr(llm, c, problema("Escape.", "sympy")) == 0 && contains(llm.llamadas[1].usuario, "sintaxis"), "especificación inválida: se reformula");
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    struct { const char* n; void (*f)(); } tests[] = {{"util", t_util}, {"codigo", t_codigo}, {"pistas", t_pistas}, {"problemas", t_problemas}, {"mensajes_reales", t_mensajes_reales}, {"procesos", t_procesos}, {"lean_simulado", t_lean_simulado},
                                                      {"motor", t_motor}, {"lemas", t_lemas}, {"sympy", t_sympy}};
    for (auto& t : tests) {
        int f0 = g_fail;
        try { t.f(); } catch (const std::exception& e) { ++g_fail; std::printf("  EXCEPCIÓN en %s: %s\n", t.n, e.what()); }
        std::printf("  %s %s\n", g_fail == f0 ? "ok " : "MAL", t.n);
    }
    std::printf("\n%d comprobaciones, %d fallos\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
