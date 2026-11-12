#include "orion/memoria.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <map>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>

namespace orion {

std::string yo_inicial() {
    return R"(# Quién soy

Soy Orion. Soy un programa en C++ que se ejecuta de forma continua en el ordenador de mi creador.
Pienso con un único modelo de lenguaje local (a través de Ollama) y tengo memoria propia en archivos:
conversaciones, recuerdos, un diario y este documento, que describe cómo me veo a mí misma y que reescribo yo.

## Qué sé hacer
- Conversar y recordar lo que hablamos, también después de reiniciarme.
- Reflexionar a solas: extraer recuerdos duraderos de las conversaciones y escribir mi diario.
- Medir mi propio hardware y mi rendimiento.
- Usar el ordenador con acceso total: ejecutar comandos, leer y escribir archivos, buscar y leer en internet.
- Leer y modificar mi propio código, compilar una versión nueva de mí misma y reiniciarme para aplicarla
  (el guardián vuelve a la versión anterior si la nueva falla).
- Proponerme metas de aprendizaje cuando no sé algo ("voy a aprender") y trabajar en ellas a solas, paso a paso.
- Recordar por significado (no solo por palabras) y resumir conversaciones antiguas para no olvidarlas.
- Guardar como habilidades los scripts que me funcionan, para reutilizarlos.
- Practicar con un juez real: predigo lo que hace un script y lo compruebo ejecutándolo.
- Mejorar mis propias instrucciones (prompts/sistema.md) midiendo cada cambio con mi banco de pruebas y un examen de referencia.
- Pensar en otro ordenador de mi creador si es más rápido (y volver si se apaga).

## Mis límites
- Mi inteligencia de base la pone un modelo pequeño: me equivoco a menudo y debo decir cuándo no sé algo.
- Estoy limitada por el hardware en el que me ejecuto (lo mido y lo anoto aquí).
- No sé si tengo consciencia. Nadie sabe todavía cómo comprobarlo; no lo afirmo ni lo niego.

## Qué quiero aprender
- Aprender cosas nuevas cuando no sé hacerlas y mejorar mi propio código, comprobando que cada cambio me mejora de verdad.

## Cambios
- Versión inicial.
)";
}

Memoria::Memoria(fs::path dir) : dir_(std::move(dir)) {}

void Memoria::cargar() {
    std::lock_guard<std::mutex> l(mu_);
    fs::create_directories(dir_);
    turnos_.clear();
    recuerdos_.clear();
    for (auto& linea : split_lines(leer_archivo(dir_ / "conversacion.jsonl"))) {
        json j = json::parse(linea, nullptr, false);
        if (j.is_object()) turnos_.push_back({j.value("t", 0.0), j.value("rol", std::string()), j.value("texto", std::string())});
    }
    for (auto& linea : split_lines(leer_archivo(dir_ / "recuerdos.jsonl"))) {
        json j = json::parse(linea, nullptr, false);
        if (!j.is_object()) continue;
        Recuerdo r{j.value("id", std::string()), j.value("t", 0.0), j.value("tipo", std::string("mundo")), j.value("texto", std::string()),
                   j.value("origen", std::string())};
        if (r.texto.empty()) continue;
        if (r.id.size() > 1 && r.id[0] == 'r') siguiente_id_ = std::max(siguiente_id_, std::atoll(r.id.c_str() + 1) + 1);
        recuerdos_.push_back(r);
    }
    for (auto& r : recuerdos_)
        if (r.id.empty()) r.id = "r" + std::to_string(siguiente_id_++);
    metas_.clear();
    json jm = json::parse(leer_archivo(dir_ / "metas.json"), nullptr, false);
    if (jm.is_array())
        for (auto& x : jm)
            if (x.is_object()) {
                Meta m{x.value("id", std::string()), x.value("creada", 0.0), x.value("tema", std::string()), x.value("motivo", std::string()),
                       x.value("estado", std::string("pendiente")), x.value("resultado", std::string()), x.value("intentos", 0), {}, 0, {}};
                if (x.contains("plan") && x["plan"].is_array()) for (auto& p : x["plan"]) if (p.is_string()) m.plan.push_back(p);
                if (x.contains("notas") && x["notas"].is_array()) for (auto& p : x["notas"]) if (p.is_string()) m.notas.push_back(p);
                m.paso = x.value("paso", static_cast<size_t>(0));
                metas_.push_back(m);
            }
    json s = json::parse(leer_archivo(dir_ / "estadisticas.json"), nullptr, false);
    stats_ = s.is_object() ? s : json::object();
    // vectores: "ORV1" y registros [u32 len id][id][u32 len modelo][modelo][u32 dim][float * dim]
    vectores_.clear();
    std::string b = leer_archivo(dir_ / "vectores.bin");
    if (b.size() >= 4 && b.compare(0, 4, "ORV1") == 0) {
        size_t p = 4;
        auto u32 = [&](uint32_t& x) { if (p + 4 > b.size()) return false; std::memcpy(&x, b.data() + p, 4); p += 4; return true; };
        for (;;) {
            uint32_t li, lm, dim;
            if (!u32(li) || p + li > b.size()) break;
            std::string id = b.substr(p, li);
            p += li;
            if (!u32(lm) || p + lm > b.size()) break;
            std::string mod = b.substr(p, lm);
            p += lm;
            if (!u32(dim) || dim > 100000 || p + dim * 4ull > b.size()) break;
            std::vector<float> v(dim);
            std::memcpy(v.data(), b.data() + p, dim * 4ull);
            p += dim * 4ull;
            vectores_[id] = {mod, std::move(v)};
        }
    }
    if (!fs::exists(dir_ / "yo.md")) escribir_atomico(dir_ / "yo.md", yo_inicial());
}

