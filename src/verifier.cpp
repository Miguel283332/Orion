#include "demo/verifier.hpp"

#include <algorithm>
#include <regex>
#include <set>

namespace demo {

std::string Verificador::titulo_error(const std::string& resumen) {
    for (auto& l : split_lines(resumen)) if (!trim(l).empty()) return l;
    return "";
}

std::string buscar_en_path(const std::string& nombre) {
    std::string path = env_o("PATH");
#ifdef _WIN32
    const char sep = ';';
    std::vector<std::string> exts = {".exe", ".cmd", ".bat", ""};
#else
    const char sep = ':';
    std::vector<std::string> exts = {""};
#endif
    std::string cur;
    std::vector<std::string> dirs;
    for (char c : path + std::string(1, sep)) { if (c == sep) { if (!cur.empty()) dirs.push_back(cur); cur.clear(); } else cur.push_back(c); }
    for (auto& d : dirs)
        for (auto& e : exts) {
            std::error_code ec;
            fs::path p = fs::path(d) / (nombre + e);
            if (fs::is_regular_file(p, ec)) return p.string();
        }
    return "";
}

static std::string rellenar(std::string t, const std::vector<std::pair<std::string, std::string>>& kv) {
    for (auto& [k, v] : kv) t = replace_all(t, "@" + k + "@", v);
    return t;
}
static std::string lineas_con_prefijo(const std::vector<std::string>& v, const std::string& pre) {
    std::string o;
    for (auto& x : v) o += pre + x + "\n";
    return o.empty() ? "" : o.substr(0, o.size() - 1);
}

// =============================================================================== Lean 4
static const char* GUIA_LEAN = R"G(REGLAS DE LEAN 4:
- Demuestra EXACTAMENTE el enunciado dado: no lo cambies, no añadas hipótesis, no inventes resultados.
- Prohibido: sorry, admit, axiom, native_decide, set_option, macros. Solo tácticas (:= by ...).
- Una táctica por línea, todas con la misma sangría.
TÁCTICAS ÚTILES:
- omega: aritmética lineal sobre Nat/Int (+, -, * por constantes, /, %, <, <=, =, y, o, no).
- simp, simp_all, decide, rfl, exact h, assumption, constructor (para y / si-y-solo-si).
- intro n / intros; obtain ⟨a, b⟩ := h; rcases h with ⟨a, b⟩ | c.
- Existenciales: `exact ⟨testigo, by omega⟩` (las tuplas planas SÍ valen: ∃ x, P x ∧ Q x se prueba con
  `exact ⟨t, by omega, by omega⟩` o con `refine ⟨t, ?_, ?_⟩`).
- Inducción: `induction n with | zero => ... | succ k ih => ...`.
- exact? busca un lema que cierre el objetivo (úsalo si no sabes cuál aplicar).
- SIN Mathlib no existen Even, Odd, Nat.Prime, Finset, Real: «par» es `m % 2 = 0`, «impar» `m % 2 = 1`.)G";

static const char* PISTA_SIN_MATHLIB =
    "PISTA: ese identificador o lema NO existe (sin Mathlib hay muy pocos). No inventes nombres de lemas: usa tácticas básicas (omega, simp, constructor, "
    "`exact ⟨testigo, by omega⟩`). «m es par» se escribe `m % 2 = 0` (o `∃ k, m = 2 * k`) y «m es impar» `m % 2 = 1`. "
    "NO existen Even, Odd, Nat.Prime, Finset, Real ni la notación ℕ (usa Nat).";
static const char* PISTA_MATHLIB =
    "PISTA: ese identificador o lema NO existe con ese nombre. No lo inventes: revisa el nombre, prueba `exact?`, o exprésalo con operaciones básicas "
    "(`m % 2 = 0`) y tácticas como omega/simp.";
static const char* PISTA_SINTAXIS = "PISTA: error de sintaxis; revisa paréntesis, ⟨ ⟩, la sangría (todas las tácticas con la misma) y que cada táctica esté en su línea.";

LeanVerificador::LeanVerificador(const Config& cfg) : cfg_(cfg), mathlib_(cfg.mathlib || !cfg.lake_dir.empty()) {
    nombre = "lean"; extension = "lean"; admite_plan = true;
    sistema = "Eres un experto en matemáticas y en Lean 4. No inventes hipótesis ni resultados. No afirmes que algo funciona sin que el compilador lo haya comprobado.";
    lean_bin_ = !cfg.lean.empty() ? cfg.lean : buscar_en_path("lean");
    if (lean_bin_.empty()) {
        const char* home = std::getenv("HOME");
        if (!home) home = std::getenv("USERPROFILE");
        lean_bin_ = (fs::path(home ? home : ".") / ".elan" / "bin" / "lean").string();
    }
    // autoImplicit desactivado: un identificador desconocido da "unknown identifier 'even'" (claro) en lugar de
    // convertirse en una variable implícita y producir "Function expected" (confuso para el modelo).
    encabezado_ = std::string(mathlib_ ? "import Mathlib" : "import Lean") + "\nset_option autoImplicit false";
}

