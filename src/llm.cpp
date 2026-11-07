#include "orion/llm.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace orion {

OllamaClient::OllamaClient(std::string base, std::string model, std::string embed_model, int num_ctx, double temperature)
    : base_(normalize_ollama_url(base)), model_(std::move(model)), embed_(std::move(embed_model)), num_ctx_(num_ctx), temp_(temperature) {}

json OllamaClient::call(const std::string& path, const json* body, int timeout, int retries) {
    std::string last;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        fs::path tmp;
        try {
            HttpOptions o;
            o.allow_private = true;  // Ollama es local: no aplica la protección SSRF
            o.no_proxy = true;
            o.timeout = timeout;
            o.max_bytes = 200'000'000;
            if (body) {
                o.method = "POST";
                o.headers = {"Content-Type: application/json"};
                tmp = fs::temp_directory_path() / ("orion_req_" + rand_hex(10) + ".json");
                write_file(tmp, body->dump(-1, ' ', false, json::error_handler_t::replace));
                o.body_file = tmp;
            }
            HttpResponse r = http_fetch(base_ + path, o);
            std::error_code ec;
            if (!tmp.empty()) fs::remove(tmp, ec);
            if (r.status != 200) throw LLMError("Ollama respondió HTTP " + std::to_string(r.status) + " en " + path + ": " + clip(r.body, 300));
            json j = json::parse(r.body, nullptr, false);
            if (j.is_discarded()) throw LLMError("Ollama devolvió una respuesta que no es JSON");
            return j;
        } catch (const LLMError&) {
            if (!tmp.empty()) { std::error_code ec; fs::remove(tmp, ec); }
            throw;
        } catch (const std::exception& e) {
            if (!tmp.empty()) { std::error_code ec; fs::remove(tmp, ec); }
            last = e.what();
            if (attempt < retries) sleep_ms(1500 * (attempt + 1));
        }
    }
    throw LLMError("No se puede hablar con Ollama en " + base_ + " (" + last + "). ¿Está en marcha? Prueba:  ollama serve");
}

std::string OllamaClient::chat(const json& messages, bool json_mode, int num_predict, double temperature) {
    json body = {{"model", model_}, {"messages", messages}, {"stream", false},
                 {"options", {{"num_ctx", num_ctx_}, {"num_predict", num_predict}, {"temperature", temperature < 0 ? temp_ : temperature}}}};
    if (json_mode) body["format"] = "json";
    json out = call("/api/chat", &body, 900, 2);
    if (!out.contains("message") || !out["message"].is_object()) return "";
    return out["message"].value("content", "");
}

std::vector<float> OllamaClient::embed(const std::string& text) {
    json body = {{"model", embed_}, {"input", json::array({text.substr(0, 2000)})}};
    json out = call("/api/embed", &body, 120, 1);
    if (!out.contains("embeddings") || !out["embeddings"].is_array() || out["embeddings"].empty()) throw LLMError("Ollama no devolvió embeddings");
    return out["embeddings"][0].get<std::vector<float>>();
}

std::vector<std::string> OllamaClient::list_models() {
    json out = call("/api/tags", nullptr, 10, 0);
    std::vector<std::string> names;
    if (out.contains("models") && out["models"].is_array())
        for (auto& m : out["models"]) names.push_back(m.value("name", ""));
    return names;
}

