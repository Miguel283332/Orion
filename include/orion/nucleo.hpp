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
#include <mutex>
#include <string>
#include <thread>

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

private:
    void bucle_mente();
    void atender(const std::string& mensaje);
    bool tarea_a_solas(bool forzar_reflexion);
    std::string hechos_medidos();
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
    Mente mente_;
    std::mutex mu_salida_, mu_;
    std::condition_variable cv_;
    std::deque<std::string> cola_;
    std::atomic<bool> salir_{false}, pausado_{false}, cancelar_fondo_{false}, cancelar_chat_{false}, ocupado_{false};
    std::atomic<bool> reflexion_pedida_{false};
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