std::vector<std::string> LeanVerificador::comando(const fs::path& archivo) const {
    if (!cfg_.lake_dir.empty()) {
        std::string lake = buscar_en_path("lake");
        if (lake.empty()) lake = "lake";
        return {lake, "env", lean_bin_, archivo.string()};
    }
    return {lean_bin_, archivo.string()};
}

std::string LeanVerificador::entorno() {
    auto cmd = comando("x.lean");
    cmd.pop_back();
    cmd.push_back("--version");
    OpcionesProceso op;
    op.timeout_s = 60;
    op.cwd = cfg_.lake_dir;
    ResultadoProceso r = ejecutar(cmd, op);
    if (r.no_ejecutado) throw std::runtime_error("No se puede ejecutar Lean 4 (" + lean_bin_ + "). Instálalo con elan o usa --lean RUTA.");
    if (r.codigo != 0) throw std::runtime_error("Lean 4 no responde: " + recortar(r.salida, 200));
    auto l = split_lines(trim(r.salida));
    return l.empty() ? "Lean" : l[0];
}

std::vector<std::string> LeanVerificador::tacticas_auto() const {
    std::vector<std::string> base = {"omega", "(simp; done)", "(simp_all; done)", "decide", "rfl", "(intros; omega)", "(intros; simp_all; done)",
                                     "(constructor <;> omega)", "(constructor <;> simp)"};
    if (mathlib_) for (auto& t : std::vector<std::string>{"(norm_num; done)", "linarith", "nlinarith [sq_nonneg _]", "positivity", "(ring; done)", "aesop"}) base.push_back(t);
    base.push_back("exact?");
    return base;
}

std::pair<bool, std::string> LeanVerificador::comprobar(const std::string& codigo_in, const std::string& nombre_final, bool permitir_sorry) {
    std::string codigo_ = codigo_in;
    if (!nombre_final.empty()) {
        while (!codigo_.empty() && std::isspace((unsigned char)codigo_.back())) codigo_.pop_back();
        codigo_ += "\n\n#print axioms " + nombre_final + "\n";
    }
    fs::path td = crear_temporal("demo_lean_");
    struct Limpia { fs::path p; ~Limpia() { std::error_code ec; fs::remove_all(p, ec); } } limpia{td};
    fs::path f = td / "Problema.lean";
    escribir_archivo(f, codigo_);
    OpcionesProceso op;
    op.timeout_s = cfg_.timeout_lean;
    op.cwd = cfg_.lake_dir.empty() ? td : fs::path(cfg_.lake_dir);
    ResultadoProceso r = ejecutar(comando(f), op);
    if (r.interrumpido) throw Pausa("parada solicitada");
    if (r.tiempo_agotado) return {false, "Lean superó el límite de " + std::to_string(cfg_.timeout_lean) + "s."};
    if (r.no_ejecutado) return {false, "no se pudo ejecutar Lean: " + lean_bin_};
    std::string salida = trim(r.salida);
    static const std::regex re_err(R"(:\d+:\d+: error)");
    if (r.codigo != 0 || std::regex_search(salida, re_err)) return {false, salida.empty() ? "Lean falló sin mensaje." : salida};
    static const std::regex re_sorry("declaration uses (`|'|’)sorry(`|'|’)");
    if (!permitir_sorry && std::regex_search(salida, re_sorry)) return {false, "El código usa sorry (no es una demostración)."};
    if (!nombre_final.empty()) {
        static const std::set<std::string> permitidos = {"propext", "Classical.choice", "Quot.sound"};
        size_t a = salida.find("depends on axioms: [");
        if (a != std::string::npos) {
            size_t b = salida.find(']', a);
            std::string lista = salida.substr(a + 20, b == std::string::npos ? std::string::npos : b - a - 20);
            std::set<std::string> raros;
            std::string cur;
            for (char c : lista + ",") {
                if (c == ',') { std::string t = trim(cur); if (!t.empty() && !permitidos.count(t)) raros.insert(t); cur.clear(); }
                else cur.push_back(c);
            }
            if (!raros.empty()) return {false, "La demostración depende de axiomas no permitidos: " + join(std::vector<std::string>(raros.begin(), raros.end()), ", ")};
        } else if (!contains(salida, "does not depend on any axioms")) {
            return {false, "No se pudo comprobar los axiomas de la demostración:\n" + recortar(salida, 400)};
        }
    }
    return {true, salida};
}

