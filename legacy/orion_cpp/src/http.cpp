#include "orion/http.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "orion/util.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t SOCK_INVALIDO = INVALID_SOCKET;
static void cerrar_socket(sock_t s) { closesocket(s); }
static bool error_es_timeout() { int e = WSAGetLastError(); return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK; }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using sock_t = int;
static const sock_t SOCK_INVALIDO = -1;
static void cerrar_socket(sock_t s) { ::close(s); }
static bool error_es_timeout() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace orion {

namespace {
struct IniciarRed {
    IniciarRed() {
#ifdef _WIN32
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
#endif
    }
};
void red() { static IniciarRed r; }

void fijar_timeout_recepcion(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval t{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
}

bool enviar_todo(sock_t s, const std::string& datos) {
    size_t enviado = 0;
    while (enviado < datos.size()) {
        auto n = ::send(s, datos.data() + enviado, static_cast<int>(datos.size() - enviado), MSG_NOSIGNAL);
        if (n <= 0) return false;
        enviado += static_cast<size_t>(n);
    }
    return true;
}

std::string minus(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
}  // namespace

// ------------------------------------------------------------------ decodificador
void DecodificadorCuerpo::configurar(bool chunked, long long longitud) {
    chunked_ = chunked;
    restante_ = longitud;
    terminado_ = !chunked && longitud == 0;
    fase_ = 0;
    buf_.clear();
}

std::string DecodificadorCuerpo::alimentar(const std::string& datos) {
    std::string salida;
    if (terminado_) return salida;
    if (!chunked_) {
        if (restante_ < 0) return datos;   // hasta cerrar la conexión
        size_t n = static_cast<size_t>(std::min<long long>(restante_, static_cast<long long>(datos.size())));
        salida = datos.substr(0, n);
        restante_ -= static_cast<long long>(n);
        if (restante_ == 0) terminado_ = true;
        return salida;
    }
    buf_ += datos;
    for (;;) {
        if (fase_ == 0) {
            size_t p = buf_.find("\r\n");
            if (p == std::string::npos) break;
            std::string linea = buf_.substr(0, p);
            buf_.erase(0, p + 2);
            size_t pc = linea.find(';');       // extensiones de chunk
            if (pc != std::string::npos) linea = linea.substr(0, pc);
            restante_ = std::stoll(trim(linea).empty() ? "0" : trim(linea), nullptr, 16);
            if (restante_ == 0) { terminado_ = true; break; }
            fase_ = 1;
        } else if (fase_ == 1) {
            if (buf_.empty()) break;
            size_t n = static_cast<size_t>(std::min<long long>(restante_, static_cast<long long>(buf_.size())));
            salida += buf_.substr(0, n);
            buf_.erase(0, n);
            restante_ -= static_cast<long long>(n);
            if (restante_ == 0) fase_ = 2;
        } else {
            if (buf_.size() < 2) break;
            buf_.erase(0, 2);
            fase_ = 0;
        }
    }
    return salida;
}

// ------------------------------------------------------------------ cliente
ClienteHttp::ClienteHttp(const std::string& url_base) {
    std::string u = url_base;
    if (empieza_por(u, "http://")) u = u.substr(7);
    if (empieza_por(u, "https://")) throw std::runtime_error("https no está soportado (Ollama local usa http)");
    size_t barra = u.find('/');
    if (barra != std::string::npos) u = u.substr(0, barra);
    size_t dp = u.rfind(':');
    if (dp != std::string::npos && u.find(']') == std::string::npos) {
        host_ = u.substr(0, dp);
        puerto_ = std::stoi(u.substr(dp + 1));
    } else {
        host_ = u;
        puerto_ = 80;
    }
    if (host_.empty()) host_ = "127.0.0.1";
}

RespuestaHttp ClienteHttp::get(const std::string& ruta, int timeout_s) const { return peticion("GET", ruta, "", nullptr, nullptr, timeout_s); }
RespuestaHttp ClienteHttp::post(const std::string& ruta, const std::string& json, int timeout_s) const {
    return peticion("POST", ruta, json, nullptr, nullptr, timeout_s);
}
RespuestaHttp ClienteHttp::post_stream(const std::string& ruta, const std::string& json, const std::function<bool(const std::string&)>& por_linea,
                                       const std::atomic<bool>* cancelar, int timeout_s) const {
    return peticion("POST", ruta, json, &por_linea, cancelar, timeout_s);
}

RespuestaHttp ClienteHttp::peticion(const std::string& metodo, const std::string& ruta, const std::string& cuerpo,
                                    const std::function<bool(const std::string&)>* por_linea, const std::atomic<bool>* cancelar,
                                    int timeout_s) const {
    red();
    RespuestaHttp r;
    addrinfo pista{};
    pista.ai_family = AF_UNSPEC;
    pista.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string puerto = std::to_string(puerto_);
    if (getaddrinfo(host_.c_str(), puerto.c_str(), &pista, &res) != 0 || !res) {
        r.error = "no se pudo resolver " + host_;
        return r;
    }
    sock_t s = SOCK_INVALIDO;
    for (addrinfo* a = res; a; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == SOCK_INVALIDO) continue;
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) break;
        cerrar_socket(s);
        s = SOCK_INVALIDO;
    }
    freeaddrinfo(res);
    if (s == SOCK_INVALIDO) {
        r.error = "no se pudo conectar con " + host_ + ":" + puerto;
        return r;
    }
    fijar_timeout_recepcion(s, 200);
    std::string req = metodo + " " + ruta + " HTTP/1.1\r\nHost: " + host_ + ":" + puerto +
                      "\r\nUser-Agent: orion\r\nAccept: */*\r\nConnection: close\r\n" + extra_;
    if (metodo == "POST") req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(cuerpo.size()) + "\r\n";
    req += "\r\n" + cuerpo;
    if (!enviar_todo(s, req)) {
        cerrar_socket(s);
        r.error = "fallo al enviar la petición";
        return r;
    }

    auto limite = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    std::string crudo, linea_pend;
    bool cabeceras = false;
    DecodificadorCuerpo dec;
    char buf[8192];
    auto procesar_cuerpo = [&](const std::string& datos) -> bool {   // false = cancelar
        if (datos.empty()) return true;
        if (!por_linea || r.estado >= 400) {
            r.cuerpo += datos;
            return true;
        }
        linea_pend += datos;
        size_t p;
        while ((p = linea_pend.find('\n')) != std::string::npos) {
            std::string l = trim(linea_pend.substr(0, p));
            linea_pend.erase(0, p + 1);
            if (!l.empty() && !(*por_linea)(l)) return false;
        }
        return true;
    };
    for (;;) {
        if (cancelar && cancelar->load()) { r.cancelada = true; break; }
        if (std::chrono::steady_clock::now() > limite) { r.error = "tiempo de espera agotado"; break; }
        auto n = ::recv(s, buf, sizeof(buf), 0);
        if (n < 0) {
            if (error_es_timeout()) continue;
            r.error = "conexión interrumpida";
            break;
        }
        if (n == 0) {   // el servidor cerró
            if (!cabeceras) r.error = "respuesta vacía";
            break;
        }
        std::string datos(buf, static_cast<size_t>(n));
        if (!cabeceras) {
            crudo += datos;
            size_t fin = crudo.find("\r\n\r\n");
            if (fin == std::string::npos) continue;
            cabeceras = true;
            auto lineas = split_lines(crudo.substr(0, fin));
            if (lineas.empty() || lineas[0].size() < 12) { r.error = "respuesta HTTP no válida"; break; }
            r.estado = std::atoi(lineas[0].substr(9, 3).c_str());
            bool chunked = false;
            long long longitud = -1;
            for (size_t i = 1; i < lineas.size(); ++i) {
                size_t dp = lineas[i].find(':');
                if (dp == std::string::npos) continue;
                std::string k = minus(trim(lineas[i].substr(0, dp))), v = minus(trim(lineas[i].substr(dp + 1)));
                if (k == "transfer-encoding" && contiene(v, "chunked")) chunked = true;
                if (k == "content-length") longitud = std::atoll(v.c_str());
            }
            dec.configurar(chunked, longitud);
            datos = crudo.substr(fin + 4);
            crudo.clear();
        }
        if (!procesar_cuerpo(dec.alimentar(datos))) { r.cancelada = true; break; }
        if (dec.terminado()) break;
    }
    if (!r.cancelada && por_linea && r.estado < 400 && !trim(linea_pend).empty()) (*por_linea)(trim(linea_pend));
    cerrar_socket(s);
    return r;
}

// ------------------------------------------------------------------ servidor
ServidorHttp::ServidorHttp(Manejador m) : manejador_(std::move(m)) {}
ServidorHttp::~ServidorHttp() { parar(); }

int ServidorHttp::iniciar(int puerto, const std::string& ip) {
    red();
    sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALIDO) throw std::runtime_error("no se pudo crear el socket");
    int si = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&si), sizeof(si));
    sockaddr_in dir{};
    dir.sin_family = AF_INET;
    dir.sin_port = htons(static_cast<unsigned short>(puerto));
    inet_pton(AF_INET, ip.c_str(), &dir.sin_addr);
    if (::bind(s, reinterpret_cast<sockaddr*>(&dir), sizeof(dir)) != 0 || ::listen(s, 16) != 0) {
        cerrar_socket(s);
        throw std::runtime_error("no se pudo escuchar en " + ip + ":" + std::to_string(puerto));
    }
    socklen_t len = sizeof(dir);
    getsockname(s, reinterpret_cast<sockaddr*>(&dir), &len);
    escucha_ = static_cast<long long>(s);
    activo_ = true;
    hilo_ = std::thread([this] { bucle(); });
    return ntohs(dir.sin_port);
}

