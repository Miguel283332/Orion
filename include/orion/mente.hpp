// Mente: todo lo que Orion hace CON el modelo. Construye el contexto (quién es, qué recuerda, en qué hardware está)
// y realiza las tareas: responder, reflexionar (extraer recuerdos), escribir el diario y actualizar yo.md.
#pragma once
#include <atomic>
#include <functional>
#include <string>

#include "orion/herramientas.hpp"
#include "orion/memoria.hpp"
#include "orion/ollama.hpp"

namespace orion {

struct Config {
    std::string modelo = "qwen2.5:3b";
    std::string ollama = "http://127.0.0.1:11434";
    std::string ritmo = "medio";        // bajo | medio | alto: cada cuánto piensa a solas
    int num_ctx = 8192;                 // con herramientas hace falta más contexto
    std::string permisos = "total";     // total | lectura | ninguno (qué herramientas puede usar)
    int max_pasos = 10;                 // herramientas por turno como máximo
    std::string cmake = "cmake";        // ruta a cmake (para compilarse a sí misma)
    int practica_cada_min = 60;         // práctica con juez real a solas (0 = nunca)
    double automejora_cada_h = 12;      // ciclo de automejora medida a solas (0 = nunca)
    bool verificar_codigo = true;       // ejecutar el código que aparezca en sus respuestas y hacerla corregir lo que no cuadre
    int web_puerto = 7860;              // interfaz web local (0 = desactivada)
    json dispositivos = json::array();  // otros ordenadores TUYOS con Ollama: [{"nombre","ollama","modelo"}]
    std::string dispositivo = "local";  // en cuál piensa ahora
    bool dispositivo_auto = true;       // pasarse sola al más rápido disponible (y volver si cae)
    int semilla = -1;                   // -1: aleatoria (en las evaluaciones se fija para poder comparar)
    int num_predict = 1024;
    double temperatura = 0.7;
    static Config cargar(const fs::path& archivo);   // crea el archivo con valores por defecto si no existe
    void guardar(const fs::path& archivo) const;
    json a_json() const;
};

struct Medida {          // rendimiento observado del modelo en este hardware
    double tokens_por_segundo = 0;
    long long llamadas = 0, tokens = 0;
};

class Mente {
public:
    Mente(Config& cfg, Memoria& mem, Ollama& llm);
    using Trozo = std::function<void(const std::string&)>;

    // Responde usando herramientas si hace falta (h puede ser nullptr: sin herramientas).
    ResultadoGeneracion responder(const std::string& mensaje, const Trozo& por_trozo, const std::atomic<bool>* cancelar, Herramientas* h = nullptr);
    // Trabaja a solas en una meta de aprendizaje, un paso cada vez (primero la planifica). Se puede interrumpir y retomar.
    struct AvanceMeta {
        bool terminada = false;
        bool cancelada = false;
        std::string texto;   // resumen final si terminó; si no, qué ha hecho en esta sesión
    };
    AvanceMeta trabajar_en_meta(const Meta& meta, Herramientas* h, const std::atomic<bool>* cancelar);
    // Práctica con juez real: inventa un script, predice lo que imprime y lo ejecuta para comprobarlo.
    // Ajusta la dificultad para acertar ~la mitad. Devuelve un texto corto ("" si se canceló).
    std::string practicar(const std::atomic<bool>* cancelar, const fs::path& dir_practica);
    bool hay_que_practicar(double cada_s) const;
    void fijar_habilidades(Habilidades* h) { habilidades_ = h; }
    void fijar_dir_prompts(const fs::path& d) { dir_prompts_ = d; }
    const fs::path& dir_prompts() const { return dir_prompts_; }
    std::string reglas() const;                     // prompts/sistema.md, o las de por defecto
    static std::string reglas_por_defecto();
    void fijar_cuerpo(const std::string& s) { cuerpo_ = s; }
    void modelo_cambiado() { sin_herramientas_ = false; embed_ok_ = true; embed_reintento_ = 0; }   // descripción de su código, memoria, pesos y hardware
    // Tareas a solas. Devuelven un texto corto para mostrar ("2 recuerdos nuevos"), o "" si no había nada que hacer.
    // Lanzan std::runtime_error si el modelo falla; si se cancelan devuelven "" sin cambiar nada.
    std::string reflexionar(const std::atomic<bool>* cancelar);
    std::string escribir_diario(const std::atomic<bool>* cancelar);
    std::string actualizar_yo(const std::atomic<bool>* cancelar, const std::string& hechos_medidos);

    // Memoria por significado (fase 2)
    std::vector<float> vectorizar(const std::string& texto);       // vacío si el modelo no da embeddings
    std::string indexar(const std::atomic<bool>* cancelar);         // vectores de los recuerdos que no lo tienen
    std::string resumir_conversacion(const std::atomic<bool>* cancelar);   // resume bloques antiguos de la conversación
    bool hay_que_indexar() const;
    bool hay_que_resumir() const;
    bool embeddings_disponibles() const { return embed_ok_; }

    bool hay_que_reflexionar(double segundos_sin_hablar) const;
    bool hay_que_escribir_diario() const;
    bool hay_que_actualizar_yo() const;

    std::string contexto_sistema(const std::string& consulta, const std::string& estado_actual, const std::vector<float>* vq = nullptr) const;
    void fijar_estado_actual(const std::string& s) { estado_actual_ = s; }
    Medida medida() const;

private:
    ResultadoGeneracion generar(const json& mensajes, const OpcionesGeneracion& op, const Trozo& por_trozo, const std::atomic<bool>* cancelar,
                                const json* herramientas = nullptr);
    ResultadoGeneracion con_herramientas(json& mensajes, const OpcionesGeneracion& op, const Trozo& por_trozo, const std::atomic<bool>* cancelar,
                                         Herramientas* h, int max_pasos);
    Config& cfg_;
    Memoria& mem_;
    Ollama& llm_;
    std::string estado_actual_;
    void verificar_codigo_de_respuesta(json& mensajes, ResultadoGeneracion& r, const Trozo& por_trozo, const std::atomic<bool>* cancelar,
                                       Herramientas* h);
    std::string cuerpo_;
    bool sin_herramientas_ = false;   // el modelo no admite herramientas
    std::atomic<bool> embed_ok_{true};
    Habilidades* habilidades_ = nullptr;
    fs::path dir_prompts_;
    double embed_reintento_ = 0;
};

std::string extraer_json_objeto(const std::string& texto);   // primer objeto {...} equilibrado
// Llamadas a herramientas escritas como texto (algunos modelos pequeños no usan el formato nativo).
json bloques_de_codigo(const std::string& texto);   // [{"lenguaje", "codigo"}] de los bloques ``` de un texto
json llamadas_en_texto(const std::string& texto, const std::vector<std::string>& nombres);

}  // namespace orion