std::optional<std::string> LeanVerificador::prohibido(const std::string& texto) {
    static const std::regex pal(R"(\b(sorry|admit|axiom|native_decide|unsafe|implemented_by|extern|opaque|macro|macro_rules|syntax|elab|notation|infix|infixl|infixr|prefix|postfix|set_option|run_cmd|initialize|builtin_initialize|trace|dbg_trace|dbgTrace|ofReduceBool|bv_decide|namespace|attribute)\b|\+native|#eval|#exit|#reduce)");
    static const std::regex imp(R"(^\s*import\b)");
    for (auto& l : split_lines(quitar_comentarios_lean(texto))) {
        std::smatch m;
        // la primera coincidencia por posición dentro de la línea
        size_t pos_a = std::string::npos, pos_b = std::string::npos;
        std::string ta, tb;
        if (std::regex_search(l, m, pal)) { pos_a = (size_t)m.position(); ta = trim(m.str()); }
        if (std::regex_search(l, m, imp)) { pos_b = (size_t)m.position(); tb = "import"; }
        if (pos_a != std::string::npos || pos_b != std::string::npos) return pos_a <= pos_b ? ta : tb;
    }
    return std::nullopt;
}

std::string LeanVerificador::codigo_lean(const std::string& texto) {
    static const std::regex pista(R"(\b(theorem|lemma)\b|:=\s*by)");
    return elegir_bloque(texto, {"lean4", "lean"}, [](const std::string& b) { return std::regex_search(b, pista); });
}

std::string LeanVerificador::extraer_firma(const std::string& texto) {
    std::string c = quitar_comentarios_lean(codigo_lean(texto));
    static const std::regex kw(R"(\b(?:theorem|lemma)\s+\S)");
    std::smatch m;
    if (!std::regex_search(c, m, kw)) return "";
    c = c.substr((size_t)m.position());
    if (starts_with(c, "lemma")) c = "theorem" + c.substr(5);
    size_t i = c.find(":=");
    if (i == std::string::npos) return "";
    std::string firma;
    {
        std::string cur;
        for (char ch : c.substr(0, i) + " ") {
            if (std::isspace((unsigned char)ch)) { if (!cur.empty()) { firma += (firma.empty() ? "" : " ") + cur; cur.clear(); } }
            else cur.push_back(ch);
        }
    }
    return starts_with(firma, "theorem ") ? firma + " := by" : "";
}

std::string LeanVerificador::extraer_cuerpo(const std::string& texto) {
    std::string c = codigo_lean(texto);
    static const std::regex kw(R"(\b(?:theorem|lemma)\b)");
    static const std::regex pby(R"(:=\s*by\b)");
    std::vector<std::string> cuerpo;
    std::smatch m;
    std::string resto;
    bool con_firma = false;
    if (std::regex_search(c, m, kw)) {
        std::string desde = c.substr((size_t)m.position());
        std::smatch m2;
        if (std::regex_search(desde, m2, pby)) { resto = desde.substr((size_t)(m2.position() + m2.length())); con_firma = true; }
    }
    if (con_firma) {
        auto lineas = split_lines(resto);
        std::string primera = trim(lineas[0]);
        std::vector<std::string> cola;
        for (size_t i = 1; i < lineas.size(); ++i) {
            const std::string& l = lineas[i];
            if (!trim(l).empty() && l[0] != ' ' && l[0] != '\t') break;   // primera línea sin sangría = otro comando
            cola.push_back(l);
        }
        if (!cola.empty()) cuerpo = split_lines(dedent(join(cola, "\n")));
        if (!primera.empty()) cuerpo.insert(cuerpo.begin(), primera);
    } else {
        std::smatch m3;
        resto = std::regex_search(c, m3, pby) ? c.substr((size_t)(m3.position() + m3.length())) : c;
        cuerpo = split_lines(dedent(resto));
    }
    while (!cuerpo.empty() && trim(cuerpo.back()).empty()) cuerpo.pop_back();
    while (!cuerpo.empty() && trim(cuerpo.front()).empty()) cuerpo.erase(cuerpo.begin());
    for (auto& l : cuerpo) while (!l.empty() && std::isspace((unsigned char)l.back())) l.pop_back();
    return cuerpo.empty() ? "" : sangrar(cuerpo);
}

