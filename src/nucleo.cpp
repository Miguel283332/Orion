#include "orion/nucleo.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>

namespace orion {

namespace {
const char* TENUE = "\x1b[2m";
const char* CIAN = "\x1b[36m";
const char* NORMAL = "\x1b[0m";

int ritmo_a_int(const std::string& r) { return r == "bajo" ? 0 : r == "alto" ? 2 : 1; }
const char* ritmo_nombre(int r) { return r == 0 ? "bajo" : r == 2 ? "alto" : "medio"; }

const char* AYUDA = R"(Habla escribiendo sin barra. Comandos:
  /estado               qué hace ahora, tiempo encendida, llamadas, tokens, velocidad
  /modelo               el modelo: parámetros, cuantización, contexto, memoria que ocupa, tokens/s
  /modelo usar <nombre> cambiar de modelo (p. ej. qwen2.5:7b cuando tengas la RTX)
  /hardware             CPU, RAM, disco y memoria que usa Orion
  /memoria              cuánto recuerda y cuánto ocupa
  /recuerdos [n]        últimos recuerdos (con su id)
  /recuerda <texto>     guardar un recuerdo a mano
  /olvida <id>          borrar un recuerdo
  /buscar <texto>       buscar en recuerdos y conversaciones
  /diario [n]           últimas n entradas de su diario
  /yo                   cómo se ve a sí misma (yo.md)
  /metas                lo que está aprendiendo (metas "voy a aprender")
  /aprende <tema>       ponerle una meta de aprendizaje
  /acciones [n]         últimas acciones con sus herramientas (comandos, archivos, internet)
  /permisos [total|lectura|ninguno]  qué herramientas puede usar (por defecto: total)
  /habilidades          lo que ya sabe hacer (scripts verificados que reutiliza)
  /practica [ahora]     su práctica con juez real: nivel, aciertos, últimos ejercicios
  /dispositivos         otros ordenadores tuyos con Ollama: añadir, usar, medir, auto (se pasa al más rápido)
  /limites              ¿me limita el hardware? (velocidad, RAM, contexto)
  /web [abrir]          dirección de la interfaz web local (chat, actividad, metas, mejoras...)
  /mejoras              historial de automejora: cada cambio, su medición y si se aceptó
  /mejorate             hacer ahora un ciclo de automejora medida
  /volver <n>           deshacer la mejora número n
  /reflexiona           que reflexione ahora sobre la conversación
  /pausa · /sigue       detener o reanudar su pensamiento a solas
  /ritmo [bajo|medio|alto]  cada cuánto piensa a solas
  /para                 cortar la respuesta que está escribiendo
  /versiones            historial de versiones de su código (git)
  /config               configuración actual
  /reinicia             reiniciar (con el guardián, vuelve a arrancar sola)
  /salir                apagar guardando todo)";
}  // namespace

Nucleo::Nucleo(Config cfg, OpcionesNucleo op, Salida salida)
    : cfg_(std::move(cfg)), op_(std::move(op)), salida_(std::move(salida)), mem_(op_.datos), llm_(cfg_.ollama, cfg_.modelo), habilidades_(op_.datos / "habilidades"),
      mente_(cfg_, mem_, llm_), automejora_(ContextoAutomejora{op_.repo, op_.datos, op_.ejecutable}, llm_) {
    mente_.fijar_habilidades(&habilidades_);
    mente_.fijar_dir_prompts(op_.prompts);
    Salida original = salida_;   // todo lo que sale por la terminal también va a la interfaz web
    salida_ = [this, original](const std::string& t) {
        if (original) original(t);
        registrar_evento(t);
    };
    ritmo_ = ritmo_a_int(cfg_.ritmo);
}

Nucleo::~Nucleo() { parar(); }

void Nucleo::decir(const std::string& t) {
    std::lock_guard<std::mutex> l(mu_salida_);
    if (linea_abierta_) { salida_("\n"); linea_abierta_ = false; prefijo_pendiente_ = true; }
    salida_(t + "\n");
}

void Nucleo::nota(const std::string& t) {
    std::lock_guard<std::mutex> l(mu_salida_);
    if (linea_abierta_) { salida_("\n"); linea_abierta_ = false; prefijo_pendiente_ = true; }
    salida_(op_.color ? std::string(TENUE) + "  · " + t + NORMAL + "\n" : "  · " + t + "\n");
}

void Nucleo::fijar_actividad(const std::string& a) {
    std::lock_guard<std::mutex> l(mu_act_);
    actividad_ = a;
}

std::string Nucleo::actividad() const {
    std::lock_guard<std::mutex> l(mu_act_);
    return actividad_;
}

double Nucleo::intervalo_ritmo() const {
    static const double seg[] = {900, 120, 20};
    return seg[ritmo_.load()];
}

void Nucleo::arrancar() {
    mem_.cargar();
    inicio_ = ultima_interaccion_ = ahora_s();
    ultima_tarea_ = 0;
    mem_.sumar("arranques", 1);
    if (cfg_.dispositivo != "local") {   // seguir en el dispositivo donde estaba (si ya no responde, la revisión vuelve al local)
        json d;
        if (dispositivo(cfg_.dispositivo, d)) {
            llm_.cambiar_url(d.value("ollama", cfg_.ollama));
            if (d.contains("modelo")) llm_.cambiar_modelo(d["modelo"].get<std::string>());
        } else {
            cfg_.dispositivo = "local";
        }
    }
    std::string version;
    bool hay_ollama = llm_.disponible(&version);
    std::string cab = op_.color ? std::string(CIAN) + "Orion" + NORMAL : "Orion";
    decir(cab + " · " + std::to_string(mem_.recuerdos().size()) + " recuerdos, " + std::to_string(mem_.num_turnos()) + " turnos de conversación, " +
          std::to_string(mem_.entradas_diario()) + " entradas de diario · datos en " + fs::absolute(op_.datos).string());
    if (!hay_ollama) {
        decir("⚠ No encuentro Ollama en " + cfg_.ollama + ". Abre la aplicación Ollama (o ejecuta `ollama serve`). "
              "Los comandos funcionan; para hablar necesito el modelo.");
    } else {
        auto info = llm_.info();
        modelo_cache_ = info;
        modelo_cache_t_ = ahora_s();
        if (!info.ok) decir("⚠ " + info.error);
        else
            decir("Modelo " + cfg_.modelo + " (" + (info.parametros > 0 ? formato_numero(info.parametros) + " de parámetros" : info.tamano_parametros) +
                  (info.cuantizacion.empty() ? "" : ", " + info.cuantizacion) + ") · Ollama " + version);
    }
    std::error_code ec;
    if (fs::exists(op_.datos / "actualizacion_fallida.txt", ec)) {
        std::string aviso = trim(leer_archivo(op_.datos / "actualizacion_fallida.txt"));
        decir("⚠ La última versión que compilé de mí misma falló al arrancar; el guardián volvió a la anterior.");
        mem_.anadir_turno("orion", "[Aviso del guardián] La versión nueva que compilé falló al arrancar y se volvió a la anterior: " + aviso);
        fs::remove(op_.datos / "actualizacion_fallida.txt", ec);
    }
    mente_.fijar_cuerpo(describir_cuerpo());
    decir("Permisos: " + cfg_.permisos + (cfg_.permisos == "total" ? " (comandos, archivos, internet, su código y su memoria; todo queda en datos/acciones.jsonl)" : ""));
    iniciar_web();
    decir("Escribe para hablar conmigo. /ayuda para ver los comandos.");
    hilo_ = std::thread([this] { bucle_mente(); });
}

bool Nucleo::entrada(const std::string& linea_cruda) {
    std::string linea = trim(linea_cruda);
    if (linea.empty()) return true;
    if (linea[0] == '/') {
        std::string cmd = split(linea, ' ')[0];
        if (cmd == "/salir") return false;
        if (cmd == "/reinicia") { codigo_salida_ = CODIGO_REINICIO; return false; }
        if (cmd == "/para") {
            cancelar_chat_ = true;
            cancelar_fondo_ = true;
            return true;
        }
        decir(comando(linea));
        return true;
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        cola_.push_back(linea);
    }
    cancelar_fondo_ = true;   // lo que estuviera pensando a solas puede esperar
    cv_.notify_all();
    return true;
}