void ServidorHttp::parar() {
    if (!activo_.exchange(false)) return;
    if (hilo_.joinable()) hilo_.join();
    for (auto& c : clientes_) if (c.hilo.joinable()) c.hilo.join();
    clientes_.clear();
    cerrar_socket(static_cast<sock_t>(escucha_));
}

void ServidorHttp::bucle() {
    sock_t s = static_cast<sock_t>(escucha_);
    while (activo_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval t{0, 100000};
        for (auto it = clientes_.begin(); it != clientes_.end();) {   // recoger los hilos ya terminados
            if (it->terminado->load()) {
                if (it->hilo.joinable()) it->hilo.join();
                it = clientes_.erase(it);
            } else {
                ++it;
            }
        }
        if (select(static_cast<int>(s) + 1, &fds, nullptr, nullptr, &t) <= 0) continue;
        sock_t c = ::accept(s, nullptr, nullptr);
        if (c == SOCK_INVALIDO) continue;
        auto fin = std::make_shared<std::atomic<bool>>(false);
        clientes_.push_back({std::thread([this, c, fin] { atender(static_cast<long long>(c)); *fin = true; }), fin});
    }
}

void ServidorHttp::atender(long long sl) {
    sock_t c = static_cast<sock_t>(sl);
    fijar_timeout_recepcion(c, 200);
    std::string crudo;
    char buf[8192];
    PeticionHttp p;
    long long longitud = 0;
    size_t fin = std::string::npos;
    for (int intentos = 0; intentos < 100 && activo_;) {
        auto n = ::recv(c, buf, sizeof(buf), 0);
        if (n < 0) { if (error_es_timeout()) { ++intentos; continue; } break; }
        if (n == 0) break;
        crudo.append(buf, static_cast<size_t>(n));
        if (fin == std::string::npos) {
            fin = crudo.find("\r\n\r\n");
            if (fin != std::string::npos) {
                auto lineas = split_lines(crudo.substr(0, fin));
                auto partes = split(lineas.empty() ? "" : lineas[0], ' ');
                if (partes.size() >= 2) { p.metodo = partes[0]; p.ruta = partes[1]; }
                for (size_t i = 1; i < lineas.size(); ++i) {
                    const std::string& l = lineas[i];
                    size_t dp = l.find(':');
                    if (dp == std::string::npos) continue;
                    std::string k = minus(trim(l.substr(0, dp))), v = trim(l.substr(dp + 1));
                    p.cabeceras[k] = v;
                    if (k == "content-length") longitud = std::atoll(v.c_str());
                }
            }
        }
        if (fin != std::string::npos && static_cast<long long>(crudo.size() - fin - 4) >= longitud) break;
    }
    if (fin != std::string::npos) {
        p.cuerpo = crudo.substr(fin + 4);
        RespuestaServidor r;
        try {
            r = manejador_(p);
        } catch (const std::exception& e) {
            r.estado = 500;
            r.trozos = {std::string("{\"error\":\"") + e.what() + "\"}"};
        }
        std::string estado = std::to_string(r.estado) + (r.estado == 200 ? " OK" : " Error");
        std::string extra;
        for (auto& [k, v] : r.cabeceras) extra += k + ": " + v + "\r\n";
        if (r.trozos.size() <= 1) {
            std::string cuerpo = r.trozos.empty() ? "" : r.trozos[0];
            enviar_todo(c, "HTTP/1.1 " + estado + "\r\nContent-Type: " + r.tipo + "\r\nContent-Length: " + std::to_string(cuerpo.size()) +
                               "\r\n" + extra + "Connection: close\r\n\r\n" + cuerpo);
        } else if (enviar_todo(c, "HTTP/1.1 " + estado + "\r\nContent-Type: " + r.tipo + "\r\nTransfer-Encoding: chunked\r\n" + extra + "Connection: close\r\n\r\n")) {
            char hex[32];
            bool ok = true;
            for (auto& t : r.trozos) {
                if (r.pausa_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(r.pausa_ms));
                std::snprintf(hex, sizeof(hex), "%zx\r\n", t.size());
                if (!activo_ || !enviar_todo(c, hex + t + "\r\n")) { ok = false; break; }
            }
            if (ok) enviar_todo(c, "0\r\n\r\n");
        }
    }
    cerrar_socket(c);
}

}  // namespace orion