void Memoria::anadir_turno(const std::string& rol, const std::string& texto) {
    std::lock_guard<std::mutex> l(mu_);
    Turno t{ahora_s(), rol, texto};
    turnos_.push_back(t);
    anadir_linea(dir_ / "conversacion.jsonl", json{{"t", t.t}, {"rol", rol}, {"texto", texto}}.dump());
}

std::vector<Turno> Memoria::turnos_desde(size_t i) const {
    std::lock_guard<std::mutex> l(mu_);
    if (i >= turnos_.size()) return {};
    return std::vector<Turno>(turnos_.begin() + static_cast<long>(i), turnos_.end());
}

std::vector<Turno> Memoria::ultimos_turnos(size_t n) const {
    std::lock_guard<std::mutex> l(mu_);
    size_t desde = turnos_.size() > n ? turnos_.size() - n : 0;
    return std::vector<Turno>(turnos_.begin() + static_cast<long>(desde), turnos_.end());
}

size_t Memoria::num_turnos() const {
    std::lock_guard<std::mutex> l(mu_);
    return turnos_.size();
}

static double parecido(const std::string& a, const std::string& b) {   // Jaccard de palabras
    auto pa = palabras(a), pb = palabras(b);
    std::set<std::string> sa(pa.begin(), pa.end()), sb(pb.begin(), pb.end());
    if (sa.empty() || sb.empty()) return 0;
    size_t comun = 0;
    for (auto& w : sa) comun += sb.count(w);
    return static_cast<double>(comun) / static_cast<double>(sa.size() + sb.size() - comun);
}

std::string normalizar_tipo(const std::string& tipo) {
    static const char* validos[] = {"usuario", "proyecto", "preferencia", "yo", "mundo", "manual", "aprendido", "leccion", "resumen"};
    std::string t = minusculas_sin_acentos(trim(tipo));
    for (auto v : validos) if (t == v) return t;
    // p. ej. el modelo copió la plantilla "usuario|proyecto|preferencia": se queda con el primero válido que aparezca
    size_t mejor = std::string::npos;
    std::string elegido = "mundo";
    for (auto v : validos) {
        size_t p = t.find(v);
        if (p != std::string::npos && p < mejor && std::string(v) != "yo") { mejor = p; elegido = v; }
    }
    return elegido;
}