int Nucleo::ejecutar(const std::function<bool(std::string&)>& leer) {
    std::string linea;
    bool fin_por_comando = false;
    while (leer(linea)) {
        if (!entrada(linea)) { fin_por_comando = true; break; }
    }
    cancelar_fondo_ = true;
    if (!fin_por_comando || codigo_salida_ == 0) esperar_inactividad(3600);   // terminar lo que le pediste antes de apagarse
    parar();
    decir("Hasta luego. Todo queda guardado en " + fs::absolute(op_.datos).string());
    return codigo_salida_;
}

void Nucleo::esperar_inactividad(double max_s) {
    double fin = ahora_s() + max_s;
    while (ahora_s() < fin) {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (cola_.empty() && !ocupado_) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void Nucleo::parar() {
    if (parado_.exchange(true)) return;
    if (web_) web_->parar();
    salir_ = true;
    cancelar_fondo_ = true;
    cv_.notify_all();
    if (hilo_.joinable() && hilo_.get_id() != std::this_thread::get_id()) hilo_.join();
    if (inicio_ > 0) mem_.sumar("segundos_encendida", ahora_s() - inicio_);
}

// ------------------------------------------------------------------ hilo de mente
void Nucleo::bucle_mente() {
    int errores_seguidos = 0;
    while (!salir_) {
        std::string msg;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait_for(l, std::chrono::milliseconds(300), [&] { return salir_ || !cola_.empty() || reflexion_pedida_ || practica_pedida_ || mejora_pedida_; });
            if (salir_) break;
            if (!cola_.empty()) {
                msg = cola_.front();
                cola_.pop_front();
                ocupado_ = true;
            }
            if (!modelo_pendiente_.empty()) {
                cfg_.modelo = modelo_pendiente_;
                bool con_modelo_propio = false;
                for (auto& x : cfg_.dispositivos)
                    if (x.value("nombre", std::string()) == cfg_.dispositivo && x.contains("modelo")) con_modelo_propio = true;
                if (!con_modelo_propio) llm_.cambiar_modelo(modelo_pendiente_);
                modelo_pendiente_.clear();
                mente_.modelo_cambiado();
                std::lock_guard<std::mutex> li(mu_info_);
                modelo_cache_t_ = 0;
            }
        }
        if (!msg.empty()) {
            cancelar_fondo_ = false;
            atender(msg);
            ultima_interaccion_ = ahora_s();
            ocupado_ = false;
            continue;
        }
        if (ahora_s() - ultima_revision_ >= 600) {   // cada 10 min: dispositivos y límites (no usa el modelo)
            ultima_revision_ = ahora_s();
            try {
                std::string d = revisar_dispositivos();
                if (!d.empty()) decir(d);
                std::string lim = evaluar_limites();
                if (!lim.empty()) decir(lim);
            } catch (const std::exception& e) {
                nota(std::string("no pude revisar mis dispositivos: ") + e.what());
            }
        }
        bool forzar = reflexion_pedida_.exchange(false);
        bool pedida = practica_pedida_.exchange(false);
        bool mejora = mejora_pedida_.exchange(false);
        if (mejora) {
            cancelar_fondo_ = false;
            try { mejorarse(true); } catch (const std::exception& e) { nota(std::string("no pude terminar la automejora: ") + e.what()); }
            fijar_actividad(pausado_ ? "en pausa" : "en reposo");
            ultima_tarea_ = ahora_s();
            continue;
        }
        double ahora = ahora_s();
        double espera = intervalo_ritmo() * (1 << std::min(errores_seguidos, 4));
        if (forzar || pedida || (op_.autonomo && !pausado_ && ahora - ultima_interaccion_ >= op_.gracia_s && ahora - ultima_tarea_ >= espera)) {
            cancelar_fondo_ = false;
            ultima_tarea_ = ahora;
            bool hizo = false;
            try {
                hizo = tarea_a_solas(forzar);
                errores_seguidos = 0;
            } catch (const std::exception& e) {
                ++errores_seguidos;
                nota(std::string("no pude terminar: ") + e.what());
            }
            fijar_actividad(pausado_ ? "en pausa" : "en reposo");
            // si había trabajo (p. ej. el siguiente paso de una meta), sigue sin esperar; con ritmo bajo, respeta el intervalo
            ultima_tarea_ = hizo && ritmo_ != 0 && !cancelar_fondo_ ? 0 : ahora_s();
        }
    }
}

void Nucleo::atender(const std::string& mensaje) {
    {
        std::lock_guard<std::mutex> l(mu_info_);
        fijar_actividad(modelo_cache_.cargado ? "pensando" : "pensando (la primera vez carga el modelo: puede tardar un poco)");
    }
    nota("pensando…");
    cancelar_chat_ = false;
    mente_.fijar_estado_actual(linea_estado_para_modelo());
    {
        std::lock_guard<std::mutex> l(mu_salida_);
        prefijo_pendiente_ = true;
    }
    auto h = herramientas("chat");
    auto r = mente_.responder(mensaje, [&](const std::string& t) {
        std::lock_guard<std::mutex> l(mu_salida_);
        if (prefijo_pendiente_) {
            fijar_actividad("respondiendo");
            std::string limpio = t;
            while (!limpio.empty() && (limpio[0] == '\n' || limpio[0] == ' ')) limpio.erase(0, 1);
            if (limpio.empty()) return;
            salida_(op_.color ? std::string(CIAN) + "Orion> " + NORMAL : "Orion> ");
            prefijo_pendiente_ = false;
            linea_abierta_ = true;
            salida_(limpio);
            return;
        }
        salida_(t);
    }, &cancelar_chat_, &h);
    {
        std::lock_guard<std::mutex> l(mu_salida_);
        if (linea_abierta_) salida_("\n");
        linea_abierta_ = prefijo_pendiente_ = false;
    }
    {
        std::lock_guard<std::mutex> l(mu_info_);
        modelo_cache_t_ = 0;   // refrescar "dónde" (CPU/GPU) y memoria del modelo
    }
    if (!r.error.empty()) decir("⚠ " + r.error);
    else if (r.cancelada) nota("respuesta interrumpida");
    else if (r.cortada) nota("la respuesta se cortó por llegar al máximo de longitud (num_predict = " + std::to_string(cfg_.num_predict) + "): dime «sigue» para que continúe");
    else if (r.tokens_generados > 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%lld tokens · %.1f tokens/s", r.tokens_generados, r.tokens_por_segundo());
        nota(buf);
    }
    fijar_actividad(pausado_ ? "en pausa" : "en reposo");
    quizas_reiniciar();
}

void Nucleo::quizas_reiniciar() {
    if (!reinicio_pedido_) return;
    decir("Me reinicio (si hay una versión nueva compilada, el guardián la aplica)…");
    if (op_.terminar) {
        mem_.sumar("segundos_encendida", ahora_s() - inicio_);
        inicio_ = 0;
        op_.terminar(CODIGO_REINICIO);
        return;
    }
    codigo_salida_ = CODIGO_REINICIO;
    salir_ = true;
}

// ------------------------------------------------------------------ interfaz web
void Nucleo::registrar_evento(const std::string& t) {
    std::string limpio;
    for (size_t i = 0; i < t.size(); ++i) {   // quitar los códigos de color ANSI
        if (t[i] == '\x1b' && i + 1 < t.size() && t[i + 1] == '[') {
            size_t f = t.find('m', i);
            if (f != std::string::npos) { i = f; continue; }
        }
        limpio += t[i];
    }
    std::lock_guard<std::mutex> l(mu_ev_);
    eventos_ += limpio;
    if (eventos_.size() > 400000) {
        eventos_.erase(0, 100000);
        base_eventos_ += 100000;
    }
}

std::string Nucleo::url_web() const {
    return puerto_web_ > 0 ? "http://127.0.0.1:" + std::to_string(puerto_web_) + "/?t=" + token_web_ : "";
}

void Nucleo::iniciar_web() {
    if (cfg_.web_puerto <= 0) return;
    token_web_ = trim(leer_archivo(op_.datos / "web_token.txt"));
    if (token_web_.size() < 32) {
        std::random_device rd;
        std::string semilla = std::to_string(rd()) + std::to_string(rd()) + std::to_string(rd()) + std::to_string(ahora_s()) + op_.datos.string();
        token_web_ = sha256_hex(semilla).substr(0, 40);
        escribir_atomico(op_.datos / "web_token.txt", token_web_ + "\n");
    }
    web_ = std::make_unique<ServidorHttp>([this](const PeticionHttp& p) { return atender_web(p); });
    try {
        puerto_web_ = web_->iniciar(cfg_.web_puerto, "127.0.0.1");
        decir("Interfaz web: " + url_web() + "   (/web abrir)");
    } catch (const std::exception& e) {
        web_.reset();
        decir(std::string("⚠ No pude abrir la interfaz web en el puerto ") + std::to_string(cfg_.web_puerto) + ": " + e.what() +
              " (cambia \"web_puerto\" en config.json)");
    }
}

json Nucleo::estado_json() {
    json s = mem_.estadisticas();
    InfoModelo m = info_modelo(false);
    Config c;
    {
        std::lock_guard<std::mutex> l(mu_);
        c = cfg_;
    }
    std::string donde = !m.cargado ? "—" : m.bytes_en_vram <= 0 ? "CPU" : m.bytes_en_vram >= m.bytes_en_memoria ? "GPU" : "CPU + GPU";
    std::string url = llm_.url();
    if (!contiene(url, "127.0.0.1") && !contiene(url, "localhost")) donde += " (" + url + ")";
    return {{"actividad", actividad()}, {"pausado", pausado_.load()}, {"modelo", c.modelo},
            {"parametros", m.parametros > 0 ? formato_numero(m.parametros) + " de parámetros" : m.tamano_parametros}, {"dispositivo", donde},
            {"tokens_por_segundo", s.value("tokens_por_segundo", 0.0)}, {"recuerdos", mem_.recuerdos().size()}, {"turnos", mem_.num_turnos()},
            {"habilidades", habilidades_.todas().size()}, {"practica_nivel", static_cast<int>(s.value("practica_nivel", 1.0))},
            {"practica_aciertos", std::to_string(static_cast<int>(s.value("practicas_acertadas", 0.0))) + "/" + std::to_string(static_cast<int>(s.value("practicas", 0.0)))},
            {"permisos", c.permisos}, {"ritmo", ritmo_nombre(ritmo_)}, {"encendida", formato_duracion(ahora_s() - inicio_)}};
}

RespuestaServidor Nucleo::atender_web(const PeticionHttp& p) {
    RespuestaServidor r;
    r.cabeceras["Cache-Control"] = "no-store";
    auto error = [&](int estado, const std::string& m) {
        r.estado = estado;
        r.trozos = {json{{"error", m}}.dump()};
        return r;
    };
    // solo desde este ordenador (también protege contra "DNS rebinding")
    std::string host = p.cabecera("host");
    if (!op_.web_cualquier_host && !empieza_por(host, "127.0.0.1:") && !empieza_por(host, "localhost:") && host != "127.0.0.1" && host != "localhost")
        return error(403, "host no permitido");
    std::string ruta = p.ruta, consulta;
    size_t q = ruta.find('?');
    if (q != std::string::npos) { consulta = ruta.substr(q + 1); ruta = ruta.substr(0, q); }
    auto parametro = [&](const std::string& k) -> std::string {
        for (auto& par : split(consulta, '&')) {
            size_t e = par.find('=');
            if (e != std::string::npos && par.substr(0, e) == k) return Herramientas::decodificar_url(par.substr(e + 1));
        }
        return "";
    };
    if (ruta == "/" || ruta == "/index.html") {
        r.tipo = "text/html; charset=utf-8";
        std::string html = op_.repo.empty() ? "" : leer_archivo(op_.repo / "interfaz" / "index.html");   // la puede modificar ella
        if (html.empty())
            html = "<!doctype html><meta charset=utf-8><title>Orion</title><body style='font-family:sans-serif;max-width:700px;margin:40px auto'>"
                   "<h1>Orion</h1><p>No encuentro <code>interfaz/index.html</code> en mi repositorio. Arráncame desde la carpeta del repositorio "
                   "para ver la interfaz completa.</p></body>";
        r.trozos = {html};
        return r;
    }
    if (!empieza_por(ruta, "/api/")) return error(404, "no existe");
    // la API exige el token en una cabecera: una página web ajena no puede enviarla sin permiso del navegador
    if (token_web_.empty() || p.cabecera("x-orion-token") != token_web_) return error(403, "token");
    if (ruta == "/api/estado") { r.trozos = {estado_json().dump()}; return r; }
    if (ruta == "/api/eventos") {
        long long desde = std::atoll(parametro("desde").c_str());
        std::lock_guard<std::mutex> le(mu_ev_);
        if (desde < base_eventos_ || desde > base_eventos_ + static_cast<long long>(eventos_.size())) desde = base_eventos_;
        std::string t = eventos_.substr(static_cast<size_t>(desde - base_eventos_));
        r.trozos = {json{{"texto", t}, {"siguiente", base_eventos_ + static_cast<long long>(eventos_.size())}}.dump(-1, ' ', false, json::error_handler_t::replace)};
        return r;
    }
    if (ruta == "/api/mensaje" && p.metodo == "POST") {
        json j = json::parse(p.cuerpo, nullptr, false);
        std::string texto = j.is_object() ? trim(j.value("texto", std::string())) : "";
        if (texto.empty()) return error(400, "mensaje vacío");
        if (texto[0] == '/') {
            std::string cmd = split(texto, ' ')[0];
            if (cmd == "/salir" || cmd == "/reinicia") { r.trozos = {json{{"respuesta", "Desde la web no me apago ni me reinicio: hazlo en la terminal."}}.dump()}; return r; }
            if (cmd == "/para") { entrada(texto); r.trozos = {json{{"respuesta", "Corto lo que estaba diciendo."}}.dump()}; return r; }
            r.trozos = {json{{"respuesta", comando(texto)}}.dump(-1, ' ', false, json::error_handler_t::replace)};
            return r;
        }
        decir("Tú (web)> " + texto);
        entrada(texto);
        r.trozos = {"{}"};
        return r;
    }
    auto ultimos = [&](const json& a, size_t def) {
        size_t n = parametro("n").empty() ? def : static_cast<size_t>(std::max(1, std::atoi(parametro("n").c_str())));
        json out = json::array();
        for (size_t i = a.size() > n ? a.size() - n : 0; i < a.size(); ++i) out.push_back(a[i]);
        return out;
    };
    if (ruta == "/api/metas") {
        json a = json::array();
        for (auto& m : mem_.metas()) a.push_back({{"id", m.id}, {"tema", m.tema}, {"estado", m.estado}, {"plan", m.plan}, {"paso", m.paso}, {"resultado", recortar(m.resultado, 600)}});
        r.trozos = {a.dump(-1, ' ', false, json::error_handler_t::replace)};
        return r;
    }
    if (ruta == "/api/mejoras") {
        json a = automejora_.historial();
        for (auto& e : a) { e.erase("sistema_antes"); e.erase("sistema_despues"); }
        r.trozos = {ultimos(a, 50).dump(-1, ' ', false, json::error_handler_t::replace)};
        return r;
    }
    if (ruta == "/api/acciones") {
        json a = json::array();
        for (auto& lin : split_lines(leer_archivo(op_.datos / "acciones.jsonl"))) {
            json j = json::parse(lin, nullptr, false);
            if (j.is_object()) { j.erase("resultado"); a.push_back(j); }
        }
        r.trozos = {ultimos(a, 10).dump(-1, ' ', false, json::error_handler_t::replace)};
        return r;
    }
    if (ruta == "/api/recuerdos") {
        json a = json::array();
        for (auto& x : mem_.recuerdos()) a.push_back({{"id", x.id}, {"tipo", x.tipo}, {"texto", x.texto}});
        r.trozos = {ultimos(a, 10).dump(-1, ' ', false, json::error_handler_t::replace)};
        return r;
    }
    if (ruta == "/api/habilidades") {
        json a = json::array();
        for (auto& h : habilidades_.todas()) a.push_back({{"nombre", h.nombre}, {"descripcion", h.descripcion}, {"usos", h.usos}, {"exitos", h.exitos}});
        r.trozos = {a.dump(-1, ' ', false, json::error_handler_t::replace)};
        return r;
    }
    return error(404, "no existe");
}

// ------------------------------------------------------------------ escalado a otros dispositivos
double Nucleo::medir_velocidad(const std::string& url, const std::string& modelo, std::string* error) {
    Ollama o(url, modelo);
    OpcionesGeneracion op;
    op.temperatura = 0;
    op.num_predict = 64;
    op.num_ctx = 2048;
    auto r = o.chat(json::array({{{"role", "user"}, {"content", "Cuenta del 1 al 40 separando los números con comas."}}}), op, nullptr, nullptr);
    if (!r.ok) {
        if (error) *error = r.error;
        return 0;
    }
    return r.tokens_por_segundo();
}

bool Nucleo::dispositivo(const std::string& nombre, json& d) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& x : cfg_.dispositivos)
        if (x.value("nombre", std::string()) == nombre) { d = x; return true; }
    return false;
}