std::string LeanVerificador::nombre_de(const std::string& firma) {
    static const std::regex re(R"(\s*theorem\s+([^\s:({\[]+))");
    std::smatch m;
    return std::regex_search(firma, m, re) && m.position() == 0 ? m[1].str() : "";
}
std::string LeanVerificador::renombrar(const std::string& firma, const std::string& nuevo) {
    static const std::regex re(R"(^\s*theorem\s+[^\s:({\[]+)");
    return std::regex_replace(firma, re, "theorem " + nuevo, std::regex_constants::format_first_only);
}
std::string LeanVerificador::clave(const std::string& firma) {
    static const std::regex a(R"(^\s*(?:theorem|lemma)\s+\S+)"), b(R"(:=\s*by$)");
    std::string t = std::regex_replace(normalizar(firma), a, "", std::regex_constants::format_first_only);
    t = std::regex_replace(t, b, "");
    std::string o;
    for (char c : t) if (!std::isspace((unsigned char)c)) o.push_back(c);
    return o;
}
std::optional<std::string> LeanVerificador::enunciado_trivial(const std::string& firma) {
    static const std::regex a(R"(^theorem\s+\S+)"), b(R"(:=\s*by$)"), t1(R"(:\s*True\s*$)"), t2(R"(\(\s*\w+\s*:\s*False\s*\))");
    std::string t = std::regex_replace(normalizar(firma), a, "", std::regex_constants::format_first_only);
    t = trim(std::regex_replace(t, b, ""));
    bool igual_true = t.size() >= 6 && t.compare(t.size() - 6, 6, "= True") == 0;
    if (std::regex_search(t, t1) || std::regex_search(t, t2) || igual_true) return std::string("el enunciado es trivial (True) o tiene una hipótesis imposible (False)");
    return std::nullopt;
}

std::string LeanVerificador::bloque(const std::string& firma, const std::string& cuerpo) { return firma + "\n" + cuerpo; }

// Consejos según la FORMA del objetivo (un modelo pequeño los necesita).
std::string LeanVerificador::estrategia(const std::string& firma) {
    std::string t = firma.substr(0, firma.find(":="));
    std::vector<std::string> h;
    if (contains(t, "∃")) h.push_back("El objetivo es un EXISTENCIAL: elige un testigo CONCRETO en función de las variables (p. ej. `2 * n + 2`) y escribe `exact ⟨testigo, by omega⟩`. NO busques lemas de la biblioteca ni uses `obtain` sobre lemas que no conozcas.");
    if (contains(t, "↔")) h.push_back("Es un si-y-solo-si: `constructor`, y en cada lado `intro h` y después `omega` (o `simp_all`).");
    if (contains(t, "→") || contains(t, "->")) h.push_back("Hay una implicación: empieza con `intro h` y usa `h` (con `omega` o `exact`).");
    if (contains(t, "%") || contains(t, "/")) h.push_back("`omega` entiende `%` y `/` por constantes: úsalo para cerrar las igualdades y desigualdades.");
    return lineas_con_prefijo(h, "- ");
}

// Alternativas `exact ⟨W, by omega⟩` para objetivos existenciales: prueba testigos lineales típicos sin gastar el modelo.
std::vector<std::string> LeanVerificador::testigos_auto(const std::string& firma) {
    std::string t = firma.substr(0, firma.find(":="));
    if (!contains(t, "∃")) return {};
    static const std::regex binder(R"(\(([^():]+?)\s*:\s*(?:Nat|Int)\))");
    std::vector<std::string> vars;
    for (auto it = std::sregex_iterator(t.begin(), t.end(), binder); it != std::sregex_iterator(); ++it) {
        std::string cur;
        for (char ch : (*it)[1].str() + " ") {
            if (std::isspace((unsigned char)ch)) { if (!cur.empty()) { vars.push_back(cur); cur.clear(); } } else cur.push_back(ch);
        }
    }
    std::vector<std::string> cands;
    for (size_t i = 0; i < vars.size() && i < 2; ++i) {
        const std::string& v = vars[i];
        for (auto& w : std::vector<std::string>{v, v + " + 1", v + " + 2", "2 * " + v, "2 * " + v + " + 1", "2 * " + v + " + 2", "3 * " + v, v + " * " + v, v + " * " + v + " + 1"}) cands.push_back(w);
    }
    for (auto& w : std::vector<std::string>{"0", "1", "2"}) cands.push_back(w);
    std::set<std::string> vistos;
    std::vector<std::string> alts;
    for (auto& w : cands) if (vistos.insert(w).second) alts.push_back("exact ⟨" + w + ", by omega⟩");
    return alts;
}

std::string LeanVerificador::codigo(const json& estado, const std::vector<std::string>& bloques, const std::string& firma, const std::string& cuerpo) {
    std::vector<std::string> partes = {encabezado_, estado.value("definiciones", std::string())};
    partes.insert(partes.end(), bloques.begin(), bloques.end());
    if (!firma.empty()) partes.push_back(firma + "\n" + cuerpo);
    return ensamblar(partes);
}

