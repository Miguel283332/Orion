#include "orion/habilidades.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace orion {

json Habilidad::a_json() const {
    return {{"nombre", nombre}, {"descripcion", descripcion}, {"lenguaje", lenguaje}, {"codigo", codigo}, {"prueba", prueba},
            {"usos", usos}, {"exitos", exitos}, {"creada", creada}};
}

Habilidad Habilidad::de_json(const json& j) {
    Habilidad h;
    h.nombre = j.value("nombre", std::string());
    h.descripcion = j.value("descripcion", std::string());
    h.lenguaje = j.value("lenguaje", std::string());
    h.codigo = j.value("codigo", std::string());
    h.prueba = j.value("prueba", std::string());
    h.usos = j.value("usos", 0);
    h.exitos = j.value("exitos", 0);
    h.creada = j.value("creada", 0.0);
    return h;
}

Habilidades::Habilidades(fs::path dir) : dir_(std::move(dir)) {}

std::string Habilidades::lenguaje_por_defecto() {
#ifdef _WIN32
    return "powershell";
#else
    return "sh";
#endif
}

bool Habilidades::nombre_valido(const std::string& n) {
    if (n.empty() || n.size() > 60) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

fs::path Habilidades::archivo_script(const Habilidad& h) const {
    std::string ext = h.lenguaje == "powershell" ? ".ps1" : h.lenguaje == "cmd" ? ".cmd" : ".sh";
    return dir_ / (h.nombre + ext);
}

ResultadoProceso Habilidades::ejecutar(const Habilidad& h, const std::string& argumentos, int timeout_s) const {
    fs::path script = archivo_script(h);
    std::string codigo = h.codigo;
#ifdef _WIN32
    if (h.lenguaje == "powershell") codigo = "$OutputEncoding = [Console]::OutputEncoding = [Text.Encoding]::UTF8\r\n" + codigo;
#endif
    escribir_atomico(script, codigo);
    std::string ruta = fs::absolute(script).string();
    std::string cmd;
    if (h.lenguaje == "powershell") cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + ruta + "\"";
    else if (h.lenguaje == "cmd") cmd = "\"" + ruta + "\"";
    else cmd = "sh \"" + ruta + "\"";
    if (!trim(argumentos).empty()) cmd += " " + argumentos;
    return ejecutar_shell(cmd, dir_, timeout_s);
}

std::string Habilidades::guardar(const Habilidad& h0, bool probar) {
    Habilidad h = h0;
    h.nombre = minusculas_sin_acentos(trim(h.nombre));
    for (auto& c : h.nombre) if (c == ' ' || c == '-') c = '_';
    if (!nombre_valido(h.nombre)) return "Nombre no válido: usa minúsculas, números y _ (por ejemplo contar_lineas).";
    if (trim(h.codigo).empty()) return "Falta el código.";
    if (h.lenguaje.empty()) h.lenguaje = lenguaje_por_defecto();
    if (h.lenguaje != "powershell" && h.lenguaje != "cmd" && h.lenguaje != "sh") return "Lenguaje no válido: powershell, cmd o sh.";
    fs::create_directories(dir_);
    std::string salida;
    if (probar) {
        auto r = ejecutar(h, h.prueba, 60);
        if (r.codigo != 0 || r.tiempo_agotado) {
            std::error_code ec;
            fs::remove(archivo_script(h), ec);
            return "No la guardo: la prueba falló (código " + std::to_string(r.codigo) + "). Salida:\n" + recortar(r.salida, 1500) +
                   "\nCorrígela y vuelve a intentarlo.";
        }
        salida = r.salida;
    }
    Habilidad previa;
    if (obtener(h.nombre, previa)) { h.usos = previa.usos; h.exitos = previa.exitos; h.creada = previa.creada; }
    if (h.creada == 0) h.creada = ahora_s();
    escribir_atomico(dir_ / (h.nombre + ".json"), h.a_json().dump(2));
    return "Habilidad '" + h.nombre + "' guardada" + (probar ? " (la prueba funcionó; salida: " + recortar(trim(salida), 300) + ")" : "") + ".";
}

std::vector<Habilidad> Habilidades::todas() const {
    std::vector<Habilidad> v;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return v;
    for (auto it = fs::directory_iterator(dir_, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        json j = json::parse(leer_archivo(it->path()), nullptr, false);
        if (j.is_object()) v.push_back(Habilidad::de_json(j));
    }
    std::sort(v.begin(), v.end(), [](const Habilidad& a, const Habilidad& b) { return a.nombre < b.nombre; });
    return v;
}

bool Habilidades::obtener(const std::string& nombre, Habilidad& h) const {
    if (!nombre_valido(nombre)) return false;
    json j = json::parse(leer_archivo(dir_ / (nombre + ".json")), nullptr, false);
    if (!j.is_object()) return false;
    h = Habilidad::de_json(j);
    return true;
}

std::vector<Habilidad> Habilidades::relevantes(const std::string& consulta, size_t k) const {
    auto q = palabras(consulta);
    std::set<std::string> qs(q.begin(), q.end());
    std::vector<std::pair<double, Habilidad>> v;
    for (auto& h : todas()) {
        auto ws = palabras(h.nombre + " " + h.descripcion);
        for (auto& w : palabras(h.nombre)) for (auto& p : split(w, '_')) ws.push_back(p);
        std::set<std::string> hs(ws.begin(), ws.end());
        double comun = 0;
        for (auto& w : qs) if (w.size() > 2 && hs.count(w)) comun += 1;
        double fiabilidad = h.usos > 0 ? static_cast<double>(h.exitos) / h.usos : 0.5;
        if (comun > 0) v.push_back({comun + 0.3 * fiabilidad, h});
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Habilidad> r;
    for (size_t i = 0; i < v.size() && i < k; ++i) r.push_back(v[i].second);
    return r;
}

std::string Habilidades::usar(const std::string& nombre, const std::string& argumentos, int timeout_s) {
    Habilidad h;
    if (!obtener(nombre, h)) return "No tengo ninguna habilidad llamada '" + nombre + "'.";
    auto r = ejecutar(h, argumentos, timeout_s);
    h.usos++;
    if (r.codigo == 0 && !r.tiempo_agotado) h.exitos++;
    escribir_atomico(dir_ / (h.nombre + ".json"), h.a_json().dump(2));
    return "código de salida: " + std::to_string(r.codigo) + (r.tiempo_agotado ? " (tiempo agotado)" : "") + "\n" +
           (trim(r.salida).empty() ? "(sin salida)" : recortar(r.salida, 3000));
}

bool Habilidades::borrar(const std::string& nombre) {
    Habilidad h;
    if (!obtener(nombre, h)) return false;
    std::error_code ec;
    fs::remove(dir_ / (nombre + ".json"), ec);
    fs::remove(archivo_script(h), ec);
    return true;
}

}  // namespace orion