// ---------------------------------------------------------------- memoria
std::vector<float> hash_embed(const std::string& text, int dim) {
    std::vector<std::string> toks;
    std::string cur;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80 || c == '_') cur.push_back((char)std::tolower(c));
        else if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) toks.push_back(cur);
    std::vector<std::string> feats = toks;
    for (size_t i = 0; i + 1 < toks.size(); ++i) feats.push_back(toks[i] + "_" + toks[i + 1]);
    std::vector<float> v(dim, 0.0f);
    for (auto& f : feats) {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : f) { h ^= c; h *= 1099511628211ULL; }
        v[h % dim] += ((h >> 40) & 1) ? 1.0f : -1.0f;
    }
    double n = 0;
    for (float x : v) n += (double)x * x;
    n = std::sqrt(n);
    if (n == 0) n = 1;
    for (auto& x : v) x = (float)(x / n);
    return v;
}

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return -1.0f;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) { dot += (double)a[i] * b[i]; na += (double)a[i] * a[i]; nb += (double)b[i] * b[i]; }
    if (na == 0 || nb == 0) return 0.0f;
    return (float)(dot / (std::sqrt(na) * std::sqrt(nb)));
}

std::pair<std::vector<float>, std::string> Embedder::embed(const std::string& text) {
    if (use_llm_) {
        try { return {llm_->embed(text), sig()}; } catch (const std::exception&) { use_llm_ = false; }
    }
    return {hash_embed(text), sig()};
}

static json item_json(const MemItem& it) {
    json e = json::array();
    for (float x : it.emb) e.push_back(std::round(x * 100000.0f) / 100000.0f);
    return {{"id", it.id}, {"ts", it.ts}, {"kind", it.kind}, {"tags", it.tags}, {"text", it.text}, {"emb", e}, {"sig", it.sig}};
}

Memory::Memory(const fs::path& home, Embedder* emb) : emb_(emb) {
    dir_ = home / "memory";
    fs::create_directories(dir_);
    core_ = dir_ / "core.md"; sem_ = dir_ / "semantic.jsonl"; epi_ = dir_ / "episodes.jsonl"; dlg_ = dir_ / "dialogue.jsonl";
    std::ifstream in(sem_);
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("id")) continue;
        if (j.value("deleted", false)) { items_.erase(j["id"].get<std::string>()); continue; }
        MemItem it;
        it.id = j["id"]; it.ts = j.value("ts", ""); it.kind = j.value("kind", "fact"); it.text = j.value("text", ""); it.sig = j.value("sig", "");
        if (j.contains("tags") && j["tags"].is_array()) for (auto& t : j["tags"]) if (t.is_string()) it.tags.push_back(t);
        if (j.contains("emb") && j["emb"].is_array()) for (auto& x : j["emb"]) it.emb.push_back(x.get<float>());
        items_[it.id] = it;
    }
}

std::string Memory::core_get() const {
    std::error_code ec;
    return fs::exists(core_, ec) ? read_file(core_) : "";
}
std::string Memory::core_set(const std::string& text) {
    std::string t = trim(text);
    if (t.size() > kCoreMax) throw std::runtime_error("el núcleo admite " + std::to_string(kCoreMax) + " caracteres; resume y vuelve a intentarlo");
    write_file(core_, t + "\n");
    return "núcleo actualizado (" + std::to_string(t.size()) + " caracteres)";
}
std::string Memory::core_append(const std::string& line) { return core_set(trim(core_get()) + "\n" + trim(line)); }

const std::vector<float>& Memory::emb_of(const MemItem& it) {
    if (it.sig == emb_->sig()) return it.emb;
    auto key = std::make_pair(it.id, emb_->sig());
    auto f = cache_.find(key);
    if (f == cache_.end()) f = cache_.emplace(key, emb_->embed(it.text).first).first;
    return f->second;
}

std::string Memory::add(const std::string& text_in, const std::string& kind, const std::vector<std::string>& tags, bool dedup) {
    std::string text = trim(text_in).substr(0, 2000);
    if (text.empty()) throw std::runtime_error("texto vacío");
    auto [emb, sig] = emb_->embed(text);
    if (dedup) {
        for (auto& [id, it] : items_) {
            if (it.kind != kind || it.sig != sig) continue;
            if (cosine(emb, it.emb) > 0.97f) { it.ts = now_str(); write_file(sem_, item_json(it).dump() + "\n", true); return it.id; }
        }
    }
    MemItem it;
    it.id = rand_hex(8); it.ts = now_str(); it.kind = kind; it.tags = tags; it.text = text; it.emb = emb; it.sig = sig;
    items_[it.id] = it;
    write_file(sem_, item_json(it).dump(-1, ' ', false, json::error_handler_t::replace) + "\n", true);
    return it.id;
}

