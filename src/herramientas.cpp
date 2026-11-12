#include "orion/herramientas.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "orion/sistema.hpp"

namespace orion {

namespace {
const size_t MAX_RESULTADO = 3500;   // lo que vuelve al modelo (su contexto es limitado)

json funcion(const std::string& nombre, const std::string& desc, const json& props, const std::vector<std::string>& req) {
    return {{"type", "function"},
            {"function", {{"name", nombre}, {"description", desc}, {"parameters", {{"type", "object"}, {"properties", props}, {"required", req}}}}}};
}
json texto(const std::string& d) { return {{"type", "string"}, {"description", d}}; }
json entero(const std::string& d) { return {{"type", "integer"}, {"description", d}}; }

std::string arg_str(const json& a, const char* k, const std::string& def = "") {
    if (!a.is_object() || !a.contains(k)) return def;
    const json& v = a[k];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return def;
    return v.dump();
}
long long arg_int(const json& a, const char* k, long long def) {
    if (!a.is_object() || !a.contains(k)) return def;
    const json& v = a[k];
    if (v.is_number()) return v.get<long long>();
    if (v.is_string()) { try { return std::stoll(v.get<std::string>()); } catch (...) {} }
    return def;
}
bool arg_bool(const json& a, const char* k) {
    if (!a.is_object() || !a.contains(k)) return false;
    const json& v = a[k];
    return v.is_boolean() ? v.get<bool>() : (v.is_string() && (v == "true" || v == "si" || v == "sí"));
}

std::string ejecutable(const std::string& nombre) {
#ifdef _WIN32
    return nombre + ".exe";
#else
    return nombre;
#endif
}
}  // namespace

Herramientas::Herramientas(ContextoHerramientas c) : c_(std::move(c)) {}

std::vector<std::string> Herramientas::nombres(const std::string& permisos) {
    std::vector<std::string> memoria = {"guardar_recuerdo", "buscar_en_memoria", "crear_meta_de_aprendizaje"};
    std::vector<std::string> lectura = {"leer_archivo", "listar_directorio", "buscar_en_internet", "leer_pagina_web"};
    std::vector<std::string> total = {"ejecutar_comando", "escribir_archivo", "guardar_habilidad", "usar_habilidad", "compilar_nueva_version", "reiniciarme"};
    std::vector<std::string> r = memoria;
    if (permisos == "lectura" || permisos == "total") r.insert(r.end(), lectura.begin(), lectura.end());
    if (permisos == "total") r.insert(r.end(), total.begin(), total.end());
    return r;
}

json Herramientas::definiciones() const {
    std::vector<json> todas = {
        funcion("ejecutar_comando",
#ifdef _WIN32
                "Ejecuta una orden en la consola de Windows (cmd.exe; para PowerShell usa: powershell -NoProfile -Command \"...\"). "
#else
                "Ejecuta una orden en la shell (/bin/sh). "
#endif
                "Devuelve el código de salida y la salida. Tienes acceso total al ordenador.",
                {{"comando", texto("la orden completa")}, {"directorio", texto("carpeta donde ejecutarla (opcional)")},
                 {"segundos", entero("tiempo máximo, por defecto 120")}},
                {"comando"}),
        funcion("leer_archivo", "Lee un archivo de texto (cualquier ruta, también tu propio código y tu memoria).",
                {{"ruta", texto("ruta del archivo")}, {"desde_linea", entero("primera línea, por defecto 1")}, {"lineas", entero("cuántas, por defecto 200")}},
                {"ruta"}),
        funcion("escribir_archivo", "Escribe (o añade al final de) un archivo de texto. Crea las carpetas que falten.",
                {{"ruta", texto("ruta del archivo")}, {"contenido", texto("texto a escribir")}, {"anadir", {{"type", "boolean"}, {"description", "true para añadir al final"}}}},
                {"ruta", "contenido"}),
        funcion("listar_directorio", "Lista el contenido de una carpeta.", {{"ruta", texto("carpeta")}}, {"ruta"}),
        funcion("buscar_en_internet", "Busca en internet (DuckDuckGo) y devuelve títulos, resúmenes y direcciones.", {{"consulta", texto("qué buscar")}}, {"consulta"}),
        funcion("leer_pagina_web", "Descarga una página web y devuelve su texto.", {{"url", texto("dirección http(s)")}}, {"url"}),
        funcion("guardar_recuerdo", "Guarda un hecho duradero en tu memoria.",
                {{"texto", texto("el hecho, en una frase autocontenida")}, {"tipo", texto("usuario | proyecto | preferencia | yo | mundo | aprendido")}}, {"texto"}),
        funcion("buscar_en_memoria", "Busca en tus recuerdos y conversaciones pasadas.", {{"consulta", texto("qué buscar")}}, {"consulta"}),
        funcion("crear_meta_de_aprendizaje",
                "Crea una meta para aprender algo que no sabes. Trabajarás en ella a solas (con tus herramientas) cuando no te hablen, y avisarás al terminar.",
                {{"tema", texto("qué vas a aprender")}, {"motivo", texto("por qué / quién lo pidió")}}, {"tema"}),
        funcion("guardar_habilidad",
                "Guarda como habilidad reutilizable un script que YA te ha funcionado (se prueba antes de guardarlo). Úsalo cuando resuelvas algo "
                "que te pueden volver a pedir.",
                {{"nombre", texto("nombre corto en minúsculas con _ (p. ej. resumir_pdf)")}, {"descripcion", texto("qué hace y qué argumentos recibe")},
                 {"lenguaje", texto(Habilidades::lenguaje_por_defecto() == "powershell" ? "powershell (por defecto) o cmd" : "sh")},
                 {"codigo", texto("el script completo")}, {"prueba", texto("argumentos para probarlo (puede ir vacío)")}},
                {"nombre", "descripcion", "codigo"}),
        funcion("usar_habilidad", "Ejecuta una de tus habilidades guardadas.",
                {{"nombre", texto("nombre de la habilidad")}, {"argumentos", texto("argumentos (opcional)")}}, {"nombre"}),
        funcion("compilar_nueva_version",
                "Compila tu código fuente actual (después de modificarlo) en una versión nueva, sin pararte. Si compila, queda lista para aplicarse con reiniciarme.",
                json::object(), {}),
        funcion("reiniciarme", "Te reinicia al terminar este turno. Si hay una versión nueva compilada, el guardián la aplica (y vuelve a la anterior si falla).",
                json::object(), {}),
    };
    auto permitidas = nombres(c_.permisos);
    json r = json::array();
    for (auto& f : todas)
        if (std::find(permitidas.begin(), permitidas.end(), f["function"]["name"].get<std::string>()) != permitidas.end()) r.push_back(f);
    return r;
}

std::string Herramientas::usar(const std::string& nombre, const json& argumentos) {
    json a = argumentos;
    if (a.is_string()) {   // algunos modelos mandan los argumentos como texto JSON
        json p = json::parse(a.get<std::string>(), nullptr, false);
        a = p.is_object() ? p : json::object();
    }
    if (!a.is_object()) a = json::object();
    std::string resumen_args = recortar(a.dump(), 300);
    if (c_.aviso) {
        std::string vista = a.contains("comando") ? arg_str(a, "comando") : a.contains("ruta") ? arg_str(a, "ruta")
                            : a.contains("url") ? arg_str(a, "url") : a.contains("consulta") ? arg_str(a, "consulta")
                            : a.contains("tema") ? arg_str(a, "tema") : a.contains("texto") ? arg_str(a, "texto") : "";
        c_.aviso("⚙ " + nombre + (vista.empty() ? "" : ": " + recortar(vista, 160)));
    }
    std::string r;
    auto permitidas = nombres(c_.permisos);
    if (std::find(permitidas.begin(), permitidas.end(), nombre) == permitidas.end()) {
        r = "No tengo permiso para '" + nombre + "' (permisos actuales: " + c_.permisos + "). Mi creador puede cambiarlo con /permisos.";
    } else {
        try {
            r = usar_sin_registro(nombre, a);
        } catch (const std::exception& e) {
            r = std::string("Error: ") + e.what();
        }
    }
    r = recortar(r, MAX_RESULTADO);
    if (c_.aviso) {
        auto lineas = split_lines(r);
        std::string vista = lineas.empty() ? "(sin salida)" : recortar(lineas[0], 160) + (lineas.size() > 1 ? "  (+" + std::to_string(lineas.size() - 1) + " líneas)" : "");
        c_.aviso("  → " + vista);
    }
    try {
        anadir_linea(c_.datos / "acciones.jsonl",
                     json{{"t", ahora_s()}, {"fecha", fecha_hora()}, {"origen", c_.origen}, {"herramienta", nombre}, {"argumentos", a}, {"resultado", recortar(r, 1500)}}
                         .dump());
    } catch (...) {
    }
    return r;
}

std::string Herramientas::usar_sin_registro(const std::string& nombre, const json& a) {
    if (nombre == "ejecutar_comando") {
        std::string cmd = arg_str(a, "comando");
        if (trim(cmd).empty()) return "Falta 'comando'.";
        int seg = static_cast<int>(std::clamp<long long>(arg_int(a, "segundos", 120), 1, 3600));
        auto res = ejecutar_shell(cmd, arg_str(a, "directorio"), seg);
        std::string s = "código de salida: " + std::to_string(res.codigo) + (res.tiempo_agotado ? " (tiempo agotado, se detuvo)" : "") + "\n";
        std::string sal = res.salida;
        if (sal.size() > MAX_RESULTADO - 200) sal = sal.substr(0, (MAX_RESULTADO - 200) / 2) + "\n[…]\n" + sal.substr(sal.size() - (MAX_RESULTADO - 200) / 2);
        return s + (trim(sal).empty() ? "(sin salida)" : sal);
    }
    if (nombre == "leer_archivo") {
        fs::path p = arg_str(a, "ruta");
        std::error_code ec;
        if (!fs::exists(p, ec)) return "No existe: " + p.string();
        if (fs::is_directory(p, ec)) return "Es una carpeta; usa listar_directorio.";
        auto lineas = split_lines(leer_archivo(p));
        long long desde = std::max<long long>(1, arg_int(a, "desde_linea", 1)), n = std::clamp<long long>(arg_int(a, "lineas", 200), 1, 2000);
        std::string s = p.string() + " (" + std::to_string(lineas.size()) + " líneas)\n";
        for (long long i = desde - 1; i < static_cast<long long>(lineas.size()) && i < desde - 1 + n; ++i)
            s += std::to_string(i + 1) + "| " + lineas[static_cast<size_t>(i)] + "\n";
        return s;
    }
    if (nombre == "escribir_archivo") {
        fs::path p = arg_str(a, "ruta");
        if (p.empty()) return "Falta 'ruta'.";
        std::string contenido = arg_str(a, "contenido");
        if (p.has_parent_path()) fs::create_directories(p.parent_path());
        if (arg_bool(a, "anadir")) {
            std::ofstream f(p, std::ios::binary | std::ios::app);
            f << contenido;
        } else {
            escribir_atomico(p, contenido);
        }
        return "Escrito " + p.string() + " (" + std::to_string(contenido.size()) + " bytes).";
    }
    if (nombre == "listar_directorio") {
        fs::path p = arg_str(a, "ruta", ".");
        std::error_code ec;
        if (!fs::is_directory(p, ec)) return "No es una carpeta: " + p.string();
        std::vector<std::string> v;
        for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            bool dir = it->is_directory(e2);
            v.push_back(it->path().filename().string() + (dir ? "/" : "  (" + formato_bytes(static_cast<double>(it->file_size(e2))) + ")"));
            if (v.size() >= 300) break;
        }
        std::sort(v.begin(), v.end());
        return fs::absolute(p).string() + ":\n" + join(v, "\n");
    }
    if (nombre == "buscar_en_internet") {
        std::string q = arg_str(a, "consulta");
        auto res = ejecutar_proceso({ejecutable("curl"), "-sSL", "--max-time", "25", "-A", "Mozilla/5.0 (Orion)", "https://html.duckduckgo.com/html/?q=" + codificar_url(q)}, {}, 40);
        if (res.codigo != 0) return "No pude buscar (¿hay conexión? ¿está curl instalado?): " + recortar(res.salida, 300);
        auto rs = resultados_busqueda(res.salida);
        if (rs.empty()) return "Sin resultados (o la página de búsqueda cambió). Texto recibido: " + recortar(html_a_texto(res.salida), 800);
        std::string s;
        for (size_t i = 0; i < rs.size() && i < 6; ++i) s += std::to_string(i + 1) + ". " + rs[i].second + "\n   " + rs[i].first + "\n";
        return s;
    }
    if (nombre == "leer_pagina_web") {
        std::string url = arg_str(a, "url");
        if (!empieza_por(url, "http://") && !empieza_por(url, "https://")) return "La url debe empezar por http:// o https://";
        auto res = ejecutar_proceso({ejecutable("curl"), "-sSL", "--max-time", "30", "--max-filesize", "8000000", "-A", "Mozilla/5.0 (Orion)", url}, {}, 45);
        if (res.codigo != 0) return "No pude descargarla (código " + std::to_string(res.codigo) + "): " + recortar(res.salida, 300);
        return html_a_texto(res.salida);
    }
    if (nombre == "guardar_recuerdo") {
        if (!c_.memoria) return "Sin memoria.";
        std::string id = c_.memoria->recordar(arg_str(a, "tipo", "mundo"), arg_str(a, "texto"), c_.origen);
        return id.empty() ? "Ya lo recordaba." : "Guardado como " + id + ".";
    }
    if (nombre == "buscar_en_memoria") {
        if (!c_.memoria) return "Sin memoria.";
        std::string q = arg_str(a, "consulta");
        std::vector<float> vq = c_.vectorizar ? c_.vectorizar(q) : std::vector<float>{};
        auto rs = c_.memoria->buscar(q, 8, true, vq.empty() ? nullptr : &vq, c_.modelo);
        if (rs.empty()) return "No encuentro nada.";
        std::string s;
        for (auto& e : rs) s += "[" + e.id + "] " + recortar(e.texto, 300) + "\n";
        return s;
    }
    if (nombre == "crear_meta_de_aprendizaje") {
        if (!c_.memoria) return "Sin memoria.";
        std::string tema = arg_str(a, "tema");
        if (trim(tema).empty()) return "Falta 'tema'.";
        std::string id = c_.memoria->crear_meta(tema, arg_str(a, "motivo"));
        return "Meta " + id + " creada: '" + tema + "'. Trabajaré en ella a solas y avisaré al terminar.";
    }
    if (nombre == "guardar_habilidad") {
        if (!c_.habilidades) return "Sin biblioteca de habilidades.";
        Habilidad h;
        h.nombre = arg_str(a, "nombre");
        h.descripcion = arg_str(a, "descripcion");
        h.lenguaje = arg_str(a, "lenguaje", Habilidades::lenguaje_por_defecto());
        h.codigo = arg_str(a, "codigo");
        h.prueba = arg_str(a, "prueba");
        return c_.habilidades->guardar(h);
    }
    if (nombre == "usar_habilidad") {
        if (!c_.habilidades) return "Sin biblioteca de habilidades.";
        return c_.habilidades->usar(arg_str(a, "nombre"), arg_str(a, "argumentos"));
    }
    if (nombre == "compilar_nueva_version") {
        if (c_.repo.empty() || !fs::exists(c_.repo / "CMakeLists.txt")) return "No encuentro mi código fuente (arranca Orion desde la carpeta del repositorio o usa --repo).";
        fs::path b = c_.repo / "build-auto";   // carpeta aparte: en Windows no se puede sobrescribir el .exe en uso
        auto conf = ejecutar_proceso({c_.cmake, "-S", c_.repo.string(), "-B", b.string(), "-DORION_TESTS=OFF"}, {}, 600);
        if (conf.codigo == -1 || conf.codigo == 127 || (conf.codigo != 0 && trim(conf.salida).empty()))
            return "No encuentro cmake ('" + c_.cmake + "'). Instálalo y añádelo al PATH, o pon su ruta en datos/config.json (\"cmake\").";
        if (conf.codigo != 0) return "Falló la configuración de CMake:\n" + recortar(conf.salida, 2500);
        auto comp = ejecutar_proceso({c_.cmake, "--build", b.string(), "--config", "Release", "--target", "orion"}, {}, 1800);
        if (comp.codigo != 0) {
            std::string sal = comp.salida;
            return "No compila. Errores:\n" + (sal.size() > 3000 ? sal.substr(sal.size() - 3000) : sal);
        }
        fs::path exe = ejecutable_compilado(b);
        if (exe.empty()) return "Compiló pero no encuentro el ejecutable en " + b.string();
        auto prueba = ejecutar_proceso({exe.string(), "--version"}, {}, 30);
        if (prueba.codigo != 0 || !contiene(prueba.salida, "orion")) return "La versión nueva no arranca bien (--version): " + recortar(prueba.salida, 500);
        fs::path destino = c_.datos / "actualizacion" / exe.filename();
        fs::create_directories(destino.parent_path());
        fs::copy_file(exe, destino, fs::copy_options::overwrite_existing);
        return "Versión nueva compilada y probada (" + trim(prueba.salida) + "). Lista en " + destino.string() +
               ". Usa reiniciarme para aplicarla; si falla al arrancar, el guardián vuelve a la versión anterior.";
    }
    if (nombre == "reiniciarme") {
        if (c_.reinicio_pedido) *c_.reinicio_pedido = true;
        return "Me reiniciaré al terminar este turno.";
    }
    return "Herramienta desconocida: " + nombre;
}