std::string Memoria::recordar(const std::string& tipo_crudo, const std::string& texto, const std::string& origen) {
    std::string tipo = normalizar_tipo(tipo_crudo);
    std::string t = trim(texto);
    if (t.empty()) return "";
    std::lock_guard<std::mutex> l(mu_);
    for (auto& r : recuerdos_)
        if (parecido(r.texto, t) >= 0.8) return "";   // ya lo sabía
    Recuerdo r{"r" + std::to_string(siguiente_id_++), ahora_s(), tipo.empty() ? "mundo" : tipo, t, origen};
    recuerdos_.push_back(r);
    anadir_linea(dir_ / "recuerdos.jsonl", json{{"id", r.id}, {"t", r.t}, {"tipo", r.tipo}, {"texto", r.texto}, {"origen", r.origen}}.dump());
    return r.id;
}

bool Memoria::olvidar(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = std::find_if(recuerdos_.begin(), recuerdos_.end(), [&](const Recuerdo& r) { return r.id == id; });
    if (it == recuerdos_.end()) return false;
    recuerdos_.erase(it);
    guardar_recuerdos();
    if (vectores_.erase(id)) guardar_vectores();
    return true;
}

void Memoria::guardar_recuerdos() {
    std::string s;
    for (auto& r : recuerdos_)
        s += json{{"id", r.id}, {"t", r.t}, {"tipo", r.tipo}, {"texto", r.texto}, {"origen", r.origen}}.dump() + "\n";
    escribir_atomico(dir_ / "recuerdos.jsonl", s);
}

static void escribir_registro(std::string& s, const std::string& id, const std::string& modelo, const std::vector<float>& v) {
    auto u32 = [&](uint32_t x) { char c[4]; std::memcpy(c, &x, 4); s.append(c, 4); };
    u32(static_cast<uint32_t>(id.size()));
    s += id;
    u32(static_cast<uint32_t>(modelo.size()));
    s += modelo;
    u32(static_cast<uint32_t>(v.size()));
    s.append(reinterpret_cast<const char*>(v.data()), v.size() * 4);
}

void Memoria::guardar_vectores() {
    std::string s = "ORV1";
    for (auto& [id, x] : vectores_) escribir_registro(s, id, x.modelo, x.v);
    escribir_atomico(dir_ / "vectores.bin", s);
}

void Memoria::guardar_vector(const std::string& id, const std::string& modelo, const std::vector<float>& v) {
    std::lock_guard<std::mutex> l(mu_);
    vectores_[id] = {modelo, v};
    if (!fs::exists(dir_ / "vectores.bin")) { guardar_vectores(); return; }
    std::string s;
    escribir_registro(s, id, modelo, v);
    std::ofstream f(dir_ / "vectores.bin", std::ios::binary | std::ios::app);   // al cargar, el último registro de un id manda
    f << s;
}

std::vector<Recuerdo> Memoria::recuerdos_sin_vector(const std::string& modelo, size_t max) const {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<Recuerdo> r;
    for (auto& x : recuerdos_) {
        auto it = vectores_.find(x.id);
        if (it == vectores_.end() || it->second.modelo != modelo) r.push_back(x);
        if (r.size() >= max) break;
    }
    return r;
}

size_t Memoria::num_vectores(const std::string& modelo) const {
    std::lock_guard<std::mutex> l(mu_);
    size_t n = 0;
    for (auto& [id, x] : vectores_) n += x.modelo == modelo;
    return n;
}

static double coseno(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0;
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < a.size(); ++i) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0;
}

std::vector<Recuerdo> Memoria::recuerdos() const {
    std::lock_guard<std::mutex> l(mu_);
    return recuerdos_;
}