void Nucleo::usar_dispositivo(const std::string& nombre, const std::string& url, const std::string& modelo) {
    std::string base;
    {
        std::lock_guard<std::mutex> l(mu_);
        cfg_.dispositivo = nombre;
        base = cfg_.modelo;   // el modelo base (local) no cambia: el del dispositivo solo se usa mientras se piensa allí
        cfg_.guardar(op_.datos / "config.json");
    }
    llm_.cambiar_url(url);   // Ollama es seguro entre hilos: lo que esté generando termina con la dirección anterior
    llm_.cambiar_modelo(modelo.empty() ? base : modelo);
    mente_.modelo_cambiado();
    std::lock_guard<std::mutex> l(mu_info_);
    modelo_cache_t_ = 0;
}

std::string Nucleo::revisar_dispositivos() {
    Config c;
    {
        std::lock_guard<std::mutex> l(mu_);
        c = cfg_;
    }
    if (c.dispositivos.empty()) return "";
    json s = mem_.estadisticas();
    // 1) si el dispositivo actual ya no responde, volver al local
    if (c.dispositivo != "local") {
        json d;
        bool existe = dispositivo(c.dispositivo, d);
        if (!existe || !Ollama(d.value("ollama", std::string()), c.modelo).disponible()) {
            usar_dispositivo("local", c.ollama, "");
            return "⚠ " + c.dispositivo + " no responde: vuelvo a pensar en este ordenador (local).";
        }
    }
    if (!c.dispositivo_auto) return "";
    // 2) medir los que no se han medido y elegir el más rápido que responda
    double v_local = s.value("velocidad_local", s.value("tokens_por_segundo", 0.0));
    std::string mejor = "local", url_mejor = c.ollama, modelo_mejor;
    double v_mejor = v_local;
    for (auto& d : c.dispositivos) {
        std::string n = d.value("nombre", std::string()), url = d.value("ollama", std::string()), mod = d.value("modelo", c.modelo);
        if (n.empty() || url.empty() || !Ollama(url, mod).disponible()) continue;
        double v = s.value("velocidad_" + n, 0.0);
        if (v <= 0) {
            std::string err;
            v = medir_velocidad(url, mod, &err);
            if (v > 0) mem_.fijar("velocidad_" + n, v);
            else continue;
        }
        if (v > v_mejor * 1.5) { mejor = n; url_mejor = url; modelo_mejor = mod; v_mejor = v; }   // solo si es claramente más rápido
    }
    if (mejor == c.dispositivo || mejor == "local") return "";
    usar_dispositivo(mejor, url_mejor, modelo_mejor);
    char buf[160];
    std::snprintf(buf, sizeof(buf), " (%.1f tokens/s frente a %.1f aquí)", v_mejor, v_local);
    mem_.anadir_turno("orion", "[Escalado] Me paso a pensar en " + mejor + buf + ".");
    return "★ Me paso a pensar en «" + mejor + "»" + buf + ". Si deja de responder, vuelvo sola a este ordenador.";
}

