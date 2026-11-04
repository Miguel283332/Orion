#include "orion/chat.hpp"

#include <algorithm>
#include <cmath>

namespace orion {

int Chat::sampleByte(const std::vector<float>& logits) {
    const int V = (int)logits.size();
    std::vector<int> idx(V);
    for (int i = 0; i < V; ++i) idx[i] = i;
    idx[0] = 0;
    const int k = std::min(std::max(1, cfg_.topK), V);
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(), [&](int a, int b) { return logits[a] > logits[b]; });
    if (cfg_.temperature <= 1e-3f) return idx[0];
    std::vector<double> w(k);
    const double mx = logits[idx[0]];
    for (int i = 0; i < k; ++i) w[i] = std::exp((logits[idx[i]] - mx) / cfg_.temperature);
    std::discrete_distribution<int> d(w.begin(), w.end());
    return idx[d(rng_)];
}

std::string Chat::continueText(const std::string& prompt, bool stopAtNewline) {
    std::string text = prompt, out;
    std::vector<float> lg;
    for (int i = 0; i < cfg_.maxBytes; ++i) {
        model_.logits(Dataset::contextOf(text, model_.genome().ctx), lg);
        lg[0] = -1e9f;  // el byte 0 es relleno, nunca se emite
        const int b = sampleByte(lg);
        const char c = (char)b;
        if (stopAtNewline && c == '\n') break;
        text.push_back(c);
        out.push_back(c);
    }
    return out;
}

std::string Chat::reply(const std::string& user) {
    std::string clean = user;
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    const std::string turn = "Usuario: " + clean + "\nOrion:";
    // el modelo solo ve los últimos `ctx` bytes, no hace falta arrastrar toda la historia
    std::string prompt = history_ + turn;
    std::string answer = continueText(prompt + " ");
    while (!answer.empty() && answer.front() == ' ') answer.erase(answer.begin());
    history_ += turn + " " + answer + "\n";
    if (history_.size() > 4096) history_.erase(0, history_.size() - 4096);
    return answer;
}

}  // namespace orion
