// Núcleo: el "bucle de vida" de Orion.
//  - Hilo de terminal (el que llama a ejecutar()): lee lo que escribes. Los comandos /... se atienden al momento;
//    los mensajes van a una cola y CANCELAN lo que Orion esté pensando a solas.
//  - Hilo de mente: atiende la cola de mensajes; si no hay nada, y según el ritmo, hace tareas a solas
//    (reflexionar, diario, yo.md). Todo queda guardado en datos/ al momento.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "orion/automejora.hpp"
#include "orion/habilidades.hpp"
#include "orion/mente.hpp"
#include "orion/sistema.hpp"

namespace orion {

constexpr int CODIGO_REINICIO = 75;   // el guardián vuelve a arrancar Orion

struct OpcionesNucleo {
    fs::path datos = "datos";
    fs::path repo;                     // carpeta con el código de Orion (para /versiones); vacío = no hay
    bool autonomo = true;              // pensar a solas cuando no le hablan
    bool color = true;
    double gracia_s = 20;              // segundos de silencio antes de ponerse a pensar a solas
    std::function<void(int)> terminar; // cómo terminar el proceso (reinicio pedido por ella); vacío = salir del bucle
    fs::path prompts;                  // sus instrucciones (prompts/sistema.md); vacío = las de por defecto
    fs::path ejecutable;               // su propio ejecutable (para medirse)
};

class Nucleo {
public:
    using Salida = std::function<void(const std::string&)>;   // recibe texto tal cual (sin salto de línea añadido)
    Nucleo(Config cfg, OpcionesNucleo op, Salida salida);
    ~Nucleo();

    void arrancar();                                  // carga la memoria, saluda y lanza el hilo de mente
    int ejecutar(const std::function<bool(std::string&)>& leer);   // bucle de terminal; devuelve el código de salida
    // Procesa una línea como si la hubieras escrito. Devuelve false si hay que salir.
    bool entrada(const std::string& linea);
    std::string comando(const std::string& linea);   // ejecuta un /comando y devuelve su texto
    void esperar_inactividad(double max_s);          // para pruebas: espera a que la cola quede vacía
    void parar();
    int codigo_salida() const { return codigo_salida_; }

    Memoria& memoria() { return mem_; }
    Mente& mente() { return mente_; }
    std::string actividad() const;
    std::string url_web() const;                     // "" si la interfaz web no está activa
    RespuestaServidor atender_web(const PeticionHttp& p);   // público para poder probarlo
    // Escalado: revisa los dispositivos registrados (se pasa al más rápido, vuelve al local si cae). Devuelve lo que hizo.
    std::string revisar_dispositivos();
    // Detecta si el hardware la limita y, como mucho una vez al día, lo dice. Devuelve el aviso ("" si no hay).
    std::string evaluar_limites(bool forzar = false);

private:
    void bucle_mente();
    void atender(const std::string& mensaje);
    bool tarea_a_solas(bool forzar_reflexion);
    std::string hechos_medidos();
    std::string describir_cuerpo();
    Herramientas herramientas(const std::string& origen);
    bool trabajar_en_metas();
    void quizas_reiniciar();
    std::string linea_estado_para_modelo();
    void decir(const std::string& texto);            // línea completa
    void nota(const std::string& texto);             // nota tenue de actividad
    void fijar_actividad(const std::string& a);
    double intervalo_ritmo() const;
    InfoModelo info_modelo(bool fresca);

    Config cfg_;
    OpcionesNucleo op_;
    Salida salida_;
    Memoria mem_;
    Ollama llm_;
    Habilidades habilidades_;
    Mente mente_;
    Automejora automejora_;
    std::mutex mu_salida_, mu_;
    bool linea_abierta_ = false, prefijo_pendiente_ = false;   // estado de la línea "Orion> ..." (protegido por mu_salida_)
    std::condition_variable cv_;
    std::deque<std::string> cola_;
    std::atomic<bool> salir_{false}, pausado_{false}, cancelar_fondo_{false}, cancelar_chat_{false}, ocupado_{false};
    std::atomic<bool> reflexion_pedida_{false}, reinicio_pedido_{false}, parado_{false}, practica_pedida_{false}, mejora_pedida_{false};
    bool mejorarse(bool forzada);
    void registrar_evento(const std::string& t);
    double medir_velocidad(const std::string& url, const std::string& modelo, std::string* error);
    bool dispositivo(const std::string& nombre, json& d);
    void usar_dispositivo(const std::string& nombre, const std::string& url, const std::string& modelo);
    double ultima_revision_ = 0;
    std::string comando_sin_bloqueo(const std::string& linea);
    void iniciar_web();
    json estado_json();
    std::unique_ptr<ServidorHttp> web_;
    std::string token_web_;
    int puerto_web_ = 0;
    std::mutex mu_ev_, mu_cmd_;
    std::string eventos_;
    long long base_eventos_ = 0;
    std::atomic<int> ritmo_{1};
    std::string modelo_pendiente_;
    InfoModelo modelo_cache_;
    double modelo_cache_t_ = 0;
    std::mutex mu_info_;
    std::string actividad_ = "en reposo";
    mutable std::mutex mu_act_;
    std::thread hilo_;
    double inicio_ = 0, ultima_interaccion_ = 0, ultima_tarea_ = 0;
    int codigo_salida_ = 0;
};

}  // namespace orion