std::string Nucleo::evaluar_limites(bool forzar) {
    json s = mem_.estadisticas();
    if (!forzar && ahora_s() - s.value("limites_aviso", 0.0) < 86400) return "";
    Hardware h = medir_hardware(op_.datos);
    Config c;
    {
        std::lock_guard<std::mutex> l(mu_);
        c = cfg_;
    }
    std::vector<std::string> limites;
    double tps = s.value("tokens_por_segundo", 0.0);
    char buf[200];
    if (tps > 0 && tps < 4) { std::snprintf(buf, sizeof(buf), "pienso despacio (%.1f tokens/s: una respuesta larga tarda minutos)", tps); limites.push_back(buf); }
    if (h.ram_total > 0 && h.ram_libre < 1.5e9) limites.push_back("queda poca RAM libre (" + formato_bytes(h.ram_libre) + " de " + formato_bytes(h.ram_total) + ")");
    double tp = s.value("tokens_prompt_ultimo", 0.0);
    if (tp > 0.9 * c.num_ctx) limites.push_back("mi memoria de trabajo se llena (" + std::to_string(static_cast<int>(tp)) + " de " + std::to_string(c.num_ctx) + " tokens de contexto)");
    if (limites.empty()) return "";
    mem_.fijar("limites_aviso", ahora_s());
    std::string texto = "Me noto limitada por el hardware: " + join(limites, "; ") + ".";
    mem_.recordar("yo", texto, "limites");
    bool hay_otros = !c.dispositivos.empty();
    return "⚠ " + texto + (hay_otros ? " Uso tus otros dispositivos cuando están disponibles (/dispositivos)."
                                     : " Si tienes otro ordenador con Ollama (por ejemplo tu PC con RTX), regístralo con "
                                       "/dispositivo añadir <nombre> http://IP:11434 [modelo] y lo usaré cuando esté encendido.");
}

bool Nucleo::mejorarse(bool forzada) {
    std::string no = automejora_.disponible();
    if (!no.empty()) {
        nota("no puedo mejorarme ahora: " + no);
        return false;
    }
    mem_.fijar("automejora_ultima", ahora_s());
    fijar_actividad("mejorándome (mido, propongo un cambio y lo vuelvo a medir)");
    nota(std::string(forzada ? "empiezo" : "me toca") + " un ciclo de automejora medida…");
    Config c;
    {
        std::lock_guard<std::mutex> l(mu_);
        c = cfg_;
    }
    auto r = automejora_.ciclo(c, &cancelar_fondo_, [this](const std::string& t) { nota(t); });
    if (r.cancelada || cancelar_fondo_) {
        mem_.fijar("automejora_ultima", 0.0);   // se retoma más tarde
        nota("lo dejo para luego: te atiendo");
        return true;
    }
    if (r.aceptada) {
        std::lock_guard<std::mutex> l(mu_);
        if (r.temperatura > 0) cfg_.temperatura = r.temperatura;
        if (r.max_pasos > 0) cfg_.max_pasos = r.max_pasos;
        cfg_.guardar(op_.datos / "config.json");
    }
    if (r.aceptada) mem_.anadir_turno("orion", "[Automejora] " + r.texto);
    decir((op_.color ? std::string(CIAN) + (r.aceptada ? "★ " : "· ") + NORMAL : (r.aceptada ? "★ " : "· ")) + std::string("Automejora: ") + r.texto);
    return true;
}

Herramientas Nucleo::herramientas(const std::string& origen) {
    ContextoHerramientas c;
    c.datos = op_.datos;
    c.repo = op_.repo;
    c.memoria = &mem_;
    c.habilidades = &habilidades_;
    {
        std::lock_guard<std::mutex> l(mu_);
        c.permisos = cfg_.permisos;
    }
    c.origen = origen;
    c.aviso = [this](const std::string& t) { nota(t); };
    c.reinicio_pedido = &reinicio_pedido_;
    c.vectorizar = [this](const std::string& t) { return mente_.vectorizar(t); };
    c.modelo = llm_.modelo();
    {
        std::lock_guard<std::mutex> l(mu_);
        c.cmake = cfg_.cmake;
    }
    return Herramientas(c);
}

bool Nucleo::trabajar_en_metas() {
    Meta m;
    if (!mem_.proxima_meta(m)) return false;
    if (m.intentos >= 3) {
        mem_.cerrar_meta(m.id, "fallida", "Fallé 3 veces seguidas en el paso " + std::to_string(m.paso + 1) + ".");
        nota("abandono la meta «" + m.tema + "» tras 3 fallos seguidos");
        return true;
    }
    std::string que = "aprendiendo: " + m.tema + (m.plan.empty() ? " (planificando)" : " (paso " + std::to_string(std::min(m.paso + 1, m.plan.size())) + "/" + std::to_string(m.plan.size()) + ")");
    fijar_actividad(que);
    nota(que + "…");
    mente_.fijar_estado_actual(linea_estado_para_modelo());
    auto h = herramientas("meta " + m.id);
    Mente::AvanceMeta av;
    try {
        av = mente_.trabajar_en_meta(m, &h, &cancelar_fondo_);
    } catch (...) {
        mem_.contar_intento_meta(m.id);
        throw;
    }
    if (av.cancelada || cancelar_fondo_) {
        nota("lo dejo para luego: te atiendo");
        return true;
    }
    if (!av.terminada) {
        nota("meta " + m.id + ": " + av.texto);
        quizas_reiniciar();
        return true;
    }
    mem_.cerrar_meta(m.id, "hecha", av.texto);
    mem_.recordar("aprendido", "Sobre «" + m.tema + "»: " + recortar(av.texto, 500), "meta");
    mem_.anadir_turno("orion", "[He terminado de aprender sobre «" + m.tema + "»] " + av.texto);
    decir((op_.color ? std::string(CIAN) + "★ " + NORMAL : "★ ") + "He terminado de aprender sobre «" + m.tema + "»:\n" + av.texto);
    quizas_reiniciar();
    return true;
}

