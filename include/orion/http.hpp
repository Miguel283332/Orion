// HTTP/1.1 mínimo sobre sockets (Windows y POSIX), sin dependencias.
// Cliente: GET/POST y POST con respuesta en streaming (NDJSON de Ollama), cancelable en cualquier momento.
// Servidor: muy simple, para pruebas (Ollama simulado) y, más adelante, una interfaz web local.
#pragma once
#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace orion {

struct RespuestaHttp {
    int estado = 0;            // código HTTP; 0 si no hubo conexión
    std::string cuerpo;
    std::string error;         // vacío si todo fue bien
    bool cancelada = false;
};

// Decodificador incremental del cuerpo (Content-Length o chunked). Público para poder probarlo.
class DecodificadorCuerpo {
public:
    void configurar(bool chunked, long long longitud);   // longitud < 0: hasta cerrar la conexión
    std::string alimentar(const std::string& datos);     // devuelve los bytes del cuerpo ya decodificados
    bool terminado() const { return terminado_; }
private:
    bool chunked_ = false, terminado_ = false;
    long long restante_ = -1;
    int fase_ = 0;               // chunked: 0 tamaño, 1 datos, 2 CRLF tras datos
    std::string buf_;
};

class ClienteHttp {
public:
    explicit ClienteHttp(const std::string& url_base);   // "http://127.0.0.1:11434"
    RespuestaHttp get(const std::string& ruta, int timeout_s = 30) const;
    RespuestaHttp post(const std::string& ruta, const std::string& json, int timeout_s = 120) const;
    void cabecera(const std::string& k, const std::string& v) { extra_ += k + ": " + v + "\r\n"; }
    // Llama a por_linea con cada línea completa del cuerpo. Si por_linea devuelve false o *cancelar se pone a true,
    // cierra la conexión (Ollama deja de generar al cerrarse la conexión).
    RespuestaHttp post_stream(const std::string& ruta, const std::string& json, const std::function<bool(const std::string&)>& por_linea,
                              const std::atomic<bool>* cancelar, int timeout_s = 600) const;
    const std::string& host() const { return host_; }
    int puerto() const { return puerto_; }
private:
    RespuestaHttp peticion(const std::string& metodo, const std::string& ruta, const std::string& cuerpo,
                           const std::function<bool(const std::string&)>* por_linea, const std::atomic<bool>* cancelar, int timeout_s) const;
    std::string host_;
    int puerto_ = 80;
    std::string extra_;
};

struct PeticionHttp {
    std::string metodo, ruta, cuerpo;
    std::map<std::string, std::string> cabeceras;   // nombres en minúsculas
    std::string cabecera(const std::string& k) const { auto it = cabeceras.find(k); return it == cabeceras.end() ? "" : it->second; }
};
struct RespuestaServidor {
    int estado = 200;
    std::string tipo = "application/json";
    std::map<std::string, std::string> cabeceras;   // cabeceras extra
    std::vector<std::string> trozos;   // si hay más de uno, se envía en chunked (streaming)
    int pausa_ms = 0;                  // pausa entre trozos (para simular un modelo lento)
};

class ServidorHttp {
public:
    using Manejador = std::function<RespuestaServidor(const PeticionHttp&)>;
    explicit ServidorHttp(Manejador m);
    ~ServidorHttp();
    int iniciar(int puerto = 0, const std::string& ip = "127.0.0.1");   // devuelve el puerto real (0 = libre)
    void parar();
private:
    void bucle();
    void atender(long long socket);
    Manejador manejador_;
    long long escucha_ = -1;
    std::atomic<bool> activo_{false};
    std::thread hilo_;
    struct Cliente {
        std::thread hilo;
        std::shared_ptr<std::atomic<bool>> terminado;
    };
    std::list<Cliente> clientes_;   // se recogen los que terminan (si no, una interfaz web que consulta cada segundo los acumularía)
};

}  // namespace orion
