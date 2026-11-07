// Utilidades comunes: cadenas, ficheros, procesos, SHA-256.
#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace orion {
namespace fs = std::filesystem;
using json = nlohmann::json;

std::string now_str();
std::string trim(const std::string& s);
std::string lower(std::string s);
bool starts_with(const std::string& s, const std::string& p);
bool contains(const std::string& s, const std::string& sub);
std::vector<std::string> split(const std::string& s, char sep);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
std::string clip(const std::string& s, size_t n);
size_t count_occurrences(const std::string& s, const std::string& sub);

std::string read_file(const fs::path& p);                                   // lanza si no se puede leer
void write_file(const fs::path& p, const std::string& data, bool append = false);
void write_file_atomic(const fs::path& p, const std::string& data);          // tmp + rename
std::string rand_hex(size_t n);
std::string sha256_hex(const std::string& data);
std::string sha256_file(const fs::path& p);

struct ProcResult { int code = 0; std::string out; };
ProcResult run_cmd(const std::string& cmdline);  // ejecuta con la shell y devuelve stdout (añade 2>&1 tú)
fs::path make_temp_dir(const std::string& prefix);
std::string shell_quote(const std::string& s);   // comillas válidas para sh y cmd

void say(const std::string& msg);                // imprime salvo en modo silencioso
void set_quiet(bool q);
bool is_quiet();
void sleep_ms(int ms);

// parseo tolerante de JSON (admite ``` y texto alrededor)
std::optional<json> parse_json_loose(const std::string& text);
std::string env_or(const char* name, const std::string& def = "");

}  // namespace orion