std::optional<std::string> LeanVerificador::auto_(const json& estado, const std::string& firma, const std::vector<std::string>& bloques) {
    std::vector<std::string> alts = tacticas_auto();
    std::string ultima = alts.back();   // `exact?` (lenta) siempre la última
    alts.pop_back();
    for (auto& w : testigos_auto(firma)) alts.push_back(w);
    alts.push_back(ultima);
    std::string cuerpo = "  first";
    for (auto& a : alts) cuerpo += "\n  | " + a;
    auto [ok, s] = comprobar(codigo(estado, bloques, firma, cuerpo));
    (void)s;
    return ok ? std::optional<std::string>(cuerpo) : std::nullopt;
}

std::string LeanVerificador::resumir_error(const std::string& salida) {
    if (salida.empty()) return "error desconocido";
    static const std::regex inicio(R"(^\S+:\d+:\d+: (error|warning))");
    std::vector<std::string> bloques;
    std::string cur;
    for (auto& l : split_lines(salida)) {
        if (std::regex_search(l, inicio)) { if (!cur.empty()) bloques.push_back(cur); cur = l; }
        else cur += (cur.empty() ? "" : "\n") + l;
    }
    if (!cur.empty()) bloques.push_back(cur);
    std::vector<std::string> errores;
    static const std::regex es_error(R"(^\S+:\d+:\d+: error)");
    for (auto& b : bloques) { std::string t = trim(b); if (std::regex_search(t, es_error)) errores.push_back(t); }
    if (errores.size() > 3) errores.resize(3);
    std::string base = errores.empty() ? salida : join(errores, "\n");
    static const std::regex ruta(R"(\S*Problema\.lean:)");
    base = recortar(std::regex_replace(base, ruta, "línea "), 1400);
    static const std::regex desconocido(R"(unknown (identifier|constant|namespace)|unknownidentifier|unknownconstant|function expected)", std::regex::icase);
    static const std::regex sintaxis(R"(unexpected token|expected term|expected ')");
    if (std::regex_search(salida, desconocido)) base += std::string("\n") + (mathlib_ ? PISTA_MATHLIB : PISTA_SIN_MATHLIB);
    else if (std::regex_search(salida, sintaxis)) base += std::string("\n") + PISTA_SINTAXIS;
    return base;
}

std::string LeanVerificador::titulo_error(const std::string& resumen) {
    std::vector<std::string> ls;
    for (auto& l : split_lines(resumen)) {
        std::string t = trim(l);
        if (!t.empty() && !starts_with(t, "PISTA")) ls.push_back(t);
    }
    if (ls.size() > 3) ls.resize(3);
    return join(ls, " ");
}

std::string LeanVerificador::prompt_formalizar(const std::string& problema, const std::string& error) {
    std::string libs = mathlib_ ? "Puedes usar Mathlib (Real, Finset, Even, Nat.Prime...)."
                                : "Solo hay Lean 4 SIN Mathlib: usa Nat e Int (no Real) y operaciones básicas. «m es par» se escribe `m % 2 = 0` "
                                  "(NO existen Even, Odd, Nat.Prime, Finset ni la notación ℕ: usa Nat).";
    return rellenar(R"P(Traduce el siguiente problema matemático a un ENUNCIADO de Lean 4 (sin demostrarlo).

PROBLEMA:
@PROBLEMA@

REGLAS:
- Conserva exactamente el significado matemático. No lo conviertas en True ni añadas hipótesis.
- @LIBS@
- Incluye solo definiciones realmente necesarias. No uses axiomas ni sorry.
- El teorema debe terminar en `:= by`.

ERROR DEL INTENTO ANTERIOR (si lo hay):
@ERROR@

Devuelve SOLO este JSON:
{"definiciones": "", "teorema": "theorem nombre (...) : proposicion := by"})P",
                    {{"PROBLEMA", problema}, {"LIBS", libs}, {"ERROR", error.empty() ? "(ninguno)" : error}});
}

std::string LeanVerificador::prompt_fidelidad(const std::string& problema, const std::string& defs, const std::string& firma) {
    return rellenar(R"P(Comprueba si este enunciado de Lean 4 expresa EXACTAMENTE el problema (ni más fuerte, ni más débil, ni trivial).

PROBLEMA:
@PROBLEMA@

DEFINICIONES:
@DEFS@

ENUNCIADO LEAN:
@FIRMA@

Devuelve SOLO JSON: {"equivalente": true|false, "motivo": "una frase"})P",
                    {{"PROBLEMA", problema}, {"DEFS", defs.empty() ? "(ninguna)" : defs}, {"FIRMA", firma}});
}

