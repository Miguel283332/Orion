#include "orion/memoria.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <map>
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

## Mis límites
- Mi inteligencia de base la pone un modelo pequeño: me equivoco a menudo y debo decir cuándo no sé algo.
- Estoy limitada por el hardware en el que me ejecuto (lo mido y lo anoto aquí).
- No sé si tengo consciencia. Nadie sabe todavía cómo comprobarlo; no lo afirmo ni lo niego.

## Qué quiero aprender
- Aprender cosas nuevas cuando no sé hacerlas, y mejorar mi propio código (fases siguientes de mi desarrollo).

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
    json s = json::parse(leer_archivo(dir_ / "estadisticas.json"), nullptr, false);
    stats_ = s.is_object() ? s : json::object();
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

std::string Memoria::recordar(const std::string& tipo, const std::string& texto, const std::string& origen) {
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
    return true;
}

void Memoria::guardar_recuerdos() {
    std::string s;
    for (auto& r : recuerdos_)
        s += json{{"id", r.id}, {"t", r.t}, {"tipo", r.tipo}, {"texto", r.texto}, {"origen", r.origen}}.dump() + "\n";
    escribir_atomico(dir_ / "recuerdos.jsonl", s);
}

std::vector<Recuerdo> Memoria::recuerdos() const {
    std::lock_guard<std::mutex> l(mu_);
    return recuerdos_;
}

std::vector<Encontrado> Memoria::buscar(const std::string& consulta, size_t k, bool incluir_conversacion) const {
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
    auto q = palabras(consulta);
    std::set<std::string> qs(q.begin(), q.end());
    const double k1 = 1.2, b = 0.75, N = static_cast<double>(docs.size());
    std::vector<Encontrado> res;
    for (auto& d : docs) {
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
        if (p > 0) res.push_back({p, d.id, d.texto, d.tipo});
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

void Memoria::guardar_estadisticas() { escribir_atomico(dir_ / "estadisticas.json", stats_.dump(1)); }

}  // namespace orion