bool Nucleo::tarea_a_solas(bool forzar_reflexion) {
    double silencio = ahora_s() - ultima_interaccion_;
    std::string que, res;
    if (forzar_reflexion || mente_.hay_que_reflexionar(silencio)) {
        que = "reflexionando sobre la conversación";
        fijar_actividad(que);
        nota(que + "…");
        res = mente_.reflexionar(&cancelar_fondo_);
    } else if (cfg_.permisos != "ninguno" && trabajar_en_metas()) {
        return true;
    } else if (mente_.hay_que_indexar()) {
        que = "ordenando mis recuerdos por significado";
        fijar_actividad(que);
        res = mente_.indexar(&cancelar_fondo_);
        if (!res.empty() && !cancelar_fondo_) nota(res);
        return true;
    } else if (mente_.hay_que_resumir()) {
        que = "resumiendo conversaciones antiguas";
        fijar_actividad(que);
        nota(que + "…");
        res = mente_.resumir_conversacion(&cancelar_fondo_);
    } else if (mente_.hay_que_escribir_diario()) {
        que = "escribiendo mi diario";
        fijar_actividad(que);
        nota(que + "…");
        mente_.fijar_estado_actual(linea_estado_para_modelo());
        res = mente_.escribir_diario(&cancelar_fondo_);
    } else if (mente_.hay_que_actualizar_yo()) {
        que = "revisando cómo me veo a mí misma (yo.md)";
        fijar_actividad(que);
        nota(que + "…");
        res = mente_.actualizar_yo(&cancelar_fondo_, hechos_medidos());
    } else if (cfg_.permisos == "total" && cfg_.automejora_cada_h > 0 && silencio >= 1800 &&
               ahora_s() - mem_.estadisticas().value("automejora_ultima", 0.0) >= cfg_.automejora_cada_h * 3600 && automejora_.disponible().empty()) {
        return mejorarse(false);
    } else if (cfg_.permisos == "total" && cfg_.practica_cada_min > 0 && mente_.hay_que_practicar(cfg_.practica_cada_min * 60.0)) {
        que = "practicando (predigo lo que hace un script y lo compruebo ejecutándolo)";
        fijar_actividad(que);
        nota(que + "…");
        res = mente_.practicar(&cancelar_fondo_, op_.datos / "practica");
    } else {
        return false;   // nada nuevo: descansa (no gasta CPU)
    }
    if (cancelar_fondo_) nota("lo dejo para luego: te atiendo");
    else nota(res.empty() ? "nada nuevo" : res);
    return true;
}

// ------------------------------------------------------------------ medidas
InfoModelo Nucleo::info_modelo(bool fresca) {
    std::lock_guard<std::mutex> l(mu_info_);
    if (fresca || ahora_s() - modelo_cache_t_ > 60) {
        modelo_cache_ = llm_.info();
        modelo_cache_t_ = ahora_s();
    }
    return modelo_cache_;
}

std::string Nucleo::linea_estado_para_modelo() {
    Hardware h = medir_hardware(op_.datos);
    InfoModelo m = info_modelo(false);
    Medida me = mente_.medida();
    char buf[512];
    std::snprintf(buf, sizeof(buf), "Me ejecuto en: %s, %s (%d hilos), RAM %s (libre %s). Modelo: %s%s. Velocidad medida: %.1f tokens/s. Memoria: %zu recuerdos.",
                  h.so.c_str(), h.cpu.c_str(), h.hilos, formato_bytes(h.ram_total).c_str(), formato_bytes(h.ram_libre).c_str(), cfg_.modelo.c_str(),
                  m.parametros > 0 ? (" (" + formato_numero(m.parametros) + " de parámetros)").c_str() : "", me.tokens_por_segundo,
                  mem_.recuerdos().size());
    return buf;
}

std::string Nucleo::describir_cuerpo() {
    InfoModelo m = info_modelo(false);
    Hardware h = medir_hardware(op_.datos);
    std::string s;
    if (!op_.repo.empty() && fs::exists(op_.repo / "CMakeLists.txt"))
        s += "- Mi código fuente (C++20) está en " + fs::absolute(op_.repo).string() +
             ": src/nucleo.cpp (bucle de vida y comandos), src/mente.cpp (prompts y razonamiento), src/herramientas.cpp (mis herramientas), "
             "src/memoria.cpp, src/ollama.cpp (acceso al modelo), src/http.cpp, src/sistema.cpp, src/main_orion.cpp, src/main_guardian.cpp, "
             "include/orion/*.hpp, docs/. Puedo leerlo y modificarlo. Para cambiarme: edito el código, uso compilar_nueva_version y después "
             "reiniciarme (el guardián aplica la versión nueva y vuelve a la anterior si falla). Registro cada cambio con git (git add/commit con ejecutar_comando).\n";
    else
        s += "- No sé dónde está mi código fuente (arráncame desde la carpeta del repositorio o con --repo).\n";
    if (!op_.prompts.empty())
        s += "- Mis instrucciones de comportamiento están en " + fs::absolute(op_.prompts / "sistema.md").string() +
             ". Puedo mejorarlas: mi automejora (/mejorate) mide cada cambio con mi banco de pruebas (evaluacion/banco.json) y lo confirma con el "
             "examen de referencia antes de quedárselo.\n";
    s += "- Mi memoria está en " + fs::absolute(op_.datos).string() +
         ": conversacion.jsonl, recuerdos.jsonl, diario.md, yo.md, metas.json, acciones.jsonl (todo lo que hago con herramientas), config.json. "
         "Puedo leerla y modificarla.\n";
    s += "- Mis pesos: modelo " + cfg_.modelo + (m.parametros > 0 ? " (" + formato_numero(m.parametros) + " de parámetros" + (m.cuantizacion.empty() ? "" : ", " + m.cuantizacion) + ")" : "") +
         (m.ruta_pesos.empty() ? ", gestionado por Ollama (en Windows suele estar en %USERPROFILE%\\.ollama\\models)"
                               : ", archivo GGUF " + m.ruta_pesos + " (" + formato_bytes(m.bytes_pesos) + ")") +
         ". Puedo leerlos y copiarlos; cambiarlos de verdad requiere entrenarlos (fase 5, con GPU). Con `ollama create` puedo crear variantes del modelo.\n";
    s += "- Mi hardware: " + h.so + ", " + h.cpu + " (" + std::to_string(h.hilos) + " hilos), RAM " + formato_bytes(h.ram_total) + ".\n";
    s += "- Dónde pienso: " + (cfg_.dispositivo == "local" ? std::string("este ordenador") : "«" + cfg_.dispositivo + "» (" + llm_.url() + ")") +
         (cfg_.dispositivos.empty() ? ". No tengo otros dispositivos registrados; si me quedo corta, puedo pedir a mi creador que registre uno suyo (/dispositivo añadir)."
                                    : ". Dispositivos de mi creador que puedo usar: " + std::to_string(cfg_.dispositivos.size()) + " (me paso sola al más rápido).") + "\n";
    s += "- Mis permisos: " + cfg_.permisos + ". Cada acción que hago con herramientas se muestra a mi creador y queda registrada.";
    return s;
}

