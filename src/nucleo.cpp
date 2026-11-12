#include "orion/nucleo.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

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
    : cfg_(std::move(cfg)), op_(std::move(op)), salida_(std::move(salida)), mem_(op_.datos), llm_(cfg_.ollama, cfg_.modelo), mente_(cfg_, mem_, llm_) {
    ritmo_ = ritmo_a_int(cfg_.ritmo);
}

Nucleo::~Nucleo() { parar(); }

void Nucleo::decir(const std::string& t) {
    std::lock_guard<std::mutex> l(mu_salida_);
    salida_(t + "\n");
}

void Nucleo::nota(const std::string& t) {
    std::lock_guard<std::mutex> l(mu_salida_);
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
    if (salir_.exchange(true)) return;
    cancelar_fondo_ = true;
    cv_.notify_all();
    if (hilo_.joinable()) hilo_.join();
    if (inicio_ > 0) mem_.sumar("segundos_encendida", ahora_s() - inicio_);
}

// ------------------------------------------------------------------ hilo de mente
void Nucleo::bucle_mente() {
    int errores_seguidos = 0;
    while (!salir_) {
        std::string msg;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait_for(l, std::chrono::milliseconds(300), [&] { return salir_ || !cola_.empty() || reflexion_pedida_; });
            if (salir_) break;
            if (!cola_.empty()) {
                msg = cola_.front();
                cola_.pop_front();
                ocupado_ = true;
            }
            if (!modelo_pendiente_.empty()) {
                cfg_.modelo = modelo_pendiente_;
                llm_.cambiar_modelo(modelo_pendiente_);
                modelo_pendiente_.clear();
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
        bool forzar = reflexion_pedida_.exchange(false);
        double ahora = ahora_s();
        double espera = intervalo_ritmo() * (1 << std::min(errores_seguidos, 4));
        if (forzar || (op_.autonomo && !pausado_ && ahora - ultima_interaccion_ >= op_.gracia_s && ahora - ultima_tarea_ >= espera)) {
            cancelar_fondo_ = false;
            ultima_tarea_ = ahora;
            try {
                tarea_a_solas(forzar);
                errores_seguidos = 0;
            } catch (const std::exception& e) {
                ++errores_seguidos;
                nota(std::string("no pude terminar: ") + e.what());
            }
            fijar_actividad(pausado_ ? "en pausa" : "en reposo");
            ultima_tarea_ = ahora_s();
        }
    }
}

void Nucleo::atender(const std::string& mensaje) {
    fijar_actividad("respondiendo");
    cancelar_chat_ = false;
    mente_.fijar_estado_actual(linea_estado_para_modelo());
    {
        std::lock_guard<std::mutex> l(mu_salida_);
        salida_(op_.color ? std::string(CIAN) + "Orion> " + NORMAL : "Orion> ");
    }
    auto r = mente_.responder(mensaje, [&](const std::string& t) {
        std::lock_guard<std::mutex> l(mu_salida_);
        salida_(t);
    }, &cancelar_chat_);
    {
        std::lock_guard<std::mutex> l(mu_salida_);
        salida_("\n");
    }
    if (!r.error.empty()) decir("⚠ " + r.error);
    else if (r.cancelada) nota("respuesta interrumpida");
    else if (r.tokens_generados > 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%lld tokens · %.1f tokens/s", r.tokens_generados, r.tokens_por_segundo());
        nota(buf);
    }
    fijar_actividad(pausado_ ? "en pausa" : "en reposo");
}

bool Nucleo::tarea_a_solas(bool forzar_reflexion) {
    double silencio = ahora_s() - ultima_interaccion_;
    std::string que, res;
    if (forzar_reflexion || mente_.hay_que_reflexionar(silencio)) {
        que = "reflexionando sobre la conversación";
        fijar_actividad(que);
        nota(que + "…");
        res = mente_.reflexionar(&cancelar_fondo_);
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
    if (fresca || ahora_s() - modelo_cache_t_ > 600) {
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
        auto res = mem_.buscar(arg, 8, true);
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