std::string LeanVerificador::prompt_prueba(const std::string& firma, const std::vector<std::string>& bloques, const json& estado, const json& nodo, const std::string&) {
    std::string ctx = bloques.empty() ? "(ninguno)" : join(bloques, "\n\n");
    std::string prev, otros;
    if (!nodo.value("ultimo_codigo", std::string()).empty()) {
        prev = "\nTU INTENTO ANTERIOR (falló):\n```lean\n" + nodo.value("ultimo_codigo", std::string()) + "\n```\nERROR DE LEAN:\n" + nodo.value("ultimo_error", std::string()) +
               "\nCorrige ese intento: analiza el error y cambia lo necesario (no repitas lo mismo).";
    }
    if (nodo.contains("errores") && nodo["errores"].size() > 1) {
        std::vector<std::string> e;
        size_t n = nodo["errores"].size();
        for (size_t i = n >= 4 ? n - 4 : 0; i + 1 < n; ++i) e.push_back(nodo["errores"][i].get<std::string>());
        otros = "\nERRORES DE INTENTOS ANTERIORES (para no repetirlos):\n" + join(e, "\n---\n");
    }
    if (nodo.contains("cuerpos") && nodo["cuerpos"].size() > 1) {
        size_t n = nodo["cuerpos"].size();   // todas salvo la última (ya va en "TU INTENTO ANTERIOR"), como mucho 2
        std::vector<std::string> antes;
        for (size_t i = n >= 3 ? n - 3 : 0; i + 1 < n; ++i) antes.push_back("```lean\n" + nodo["cuerpos"][i].get<std::string>() + "\n```");
        otros += "\nDEMOSTRACIONES QUE YA FALLARON (NO las repitas ni las variantes triviales):\n" + join(antes, "\n---\n");
    }
    std::string est = estrategia(firma);
    if (!est.empty()) otros += "\n\nESTRATEGIA SUGERIDA PARA ESTE OBJETIVO:\n" + est;
    std::string defs = estado.value("definiciones", std::string());
    return rellenar(R"P(Demuestra este teorema en Lean 4.

TEOREMA:
```lean
@FIRMA@
```
DEFINICIONES:
@DEFS@

LEMAS YA DEMOSTRADOS (puedes usarlos por su nombre):
@CTX@
@PREV@@OTROS@

@GUIA@

Devuelve el teorema completo en UN solo bloque ```lean ... ``` (con `:= by` y la demostración).)P",
                    {{"FIRMA", firma}, {"DEFS", defs.empty() ? "(ninguna)" : defs}, {"CTX", ctx}, {"PREV", prev}, {"OTROS", otros}, {"GUIA", GUIA_LEAN}});
}

std::string LeanVerificador::prompt_plan(const std::string& firma, const std::vector<std::string>& bloques, const json& estado, const json& nodo, int max_lemas,
                                         const std::vector<std::string>& fallidos) {
    std::string probados = bloques.empty() ? "(ninguno)" : join(bloques, "\n\n");
    std::vector<std::string> ult;
    for (size_t i = fallidos.size() > 6 ? fallidos.size() - 6 : 0; i < fallidos.size(); ++i) ult.push_back(fallidos[i]);
    std::string f = ult.empty() ? "(ninguno)" : lineas_con_prefijo(ult, "- ");
    std::string defs = estado.value("definiciones", std::string());
    std::string ue = nodo.value("ultimo_error", std::string());
    return rellenar(R"P(Diseña una estrategia para demostrar este teorema en Lean 4 descomponiéndolo en lemas.

TEOREMA PRINCIPAL:
@FIRMA@

DEFINICIONES:
@DEFS@

LEMAS YA DEMOSTRADOS (no los repitas; úsalos):
@PROBADOS@

LEMAS QUE NO SE PUDIERON DEMOSTRAR (propón otros, más sencillos o distintos):
@F@

ÚLTIMO ERROR DEL TEOREMA PRINCIPAL:
@UE@

Propón como máximo @MAX@ lemas NUEVOS, en el orden en que deben demostrarse.
REGLAS: cada lema aporta una propiedad intermedia necesaria; no repitas el teorema principal; sin lemas tautológicos;
sin hipótesis injustificadas; sin sorry/admit/axiom; cada uno termina en `:= by`.

Devuelve SOLO JSON: {"lemas": ["theorem nombre (...) : proposicion := by"]})P",
                    {{"FIRMA", firma}, {"DEFS", defs.empty() ? "(ninguna)" : defs}, {"PROBADOS", probados}, {"F", f}, {"UE", ue.empty() ? "(ninguno)" : ue}, {"MAX", std::to_string(max_lemas)}});
}

// =============================================================================== SymPy
const char* PRELUDIO_PY = R"PY(import sympy as sp
from sympy import *

def verificar(i, cond):
    if not (cond is True or cond == sp.true):
        raise AssertionError("CRITERIO_FALLA %s (el criterio debe ser una comparacion que valga True, p. ej. simplify(...) == 0)" % i)
    print("CRITERIO_OK %s" % i)
)PY";