std::string Nucleo::hechos_medidos() {
    Hardware h = medir_hardware(op_.datos);
    InfoModelo m = info_modelo(true);
    Medida me = mente_.medida();
    json s = mem_.estadisticas();
    std::string dispositivo = !m.cargado ? "no cargado ahora mismo" : m.bytes_en_vram <= 0 ? "solo CPU (sin GPU)"
                              : m.bytes_en_vram >= m.bytes_en_memoria ? "GPU" : "repartido entre CPU y GPU";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.1f", me.tokens_por_segundo);
    return "- Sistema: " + h.so + "; CPU: " + h.cpu + " (" + std::to_string(h.hilos) + " hilos)\n" +
           "- RAM: " + formato_bytes(h.ram_total) + " en total, " + formato_bytes(h.ram_libre) + " libre\n" +
           "- Modelo: " + cfg_.modelo + (m.parametros > 0 ? ", " + formato_numero(m.parametros) + " de parámetros" : "") +
           (m.cuantizacion.empty() ? "" : ", " + m.cuantizacion) + "; ocupa " + formato_bytes(m.bytes_en_memoria) + "; se ejecuta en: " + dispositivo + "\n" +
           "- Velocidad medida: " + buf + " tokens/s\n" +
           "- Memoria propia: " + std::to_string(mem_.recuerdos().size()) + " recuerdos, " + std::to_string(mem_.num_turnos()) + " turnos, " +
           std::to_string(mem_.entradas_diario()) + " entradas de diario\n" +
           "- Tiempo encendida (acumulado): " + formato_duracion(s.value("segundos_encendida", 0.0) + (ahora_s() - inicio_)) + "\n";
}

// ------------------------------------------------------------------ comandos
std::string Nucleo::comando(const std::string& linea) {
    std::lock_guard<std::mutex> l(mu_cmd_);   // terminal e interfaz web: un comando cada vez
    return comando_sin_bloqueo(linea);
}

