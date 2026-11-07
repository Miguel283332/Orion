// Red: HTTP mediante el ejecutable `curl` (incluido en Windows 10+ y en Linux/macOS), protegido contra SSRF.
#pragma once
#include "orion/judge.hpp"

namespace orion {

struct HttpOptions {
    std::string method = "GET";
    std::vector<std::string> headers;        // "Nombre: valor"
    fs::path body_file;                      // cuerpo crudo (PUT/POST)
    std::vector<std::pair<std::string, std::string>> form_files;  // (campo, ruta) multipart
    fs::path dest;                           // si no está vacío, guarda el cuerpo en este archivo
    size_t max_bytes = 2'000'000;
    int timeout = 30;
    bool allow_private = false;
    bool no_proxy = false;
    int max_redirects = 5;
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // claves en minúsculas
    std::string body;
    size_t size = 0;
    std::string sha256;
    std::string final_url;
};

// Lanza std::runtime_error si la URL se bloquea, curl falla o se supera max_bytes.
HttpResponse http_fetch(const std::string& url, const HttpOptions& opt);

struct SearchResult { std::string title, url, snippet; };
std::string html_to_text(const std::string& html, const std::string& base_url,
                         std::vector<std::pair<std::string, std::string>>* links = nullptr);
std::vector<SearchResult> parse_ddg(const std::string& page, int n);
std::string join_url(const std::string& base, const std::string& loc);
std::string url_encode(const std::string& s);
std::string normalize_ollama_url(const std::string& raw);
std::string wrap_untrusted(const std::string& source, const std::string& text);

}  // namespace orion