SympyVerificador::SympyVerificador(const Config& cfg) : cfg_(cfg) {
    nombre = "sympy"; extension = "py"; admite_plan = false;
    sistema = "Eres un físico teórico y matemático riguroso que programa en Python con SymPy. DERIVAS los resultados paso a paso a partir de principios; nunca los das por supuestos ni los escribes directamente.";
    python_ = buscar_en_path("python3");
    if (python_.empty()) python_ = buscar_en_path("python");
    if (python_.empty()) python_ = "python3";
}

std::string SympyVerificador::entorno() {
    OpcionesProceso op;
    op.timeout_s = 60;
    ResultadoProceso r = ejecutar({python_, "-c", "import sympy; print(sympy.__version__)"}, op);
    if (r.no_ejecutado) throw std::runtime_error("No se encuentra Python (python3). El backend sympy lo necesita.");
    if (r.codigo != 0) throw std::runtime_error("Falta SymPy: pip install sympy");
    return "sympy " + trim(r.salida);
}

static bool marcador_unico_final(const std::string& salida) {
    size_t n = 0, pos = 0;
    while ((pos = salida.find("TODO_VERIFICADO", pos)) != std::string::npos) { ++n; pos += 15; }
    auto lineas = split_lines(salida);
    while (!lineas.empty() && trim(lineas.back()).empty()) lineas.pop_back();
    return n == 1 && !lineas.empty() && trim(lineas.back()) == "TODO_VERIFICADO";
}

std::pair<bool, std::string> SympyVerificador::comprobar(const std::string& codigo_, const std::string& nombre_final, bool) {
    fs::path td = crear_temporal("demo_py_");
    struct Limpia { fs::path p; ~Limpia() { std::error_code ec; fs::remove_all(p, ec); } } limpia{td};
    escribir_archivo(td / "derivacion.py", codigo_);
    OpcionesProceso op;
    op.timeout_s = cfg_.timeout_py;
    op.cwd = td;
    op.limites_py = true;
    op.limpiar_secretos = true;
    op.cpu_s = cfg_.timeout_py + 5;
    ResultadoProceso r = ejecutar({python_, "-B", (td / "derivacion.py").string()}, op);
    if (r.interrumpido) throw Pausa("parada solicitada");
    if (r.tiempo_agotado) return {false, "El cálculo superó el límite de " + std::to_string(cfg_.timeout_py) + "s."};
    std::string salida = trim(r.salida);
    if (r.senal != 0)
        return {false, "El proceso fue terminado por el sistema (señal " + std::to_string(r.senal) + "): probablemente superó el límite de CPU o de memoria. Usa un cálculo más ligero (menos simplificaciones pesadas, series cortas)."};
    if (r.codigo != 0) return {false, salida.empty() ? "El script falló sin mensaje." : salida};
    if (!nombre_final.empty() && !marcador_unico_final(salida)) return {false, "El script terminó pero no llegó a comprobar todos los criterios."};
    return {true, salida};
}

std::optional<std::string> SympyVerificador::prohibido(const std::string& texto) {
    static const std::regex re(R"(\b(verificar|subprocess|socket|urllib|requests|http|ftplib|ctypes|shutil|pathlib|importlib|__import__|exec|eval|compile|globals|locals|breakpoint|input|open|os\.system|os\.popen|os\.remove|sys\.exit|exit|quit|os|sys|SystemExit|_exit|builtins|getattr|setattr|sympify|parse_expr|__\w+__)\b)");
    std::smatch m;
    std::string t = quitar_comentarios_py(texto);
    if (std::regex_search(t, m, re)) return m.str();
    return std::nullopt;
}

std::string SympyVerificador::extraer_cuerpo(const std::string& texto) {
    static const std::regex pista_re(R"(import|=)");
    std::string c = elegir_bloque(texto, {"python", "py"}, [](const std::string& b) { return std::regex_search(b, pista_re); });
    std::vector<std::string> keep;
    for (auto& l : split_lines(c)) {
        std::string t = trim(l);
        if (t == "import sympy" || t == "from sympy import *") continue;
        keep.push_back(l);
    }
    std::string d = trim(dedent(join(keep, "\n")));
    return d;
}

std::string SympyVerificador::codigo(const json& estado, const std::vector<std::string>& bloques, const std::string&, const std::string& cuerpo) {
    std::string crit;
    if (estado.contains("criterios"))
        for (size_t i = 0; i < estado["criterios"].size(); ++i) crit += (i ? "\n" : "") + std::string("verificar(") + std::to_string(i + 1) + ", " + estado["criterios"][i].get<std::string>() + ")";
    std::vector<std::string> partes = {PRELUDIO_PY, estado.value("preambulo", std::string())};
    partes.insert(partes.end(), bloques.begin(), bloques.end());
    partes.push_back(cuerpo);
    partes.push_back("# --- criterios fijados antes de resolver ---\n" + crit);
    partes.push_back("print(\"TODO_VERIFICADO\")");
    return ensamblar(partes);
}