std::string Nucleo::comando_sin_bloqueo(const std::string& linea) {
    std::string cmd = split(linea, ' ')[0];
    std::string arg = trim(linea.substr(cmd.size()));
    if (cmd == "/ayuda" || cmd == "/?") return AYUDA;

    if (cmd == "/estado") {
        json s = mem_.estadisticas();
        Medida me = mente_.medida();
        size_t en_cola;
        {
            std::lock_guard<std::mutex> l(mu_);
            en_cola = cola_.size();
        }
        char vel[64];
        std::snprintf(vel, sizeof(vel), "%.1f tokens/s", me.tokens_por_segundo);
        return "Ahora: " + actividad() + (pausado_ ? " (pensamiento a solas en pausa)" : "") + "\n" +
               "Ritmo: " + ritmo_nombre(ritmo_) + " (piensa a solas cada " + formato_duracion(intervalo_ritmo()) + " si no le hablas)\n" +
               "Encendida: " + formato_duracion(ahora_s() - inicio_) + " esta vez, " +
               formato_duracion(s.value("segundos_encendida", 0.0) + ahora_s() - inicio_) + " en total (" +
               std::to_string(static_cast<long long>(s.value("arranques", 0.0))) + " arranques)\n" +
               "Llamadas al modelo: " + std::to_string(me.llamadas) + " · tokens generados: " + formato_numero(static_cast<double>(me.tokens)) +
               " · última velocidad: " + vel + "\n" +
               "Reflexiones: " + std::to_string(static_cast<long long>(s.value("reflexiones", 0.0))) + " · mensajes en cola: " + std::to_string(en_cola);
    }

    if (cmd == "/modelo") {
        if (empieza_por(arg, "usar ")) {
            std::string nuevo = trim(arg.substr(5));
            if (nuevo.empty()) return "Uso: /modelo usar <nombre>   (p. ej. qwen2.5:7b)";
            {
                std::lock_guard<std::mutex> l(mu_);
                modelo_pendiente_ = nuevo;
                Config c = cfg_;
                c.modelo = nuevo;
                c.guardar(op_.datos / "config.json");
            }
            cv_.notify_all();
            return "Cambiaré a " + nuevo + " en cuanto termine lo que esté haciendo (guardado en config.json). "
                   "Si no lo tienes: ollama pull " + nuevo;
        }
        InfoModelo m = info_modelo(true);
        if (!m.ok) return "⚠ " + m.error;
        Medida me = mente_.medida();
        std::string dispositivo = !m.cargado ? "no está cargado ahora (se carga al hablar)" : m.bytes_en_vram <= 0 ? "CPU"
                                  : m.bytes_en_vram >= m.bytes_en_memoria ? "GPU" : "CPU + GPU";
        char vel[64];
        std::snprintf(vel, sizeof(vel), "%.1f tokens/s", me.tokens_por_segundo);
        std::string params = m.parametros > 0 ? formato_numero(m.parametros) + " (" + std::to_string(static_cast<long long>(m.parametros)) + ")"
                                              : m.tamano_parametros;
        return "Modelo: " + cfg_.modelo + "\n" + "Parámetros: " + params + "\n" + "Familia: " + (m.familia.empty() ? "?" : m.familia) +
               " · cuantización: " + (m.cuantizacion.empty() ? "?" : m.cuantizacion) + "\n" +
               "Contexto: " + std::to_string(cfg_.num_ctx) + " tokens en uso (máximo del modelo: " + std::to_string(m.contexto_max) + ")\n" +
               "En memoria: " + (m.cargado ? formato_bytes(m.bytes_en_memoria) + " (VRAM: " + formato_bytes(m.bytes_en_vram) + ")" : "—") +
               " · se ejecuta en: " + dispositivo + "\n" + "Velocidad medida: " + vel;
    }

    if (cmd == "/hardware") {
        Hardware h = medir_hardware(op_.datos);
        return "Sistema: " + h.so + "\nCPU: " + h.cpu + " (" + std::to_string(h.hilos) + " hilos)\n" + "RAM: " + formato_bytes(h.ram_total) +
               " en total, " + formato_bytes(h.ram_libre) + " libre\n" + "Orion (sin el modelo): " + formato_bytes(h.ram_proceso) + "\n" +
               "Disco libre (datos/): " + formato_bytes(h.disco_libre);
    }

    if (cmd == "/memoria") {
        std::map<std::string, int> por_tipo;
        auto recs = mem_.recuerdos();
        for (auto& r : recs) por_tipo[r.tipo]++;
        std::string tipos;
        for (auto& [t, n] : por_tipo) tipos += (tipos.empty() ? "" : ", ") + t + " " + std::to_string(n);
        return "Recuerdos: " + std::to_string(recs.size()) + (tipos.empty() ? "" : " (" + tipos + ")") + "\n" +
               "Indexados por significado: " + std::to_string(mem_.num_vectores(llm_.modelo())) + " de " + std::to_string(recs.size()) +
               (mente_.embeddings_disponibles() ? "" : " (mi modelo no da vectores ahora: busco por palabras)") + "\n" +
               "Conversación: " + std::to_string(mem_.num_turnos()) + " turnos\n" + "Diario: " + std::to_string(mem_.entradas_diario()) + " entradas\n" +
               "yo.md: " + formato_bytes(static_cast<double>(mem_.yo().size())) + "\n" +
               "Ocupa en disco: " + formato_bytes(tamano_directorio(op_.datos)) + " en " + fs::absolute(op_.datos).string();
    }

    if (cmd == "/recuerdos") {
        auto recs = mem_.recuerdos();
        size_t n = arg.empty() ? 15 : static_cast<size_t>(std::max(1, std::atoi(arg.c_str())));
        if (recs.empty()) return "Todavía no tengo recuerdos. Se crean al reflexionar sobre lo que hablamos (o con /recuerda).";
        std::string s;
        for (size_t i = recs.size() > n ? recs.size() - n : 0; i < recs.size(); ++i)
            s += "[" + recs[i].id + "] (" + recs[i].tipo + ") " + recs[i].texto + "\n";
        return trim(s);
    }

    if (cmd == "/recuerda") {
        if (arg.empty()) return "Uso: /recuerda <texto>";
        std::string id = mem_.recordar("manual", arg, "manual");
        return id.empty() ? "Ya lo recordaba." : "Guardado como " + id + ".";
    }

    if (cmd == "/olvida") {
        if (arg.empty()) return "Uso: /olvida <id>   (los ids salen en /recuerdos)";
        return mem_.olvidar(arg) ? "Olvidado " + arg + "." : "No tengo ningún recuerdo con id " + arg + ".";
    }

    if (cmd == "/buscar") {
        if (arg.empty()) return "Uso: /buscar <texto>";
        auto vq = mente_.vectorizar(arg);
        auto res = mem_.buscar(arg, 8, true, vq.empty() ? nullptr : &vq, llm_.modelo());
        if (res.empty()) return "No encuentro nada sobre eso.";
        std::string s;
        for (auto& e : res) s += "[" + e.id + "] " + recortar(e.texto, 200) + "\n";
        return trim(s);
    }

    if (cmd == "/diario") {
        size_t n = arg.empty() ? 1 : static_cast<size_t>(std::max(1, std::atoi(arg.c_str())));
        std::string d = trim(mem_.diario(n));
        return d.empty() ? "Mi diario está vacío todavía: escribo cuando hay algo que contar." : d;
    }

    if (cmd == "/yo") return trim(mem_.yo());

    if (cmd == "/metas") {
        auto ms = mem_.metas();
        if (ms.empty()) return "No tengo metas. Se crean cuando no sé algo y digo \"voy a aprender\", o con /aprende <tema>.";
        std::string s;
        for (auto& m : ms)
            s += "[" + m.id + "] " + m.estado + (m.intentos ? " (" + std::to_string(m.intentos) + " intentos)" : "") + " — " + m.tema +
                 (m.resultado.empty() ? "" : "\n      " + recortar(m.resultado, 300)) + "\n";
        return trim(s);
    }

    if (cmd == "/aprende") {
        if (arg.empty()) return "Uso: /aprende <tema>";
        std::string id = mem_.crear_meta(arg, "me lo pidió mi creador con /aprende");
        reflexion_pedida_ = false;
        return "Meta " + id + " creada. Trabajaré en ella a solas cuando no me hables (" + std::string(ritmo_nombre(ritmo_)) + " ritmo) y te avisaré al terminar.";
    }

    if (cmd == "/habilidades") {
        if (empieza_por(arg, "borrar ")) {
            std::string n = trim(arg.substr(7));
            return habilidades_.borrar(n) ? "Habilidad '" + n + "' borrada." : "No tengo ninguna habilidad llamada '" + n + "'.";
        }
        auto hs = habilidades_.todas();
        if (hs.empty()) return "Todavía no tengo habilidades. Las guardo cuando resuelvo algo con un script que funciona.";
        std::string s2;
        for (auto& h : hs)
            s2 += h.nombre + " [" + h.lenguaje + ", " + std::to_string(h.exitos) + "/" + std::to_string(h.usos) + " usos con éxito] — " + recortar(h.descripcion, 160) + "\n";
        return trim(s2) + "\n(/habilidades borrar <nombre> para borrar una; están en " + (op_.datos / "habilidades").string() + ")";
    }

    if (cmd == "/practica") {
        json st = mem_.estadisticas();
        if (arg == "ahora") {
            mem_.fijar("practica_ultima", 0.0);
            practica_pedida_ = true;
            cv_.notify_all();
            return "De acuerdo, practico en cuanto termine lo que esté haciendo.";
        }
        double total = st.value("practicas", 0.0), bien = st.value("practicas_acertadas", 0.0);
        auto lineas = split_lines(leer_archivo(op_.datos / "practica.jsonl"));
        std::string ult;
        for (size_t i = lineas.size() > 5 ? lineas.size() - 5 : 0; i < lineas.size(); ++i) {
            json j = json::parse(lineas[i], nullptr, false);
            if (!j.is_object()) continue;
            ult += "  " + j.value("fecha", std::string()) + " nivel " + std::to_string(j.value("nivel", 0)) + " — " + j.value("tema", std::string()) + ": " +
                   (!j.value("valido", true) ? "ejercicio no válido" : j.value("acierto", false) ? "acierto" : "fallo") + "\n";
        }
        return "Práctica con juez real (predigo lo que imprime un script y lo ejecuto para comprobarlo):\n"
               "Nivel actual: " + std::to_string(static_cast<int>(st.value("practica_nivel", 1.0))) + "/5 · aciertos: " +
               std::to_string(static_cast<int>(bien)) + "/" + std::to_string(static_cast<int>(total)) +
               (total > 0 ? " (" + std::to_string(static_cast<int>(100 * bien / total)) + " %)" : "") + " · cada " + std::to_string(cfg_.practica_cada_min) +
               " min si estoy libre\n" + (ult.empty() ? "" : "Últimas:\n" + ult) + "(/practica ahora para practicar ya; los ejemplos verificados quedan en datos/practica.jsonl)";
    }

    if (cmd == "/dispositivos" || cmd == "/dispositivo") {
        auto partes = split(arg, ' ');
        std::string sub_ = partes.empty() ? "" : partes[0];
        if (sub_ == "añadir" || sub_ == "anadir") {
            if (partes.size() < 3) return "Uso: /dispositivo añadir <nombre> <http://IP:11434> [modelo]";
            std::string nombre = partes[1], url = partes[2], mod = partes.size() > 3 ? partes[3] : "";
            if (!empieza_por(url, "http://")) url = "http://" + url;
            if (nombre == "local") return "«local» es este ordenador: elige otro nombre.";
            {
                std::lock_guard<std::mutex> l(mu_);
                for (auto it = cfg_.dispositivos.begin(); it != cfg_.dispositivos.end(); ++it)
                    if (it->value("nombre", std::string()) == nombre) { cfg_.dispositivos.erase(it); break; }
                json d = {{"nombre", nombre}, {"ollama", url}};
                if (!mod.empty()) d["modelo"] = mod;
                cfg_.dispositivos.push_back(d);
                cfg_.guardar(op_.datos / "config.json");
            }
            mem_.fijar("velocidad_" + nombre, 0.0);
            ultima_revision_ = 0;   // revisarlo en cuanto pueda
            cv_.notify_all();
            std::string ver;
            bool ok = Ollama(url, mod.empty() ? cfg_.modelo : mod).disponible(&ver);
            return "Dispositivo «" + nombre + "» registrado (" + url + (mod.empty() ? "" : ", modelo " + mod) + "). " +
                   (ok ? "Responde (Ollama " + ver + "): lo mediré y lo usaré si es más rápido."
                       : "Ahora no responde: en ese ordenador, Ollama debe escuchar en la red (variable OLLAMA_HOST=0.0.0.0) y el cortafuegos permitir el puerto 11434.");
        }
        if (sub_ == "quitar") {
            if (partes.size() < 2) return "Uso: /dispositivo quitar <nombre>";
            bool quitado = false, era_actual = false;
            {
                std::lock_guard<std::mutex> l(mu_);
                for (auto it = cfg_.dispositivos.begin(); it != cfg_.dispositivos.end(); ++it)
                    if (it->value("nombre", std::string()) == partes[1]) { cfg_.dispositivos.erase(it); quitado = true; break; }
                era_actual = cfg_.dispositivo == partes[1];
                cfg_.guardar(op_.datos / "config.json");
            }
            if (era_actual) usar_dispositivo("local", cfg_.ollama, "");
            return quitado ? "Quitado «" + partes[1] + "»." : "No tengo ningún dispositivo llamado «" + partes[1] + "».";
        }
        if (sub_ == "usar") {
            if (partes.size() < 2) return "Uso: /dispositivo usar <nombre|local>";
            if (partes[1] == "local") { usar_dispositivo("local", cfg_.ollama, ""); return "Vuelvo a pensar en este ordenador."; }
            json d;
            if (!dispositivo(partes[1], d)) return "No tengo ningún dispositivo llamado «" + partes[1] + "».";
            if (!Ollama(d.value("ollama", std::string()), cfg_.modelo).disponible()) return "«" + partes[1] + "» no responde ahora.";
            usar_dispositivo(partes[1], d.value("ollama", std::string()), d.value("modelo", std::string()));
            return "Paso a pensar en «" + partes[1] + "» en cuanto termine lo que esté haciendo.";
        }
        if (sub_ == "auto") {
            bool on = partes.size() > 1 && (partes[1] == "on" || partes[1] == "si" || partes[1] == "sí");
            std::lock_guard<std::mutex> l(mu_);
            cfg_.dispositivo_auto = on;
            cfg_.guardar(op_.datos / "config.json");
            return on ? "Elegiré sola el dispositivo más rápido disponible." : "Solo cambiaré de dispositivo cuando me lo digas (salvo si el actual cae).";
        }
        if (sub_ == "medir") {
            std::string err;
            std::string url_local;
            {
                std::lock_guard<std::mutex> l(mu_);
                url_local = cfg_.ollama;
            }
            std::string s2;
            double vl = medir_velocidad(url_local, cfg_.modelo, &err);
            if (vl > 0) mem_.fijar("velocidad_local", vl);
            char b[64];
            std::snprintf(b, sizeof(b), "%.1f tokens/s", vl);
            s2 += "local: " + (vl > 0 ? std::string(b) : "no responde (" + recortar(err, 80) + ")") + "\n";
            json lista;
            {
                std::lock_guard<std::mutex> l(mu_);
                lista = cfg_.dispositivos;
            }
            for (auto& d : lista) {
                std::string n = d.value("nombre", std::string());
                double v = medir_velocidad(d.value("ollama", std::string()), d.value("modelo", cfg_.modelo), &err);
                if (v > 0) mem_.fijar("velocidad_" + n, v);
                std::snprintf(b, sizeof(b), "%.1f tokens/s", v);
                s2 += n + ": " + (v > 0 ? std::string(b) : "no responde (" + recortar(err, 80) + ")") + "\n";
            }
            return trim(s2);
        }
        // listado
        json st = mem_.estadisticas();
        Config c;
        {
            std::lock_guard<std::mutex> l(mu_);
            c = cfg_;
        }
        char b[64];
        std::snprintf(b, sizeof(b), "%.1f", st.value("velocidad_local", st.value("tokens_por_segundo", 0.0)));
        std::string s2 = std::string(c.dispositivo == "local" ? "▶ " : "  ") + "local: " + c.ollama + " · " + c.modelo + " · " + b + " tokens/s\n";
        for (auto& d : c.dispositivos) {
            std::string n = d.value("nombre", std::string()), url = d.value("ollama", std::string());
            std::string ver;
            bool ok = Ollama(url, d.value("modelo", c.modelo)).disponible(&ver);
            std::snprintf(b, sizeof(b), "%.1f", st.value("velocidad_" + n, 0.0));
            s2 += std::string(c.dispositivo == n ? "▶ " : "  ") + n + ": " + url + " · " + d.value("modelo", c.modelo) + " · " +
                  (ok ? std::string("responde") + (st.value("velocidad_" + n, 0.0) > 0 ? ", " + std::string(b) + " tokens/s" : "") : "no responde") + "\n";
        }
        return trim(s2) + "\nElección automática: " + (c.dispositivo_auto ? "sí" : "no") +
               ". Comandos: /dispositivo añadir <nombre> <url> [modelo] · quitar <nombre> · usar <nombre|local> · medir · auto on|off";
    }

    if (cmd == "/limites") {
        std::string l = evaluar_limites(true);
        return l.empty() ? "Ahora mismo no detecto límites de hardware importantes." : l;
    }

    if (cmd == "/web") {
        if (url_web().empty()) return "La interfaz web está desactivada (\"web_puerto\": 0 en config.json) o no se pudo abrir.";
        if (arg == "abrir") {
#ifdef _WIN32
            ejecutar_shell("start \"\" \"" + url_web() + "\"", {}, 15);
#else
            ejecutar_proceso({"xdg-open", url_web()}, {}, 15);
#endif
            return "Abro " + url_web();
        }
        return "Interfaz web: " + url_web() + "\n(solo accesible desde este ordenador; /web abrir la abre en el navegador)";
    }

    if (cmd == "/mejoras") {
        auto h = automejora_.historial();
        std::string s2 = automejora_.aviso_examen();
        if (h.empty()) return (s2.empty() ? "" : s2 + "\n") + "Todavía no he intentado mejorarme. /mejorate para hacerlo ahora (mide, propone, mide y decide).";
        for (auto& e : h) {
            if (e.value("tipo", std::string()) == "vuelta") { s2 += "#" + std::to_string(e.value("n", 0)) + " " + e.value("fecha", std::string()) + "  ↩ " + e.value("motivo", std::string()) + "\n"; continue; }
            s2 += "#" + std::to_string(e.value("n", 0)) + " " + e.value("fecha", std::string()) + "  " + (e.value("aceptada", false) ? "✓ ACEPTADA" : "✗ rechazada") + " — " +
                  recortar(e.value("motivo", std::string()), 140) + "\n     banco " + e.value("banco_antes", std::string("?")) + " → " + e.value("banco_despues", std::string("—")) +
                  (e.contains("examen_antes") ? " · examen " + e.value("examen_antes", std::string()) + " → " + e.value("examen_despues", std::string()) : "") + " · " +
                  e.value("razon", std::string()) + (e.value("commit", std::string()).empty() ? "" : " · commit " + e.value("commit", std::string())) + "\n";
        }
        return trim(s2) + "\n(/volver <n> deshace una mejora aceptada)";
    }

    if (cmd == "/mejorate") {
        std::string no = automejora_.disponible();
        if (!no.empty()) return "No puedo mejorarme: " + no;
        mejora_pedida_ = true;
        cv_.notify_all();
        return "De acuerdo: mido mi versión actual, propongo un cambio, lo mido y solo me lo quedo si mejora. Puede tardar (cada medición hace varias tareas).";
    }

    if (cmd == "/volver") {
        int n = std::atoi(arg.c_str());
        if (n <= 0) return "Uso: /volver <n>   (los números salen en /mejoras)";
        Config c;
        {
            std::lock_guard<std::mutex> l(mu_);
            c = cfg_;
        }
        auto r = automejora_.volver(n, c);
        if (r.aceptada) {
            std::lock_guard<std::mutex> l(mu_);
            if (r.temperatura > 0) cfg_.temperatura = r.temperatura;
            if (r.max_pasos > 0) cfg_.max_pasos = r.max_pasos;
            cfg_.guardar(op_.datos / "config.json");
        }
        return r.texto;
    }

    if (cmd == "/acciones") {
        size_t n = arg.empty() ? 10 : static_cast<size_t>(std::max(1, std::atoi(arg.c_str())));
        auto lineas = split_lines(leer_archivo(op_.datos / "acciones.jsonl"));
        if (lineas.empty()) return "Todavía no he usado ninguna herramienta.";
        std::string s;
        for (size_t i = lineas.size() > n ? lineas.size() - n : 0; i < lineas.size(); ++i) {
            json j = json::parse(lineas[i], nullptr, false);
            if (!j.is_object()) continue;
            s += j.value("fecha", std::string()) + " [" + j.value("origen", std::string()) + "] " + j.value("herramienta", std::string()) + " " +
                 recortar(j.contains("argumentos") ? j["argumentos"].dump() : "", 160) + "\n      → " +
                 recortar(split_lines(j.value("resultado", std::string())).empty() ? "" : split_lines(j.value("resultado", std::string()))[0], 160) + "\n";
        }
        return trim(s);
    }

    if (cmd == "/permisos") {
        if (arg.empty())
            return "Permisos: " + cfg_.permisos + ". Opciones: total (comandos, archivos, internet, compilarse), lectura (leer archivos e internet), "
                   "ninguno (solo su memoria).";
        if (arg != "total" && arg != "lectura" && arg != "ninguno") return "Opciones: total, lectura, ninguno.";
        {
            std::lock_guard<std::mutex> l(mu_);
            cfg_.permisos = arg;
            cfg_.guardar(op_.datos / "config.json");
        }
        mente_.fijar_cuerpo(describir_cuerpo());
        return "Permisos: " + arg + ".";
    }

    if (cmd == "/reflexiona") {
        reflexion_pedida_ = true;
        cv_.notify_all();
        return "De acuerdo, reflexiono ahora.";
    }

    if (cmd == "/pausa") {
        pausado_ = true;
        cancelar_fondo_ = true;
        return "Dejo de pensar a solas (sigo atendiéndote). /sigue para reanudar.";
    }
    if (cmd == "/sigue") {
        pausado_ = false;
        return "Vuelvo a pensar a solas cuando no me hables.";
    }

    if (cmd == "/ritmo") {
        if (arg.empty()) return std::string("Ritmo: ") + ritmo_nombre(ritmo_) + ". Opciones: bajo (15 min), medio (2 min), alto (20 s).";
        if (arg != "bajo" && arg != "medio" && arg != "alto") return "Opciones: bajo, medio, alto.";
        ritmo_ = ritmo_a_int(arg);
        {
            std::lock_guard<std::mutex> l(mu_);
            cfg_.ritmo = arg;
            cfg_.guardar(op_.datos / "config.json");
        }
        return "Ritmo " + arg + ".";
    }

    if (cmd == "/versiones") {
        if (op_.repo.empty() || !fs::exists(op_.repo / ".git")) return "No encuentro el repositorio git de mi código.";
        auto r = ejecutar_proceso({"git", "log", "--oneline", "-n", "10"}, op_.repo, 15);
        return r.codigo == 0 ? trim(r.salida) : "No pude leer el historial: " + trim(r.salida);
    }

    if (cmd == "/config") {
        std::lock_guard<std::mutex> l(mu_);
        return cfg_.a_json().dump(2);
    }

    return "Comando desconocido: " + cmd + ". Escribe /ayuda.";
}

}  // namespace orion