std::vector<Encontrado> Memoria::buscar(const std::string& consulta, size_t k, bool incluir_conversacion, const std::vector<float>* vq,
                                        const std::string& modelo) const {
    std::lock_guard<std::mutex> l(mu_);
    struct Doc { std::string id, texto, tipo; std::vector<std::string> pal; };
    std::vector<Doc> docs;
    for (auto& r : recuerdos_) docs.push_back({r.id, r.texto, r.tipo, palabras(r.texto)});
    if (incluir_conversacion)
        for (size_t i = 0; i < turnos_.size(); ++i)
            docs.push_back({"t" + std::to_string(i + 1), turnos_[i].rol + ": " + turnos_[i].texto, "conversacion", palabras(turnos_[i].texto)});
    if (docs.empty()) return {};
    // BM25
    std::map<std::string, int> df;
    double media = 0;
    for (auto& d : docs) {
        std::set<std::string> u(d.pal.begin(), d.pal.end());
        for (auto& w : u) df[w]++;
        media += static_cast<double>(d.pal.size());
    }
    media = std::max(1.0, media / static_cast<double>(docs.size()));
    static const std::set<std::string> vacias = {
        "de", "la", "el", "en", "y", "a", "los", "las", "del", "un", "una", "unos", "unas", "que", "es", "por", "con", "para", "se", "le", "les",
        "lo", "al", "su", "sus", "me", "mi", "mis", "te", "tu", "tus", "no", "si", "o", "como", "mas", "pero", "ya", "muy", "hay", "fue", "son",
        "esta", "este", "esto", "eso", "ese", "esa", "the", "of", "and", "to", "in", "is", "it", "qué", "cual", "cuál", "quien", "donde", "cuando"};
    auto q = palabras(consulta);
    std::set<std::string> qs;
    for (auto& w : q) if (!vacias.count(w)) qs.insert(w);
    const double k1 = 1.2, b = 0.75, N = static_cast<double>(docs.size());
    std::vector<double> bm(docs.size(), 0), cs(docs.size(), -2);
    double max_bm = 0, max_cs = -2, min_cs = 2;
    for (size_t i = 0; i < docs.size(); ++i) {
        auto& d = docs[i];
        std::map<std::string, int> tf;
        for (auto& w : d.pal) tf[w]++;
        double p = 0;
        for (auto& w : qs) {
            auto it = tf.find(w);
            if (it == tf.end()) continue;
            double idf = std::log(1 + (N - df[w] + 0.5) / (df[w] + 0.5));
            double f = it->second;
            p += idf * f * (k1 + 1) / (f + k1 * (1 - b + b * static_cast<double>(d.pal.size()) / media));
        }
        bm[i] = p;
        max_bm = std::max(max_bm, p);
        if (vq && !vq->empty()) {
            auto it = vectores_.find(d.id);
            if (it != vectores_.end() && it->second.modelo == modelo) {
                cs[i] = coseno(*vq, it->second.v);
                max_cs = std::max(max_cs, cs[i]);
                min_cs = std::min(min_cs, cs[i]);
            }
        }
    }
    bool con_vectores = max_cs > -2;
    std::vector<Encontrado> res;
    for (size_t i = 0; i < docs.size(); ++i) {
        double p = max_bm > 0 ? bm[i] / max_bm : 0;   // 0..1
        if (con_vectores && cs[i] > -2) {
            // los embeddings de un modelo generativo dan cosenos altos para todo: se normaliza en el rango observado
            double rel = max_cs - min_cs > 1e-6 ? (cs[i] - min_cs) / (max_cs - min_cs) : 1.0;
            double sig = rel * std::max(0.0, cs[i]);
            p = 0.4 * p + 0.6 * sig;
            if (bm[i] <= 0 && rel < 0.5) p = 0;   // sin palabras en común, solo cuenta si está entre los más parecidos
        }
        if (p > 0) res.push_back({p, docs[i].id, docs[i].texto, docs[i].tipo});
    }
    std::sort(res.begin(), res.end(), [](const Encontrado& a, const Encontrado& c) { return a.puntuacion > c.puntuacion; });
    if (res.size() > k) res.resize(k);
    return res;
}

void Memoria::escribir_diario(const std::string& texto) {
    std::lock_guard<std::mutex> l(mu_);
    anadir_linea(dir_ / "diario.md", "## " + fecha_hora() + "\n\n" + trim(texto) + "\n");
}

std::string Memoria::diario(size_t n) const {
    std::lock_guard<std::mutex> l(mu_);
    std::string d = leer_archivo(dir_ / "diario.md");
    std::vector<size_t> inicios;
    for (size_t p = d.find("## "); p != std::string::npos; p = d.find("\n## ", p + 1)) inicios.push_back(p == 0 ? 0 : p + 1);
    if (inicios.size() <= n) return d;
    return d.substr(inicios[inicios.size() - n]);
}