std::vector<std::pair<float, MemItem>> Memory::search(const std::string& query, size_t k, const std::string& kind) {
    std::vector<std::pair<float, MemItem>> out;
    if (items_.empty()) return out;
    auto [q, sig] = emb_->embed(query);
    for (auto& [id, it] : items_) {
        if (!kind.empty() && it.kind != kind) continue;
        const auto& e = it.sig == sig ? it.emb : emb_of(it);
        if (e.size() != q.size()) continue;
        float s = cosine(q, e);
        if (it.kind == "lesson" || it.kind == "skill" || it.kind == "rule") s += 0.03f;
        out.emplace_back(s, it);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (out.size() > k) out.resize(k);
    return out;
}

bool Memory::forget(const std::string& id) {
    if (!items_.erase(id)) return false;
    write_file(sem_, json{{"id", id}, {"deleted", true}, {"ts", now_str()}}.dump() + "\n", true);
    return true;
}

void Memory::compact() {
    std::string all;
    for (auto& [id, it] : items_) all += item_json(it).dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    write_file_atomic(sem_, all);
}

std::string Memory::stats() const {
    std::map<std::string, int> kinds;
    for (auto& [id, it] : items_) kinds[it.kind]++;
    std::string ks;
    for (auto& [k, n] : kinds) ks += (ks.empty() ? "" : ", ") + k + "=" + std::to_string(n);
    return "núcleo: " + std::to_string(core_get().size()) + "/" + std::to_string(kCoreMax) + " caracteres | archivo: " + std::to_string(items_.size()) +
           " entradas {" + ks + "} | episodios: " + std::to_string(episode_count()) + " | embeddings: " + emb_->sig();
}

void Memory::log_episode(json rec) {
    rec["ts"] = now_str();
    write_file(epi_, rec.dump(-1, ' ', false, json::error_handler_t::replace) + "\n", true);
}
size_t Memory::episode_count() const {
    std::ifstream in(epi_);
    size_t n = 0;
    std::string l;
    while (std::getline(in, l)) ++n;
    return n;
}
std::vector<json> Memory::read_episodes(size_t start, size_t n) const {
    std::vector<json> out;
    std::ifstream in(epi_);
    std::string l;
    size_t i = 0;
    while (std::getline(in, l)) {
        if (i++ < start) continue;
        if (out.size() >= n) break;
        json j = json::parse(l, nullptr, false);
        if (!j.is_discarded()) out.push_back(j);
    }
    return out;
}
size_t Memory::rotate_episodes(size_t keep_last) {
    auto recs = read_episodes();
    if (recs.size() <= keep_last) return 0;
    size_t old = recs.size() - keep_last;
    std::string arch, keep;
    for (size_t i = 0; i < recs.size(); ++i) (i < old ? arch : keep) += recs[i].dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    write_file(dir_ / "episodes_archive.jsonl", arch, true);
    write_file_atomic(epi_, keep);
    return old;
}
void Memory::log_dialogue(const std::string& role, const std::string& text) {
    write_file(dlg_, json{{"ts", now_str()}, {"role", role}, {"text", text.substr(0, 2000)}}.dump(-1, ' ', false, json::error_handler_t::replace) + "\n", true);
}
std::vector<json> Memory::recent_dialogue(size_t n) const {
    std::vector<json> all;
    std::ifstream in(dlg_);
    std::string l;
    while (std::getline(in, l)) { json j = json::parse(l, nullptr, false); if (!j.is_discarded()) all.push_back(j); }
    if (all.size() > n) all.erase(all.begin(), all.end() - (long)n);
    return all;
}

}  // namespace orion