std::string SympyVerificador::resumir_error(const std::string& salida) {
    std::vector<std::string> l;
    for (auto& x : split_lines(salida)) if (!trim(x).empty()) l.push_back(x);
    if (l.size() > 12) l.erase(l.begin(), l.end() - 12);
    return l.empty() ? "error desconocido" : recortar(join(l, "\n"), 1400);
}
std::string SympyVerificador::titulo_error(const std::string& resumen) {
    std::string ult;
    for (auto& x : split_lines(resumen)) if (!trim(x).empty()) ult = x;
    return ult;
}

std::string SympyVerificador::prompt_formalizar(const std::string& problema, const std::string& error) {
    return rellenar(R"P(Convierte este problema de física/matemáticas en una ESPECIFICACIÓN verificable con SymPy.

PROBLEMA:
@PROBLEMA@

Debes definir:
1. "preambulo": código Python (SymPy) que declara símbolos, constantes y supuestos (p. ej. `G, M, c, r = symbols('G M c r', positive=True)`).
2. "variables": nombres de las variables que la solución debe DEFINIR (los resultados).
3. "criterios": expresiones Python que valen True solo si el resultado es correcto (p. ej. `simplify(v_esc.subs(r, 2*G*M/c**2) - c) == 0`).
   Deben usar las variables de resultado, ser estrictas (no triviales) y comprobar el resultado pedido, no uno parecido.
4. "descripcion": el objetivo en una frase.

ERROR DEL INTENTO ANTERIOR (si lo hay):
@ERROR@

Devuelve SOLO JSON: {"descripcion": "", "preambulo": "", "variables": ["..."], "criterios": ["..."]})P",
                    {{"PROBLEMA", problema}, {"ERROR", error.empty() ? "(ninguno)" : error}});
}

std::string SympyVerificador::prompt_fidelidad(const std::string& problema, const std::string& defs, const std::string&) {
    return rellenar(R"P(Comprueba si estos criterios verifican EXACTAMENTE lo que pide el problema (ni más débiles, ni triviales, ni de otro resultado).

PROBLEMA:
@PROBLEMA@

@DEFS@

Devuelve SOLO JSON: {"equivalente": true|false, "motivo": "una frase"})P",
                    {{"PROBLEMA", problema}, {"DEFS", defs}});
}

std::string SympyVerificador::prompt_prueba(const std::string& firma, const std::vector<std::string>&, const json& estado, const json& nodo, const std::string&) {
    std::string prev;
    if (!nodo.value("ultimo_codigo", std::string()).empty())
        prev = "\nTU INTENTO ANTERIOR (falló):\n```python\n" + nodo.value("ultimo_codigo", std::string()) + "\n```\nSALIDA / ERROR:\n" + nodo.value("ultimo_error", std::string()) +
               "\nCorrige el fallo concreto; no repitas lo mismo.";
    std::string crit, vars;
    if (estado.contains("criterios"))
        for (size_t i = 0; i < estado["criterios"].size(); ++i) crit += "  " + std::to_string(i + 1) + ". " + estado["criterios"][i].get<std::string>() + (i + 1 < estado["criterios"].size() ? "\n" : "");
    if (estado.contains("variables"))
        for (size_t i = 0; i < estado["variables"].size(); ++i) vars += (i ? ", " : "") + estado["variables"][i].get<std::string>();
    return rellenar(R"P(Resuelve este problema DERIVANDO el resultado con SymPy.

PROBLEMA:
@PROBLEMA@

OBJETIVO: @FIRMA@

CONTEXTO ya ejecutado antes de tu código (no lo repitas):
```python
@PRE@
```
Tu código debe DEFINIR estas variables: @VARS@
Después de tu código se comprobarán automáticamente estos criterios (no los escribas tú):
@CRIT@

REGLAS: deriva el resultado (planteando ecuaciones y resolviendo con solve/dsolve/integrate/series/simplify...), no lo escribas ya hecho;
sin entrada/salida de ficheros ni red; sin `verificar`; el código debe ejecutarse sin errores.
@PREV@

Devuelve SOLO un bloque ```python ... ```.)P",
                    {{"PROBLEMA", estado["problema"].value("enunciado", std::string())}, {"FIRMA", firma}, {"PRE", estado.value("preambulo", std::string())},
                     {"VARS", vars}, {"CRIT", crit}, {"PREV", prev}});
}

}  // namespace demo