fs::path Herramientas::ejecutable_compilado(const fs::path& b) {
    for (auto& p : {b / "Release" / ejecutable("orion"), b / ejecutable("orion"), b / "RelWithDebInfo" / ejecutable("orion")}) {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
    return {};
}

// ------------------------------------------------------------------ web
static std::string decodificar_entidades(std::string s) {
    static const std::pair<const char*, const char*> ent[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"},
                                                              {"&#x27;", "'"}, {"&nbsp;", " "}, {"&apos;", "'"}, {"&#x2F;", "/"}};
    for (auto& [e, c] : ent)
        for (size_t p = s.find(e); p != std::string::npos; p = s.find(e, p + 1)) s.replace(p, std::char_traits<char>::length(e), c);
    return s;
}

static std::string minus_ascii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string Herramientas::html_a_texto(const std::string& html) {
    std::string h = html, bajo = minus_ascii(html), out;
    // quitar bloques que no son texto
    for (const char* etiqueta : {"script", "style", "noscript", "svg", "head"}) {
        std::string ab = std::string("<") + etiqueta, ce = std::string("</") + etiqueta + ">";
        for (size_t p = bajo.find(ab); p != std::string::npos; p = bajo.find(ab, p)) {
            size_t f = bajo.find(ce, p);
            size_t fin = f == std::string::npos ? bajo.size() : f + ce.size();
            h.erase(p, fin - p);
            bajo.erase(p, fin - p);
        }
    }
    bool dentro = false;
    for (size_t i = 0; i < h.size(); ++i) {
        char c = h[i];
        if (c == '<') {
            dentro = true;
            std::string t = bajo.substr(i, 4);
            if (t.rfind("<p>", 0) == 0 || t.rfind("<p ", 0) == 0 || t.rfind("<br", 0) == 0 || t.rfind("<li", 0) == 0 || t.rfind("<h", 0) == 0 ||
                t.rfind("<tr", 0) == 0 || t.rfind("<div", 0) == 0)
                out += '\n';
            continue;
        }
        if (c == '>' && dentro) { dentro = false; out += ' '; continue; }
        if (!dentro) out += c;
    }
    out = decodificar_entidades(out);
    std::string limpio;   // espacios y líneas en blanco repetidos
    int saltos = 0;
    bool espacio = false;
    for (char c : out) {
        if (c == '\r' || c == '\t') c = ' ';
        if (c == '\n') { if (++saltos <= 2) limpio += '\n'; espacio = false; continue; }
        if (c == ' ') { if (!espacio && saltos == 0) limpio += ' '; espacio = true; continue; }
        saltos = 0;
        espacio = false;
        limpio += c;
    }
    std::vector<std::string> lineas;
    for (auto& l : split_lines(limpio)) lineas.push_back(trim(l));
    return recortar(trim(join(lineas, "\n")), 12000);
}

std::string Herramientas::codificar_url(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += static_cast<char>(c);
        else if (c == ' ') r += '+';
        else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
    }
    return r;
}

