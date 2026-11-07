// Cliente del LLM (Ollama) y memoria del agente.
#pragma once
#include "orion/net.hpp"

namespace orion {

struct LLMError : std::runtime_error { using std::runtime_error::runtime_error; };

class LLM {
public:
    virtual ~LLM() = default;
    virtual std::string chat(const json& messages, bool json_mode = true, int num_predict = 700, double temperature = -1) = 0;
    virtual std::vector<float> embed(const std::string& text) = 0;  // lanza LLMError si no hay embeddings
    virtual std::string model_name() const = 0;
    virtual std::string embed_model_name() const = 0;
};

class OllamaClient : public LLM {
public:
    OllamaClient(std::string base, std::string model, std::string embed_model, int num_ctx = 6144, double temperature = 0.3);
    std::string chat(const json& messages, bool json_mode = true, int num_predict = 700, double temperature = -1) override;
    std::vector<float> embed(const std::string& text) override;
    std::string model_name() const override { return model_; }
    std::string embed_model_name() const override { return embed_; }
    std::vector<std::string> list_models();
    const std::string& base() const { return base_; }
private:
    json call(const std::string& path, const json* body, int timeout, int retries);
    std::string base_, model_, embed_;
    int num_ctx_;
    double temp_;
};

// ---------------------------------------------------------------- memoria
struct MemItem {
    std::string id, ts, kind, text, sig;
    std::vector<std::string> tags;
    std::vector<float> emb;
};

std::vector<float> hash_embed(const std::string& text, int dim = 256);
float cosine(const std::vector<float>& a, const std::vector<float>& b);

class Embedder {
public:
    explicit Embedder(LLM* llm = nullptr) : llm_(llm), use_llm_(llm != nullptr) {}
    std::string sig() const { return use_llm_ ? "ollama:" + llm_->embed_model_name() : "hash256c"; }
    std::pair<std::vector<float>, std::string> embed(const std::string& text);
private:
    LLM* llm_;
    bool use_llm_;
};

class Memory {
public:
    static constexpr size_t kCoreMax = 3000;
    Memory(const fs::path& home, Embedder* emb);
    std::string core_get() const;
    std::string core_set(const std::string& text);
    std::string core_append(const std::string& line);
    std::string add(const std::string& text, const std::string& kind = "fact", const std::vector<std::string>& tags = {}, bool dedup = true);
    std::vector<std::pair<float, MemItem>> search(const std::string& query, size_t k = 5, const std::string& kind = "");
    bool forget(const std::string& id);
    void compact();
    std::string stats() const;
    size_t size() const { return items_.size(); }
    const std::map<std::string, MemItem>& items() const { return items_; }
    void log_episode(json rec);
    size_t episode_count() const;
    std::vector<json> read_episodes(size_t start = 0, size_t n = (size_t)-1) const;
    size_t rotate_episodes(size_t keep_last = 100);
    void log_dialogue(const std::string& role, const std::string& text);
    std::vector<json> recent_dialogue(size_t n = 6) const;
    Embedder& embedder() { return *emb_; }
private:
    const std::vector<float>& emb_of(const MemItem& it);
    fs::path dir_, core_, sem_, epi_, dlg_;
    Embedder* emb_;
    std::map<std::string, MemItem> items_;
    std::map<std::pair<std::string, std::string>, std::vector<float>> cache_;
};

}  // namespace orion