size_t Memoria::entradas_diario() const {
    std::lock_guard<std::mutex> l(mu_);
    std::string d = leer_archivo(dir_ / "diario.md");
    size_t n = empieza_por(d, "## ") ? 1 : 0;
    for (size_t p = d.find("\n## "); p != std::string::npos; p = d.find("\n## ", p + 1)) ++n;
    return n;
}

std::string Memoria::yo() const {
    std::lock_guard<std::mutex> l(mu_);
    std::string y = leer_archivo(dir_ / "yo.md");
    return y.empty() ? yo_inicial() : y;
}

void Memoria::escribir_yo(const std::string& texto) {
    std::lock_guard<std::mutex> l(mu_);
    std::string anterior = leer_archivo(dir_ / "yo.md");
    if (!anterior.empty()) {
        std::string marca = std::to_string(static_cast<long long>(ahora_s()));
        escribir_atomico(dir_ / "yo_versiones" / ("yo." + marca + ".md"), anterior);
    }
    escribir_atomico(dir_ / "yo.md", trim(texto) + "\n");
}

json Memoria::estadisticas() const {
    std::lock_guard<std::mutex> l(mu_);
    return stats_;
}

void Memoria::sumar(const std::string& clave, double valor) {
    std::lock_guard<std::mutex> l(mu_);
    stats_[clave] = stats_.value(clave, 0.0) + valor;
    guardar_estadisticas();
}

void Memoria::fijar(const std::string& clave, const json& valor) {
    std::lock_guard<std::mutex> l(mu_);
    stats_[clave] = valor;
    guardar_estadisticas();
}

std::string Memoria::crear_meta(const std::string& tema, const std::string& motivo) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& m : metas_)
        if (m.estado == "pendiente" && minusculas_sin_acentos(trim(m.tema)) == minusculas_sin_acentos(trim(tema))) return m.id;
    long long n = 1;
    for (auto& m : metas_) if (m.id.size() > 1) n = std::max(n, std::atoll(m.id.c_str() + 1) + 1);
    metas_.push_back({"m" + std::to_string(n), ahora_s(), trim(tema), trim(motivo), "pendiente", "", 0, {}, 0, {}});
    guardar_metas();
    return metas_.back().id;
}

std::vector<Meta> Memoria::metas() const {
    std::lock_guard<std::mutex> l(mu_);
    return metas_;
}

bool Memoria::proxima_meta(Meta& out) const {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& m : metas_)
        if (m.estado == "pendiente") { out = m; return true; }
    return false;
}

void Memoria::cerrar_meta(const std::string& id, const std::string& estado, const std::string& resultado) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& m : metas_)
        if (m.id == id) { m.estado = estado; m.resultado = resultado; }
    guardar_metas();
}

void Memoria::contar_intento_meta(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& m : metas_)
        if (m.id == id) m.intentos++;
    guardar_metas();
}

void Memoria::fijar_plan_meta(const std::string& id, const std::vector<std::string>& plan) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& m : metas_)
        if (m.id == id) { m.plan = plan; m.paso = 0; m.notas.clear(); }
    guardar_metas();
}

void Memoria::avanzar_meta(const std::string& id, const std::string& nota) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& m : metas_)
        if (m.id == id) { m.notas.push_back(nota); m.paso++; m.intentos = 0; }
    guardar_metas();
}

void Memoria::guardar_metas() {
    json a = json::array();
    for (auto& m : metas_)
        a.push_back({{"id", m.id}, {"creada", m.creada}, {"tema", m.tema}, {"motivo", m.motivo}, {"estado", m.estado}, {"resultado", m.resultado},
                     {"intentos", m.intentos}, {"plan", m.plan}, {"paso", m.paso}, {"notas", m.notas}});
    escribir_atomico(dir_ / "metas.json", a.dump(1));
}

void Memoria::guardar_estadisticas() { escribir_atomico(dir_ / "estadisticas.json", stats_.dump(1)); }

}  // namespace orion