std::string Herramientas::decodificar_url(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) && isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            r += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            r += s[i] == '+' ? ' ' : s[i];
        }
    }
    return r;
}

std::vector<std::pair<std::string, std::string>> Herramientas::resultados_busqueda(const std::string& html) {
    std::vector<std::pair<std::string, std::string>> v;
    size_t p = 0;
    while ((p = html.find("result__a", p)) != std::string::npos) {
        size_t a_ini = html.rfind("<a", p);
        size_t a_fin = html.find('>', p);
        size_t cierre = html.find("</a>", a_fin == std::string::npos ? p : a_fin);
        if (a_ini == std::string::npos || a_fin == std::string::npos || cierre == std::string::npos) break;
        std::string etiqueta = html.substr(a_ini, a_fin - a_ini);
        std::string url;
        size_t h = etiqueta.find("href=\"");
        if (h != std::string::npos) url = decodificar_entidades(etiqueta.substr(h + 6, etiqueta.find('"', h + 6) - h - 6));
        size_t u = url.find("uddg=");
        if (u != std::string::npos) url = decodificar_url(url.substr(u + 5, url.find('&', u) == std::string::npos ? std::string::npos : url.find('&', u) - u - 5));
        if (empieza_por(url, "//")) url = "https:" + url;
        std::string titulo = trim(html_a_texto(html.substr(a_fin + 1, cierre - a_fin - 1)));
        std::string resumen;
        size_t sn = html.find("result__snippet", cierre);
        size_t siguiente = html.find("result__a", cierre);
        if (sn != std::string::npos && (siguiente == std::string::npos || sn < siguiente)) {
            size_t s_ini = html.find('>', sn), s_fin = html.find("</", s_ini == std::string::npos ? sn : s_ini);
            while (s_fin != std::string::npos && html.compare(s_fin, 4, "</b>") == 0) s_fin = html.find("</", s_fin + 4);
            if (s_ini != std::string::npos && s_fin != std::string::npos) resumen = trim(html_a_texto(html.substr(s_ini + 1, s_fin - s_ini - 1)));
        }
        if (!url.empty() && !titulo.empty() && !contiene(url, "duckduckgo.com/y.js")) v.push_back({url, titulo + (resumen.empty() ? "" : " — " + resumen)});
        p = cierre;
    }
    return v;
}

}  // namespace orion
