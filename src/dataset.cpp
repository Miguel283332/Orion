#include "orion/dataset.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace orion {

namespace {
bool textual(const fs::path& p) {
    static const std::set<std::string> ok = {".txt", ".md", ".cpp", ".hpp", ".h", ".c", ".cc", ".cmake", ".sh", ".py", ".json", ".log"};
    std::string e = p.extension().string();
    if (p.filename() == "CMakeLists.txt") return true;
    return ok.count(e) > 0;
}
}  // namespace

const std::vector<uint32_t>& Dataset::pos(Split s) const {
    return s == Split::Train ? train_ : s == Split::Val ? val_ : test_;
}

void Dataset::appendBytes(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const size_t t = bytes_.size();
        bytes_.push_back(p[i]);
        const size_t c = (t / kChunk) % 20;
        (c <= 1 ? val_ : c == 10 ? test_ : train_).push_back((uint32_t)t);
    }
}

size_t Dataset::addText(const std::string& text) {
    if (text.empty()) return 0;
    appendBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    const uint8_t nl = '\n';
    appendBytes(&nl, 1);
    return text.size() + 1;
}

size_t Dataset::addPath(const std::string& path, bool recursive) {
    size_t added = 0;
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        if (recursive) {
            for (auto it = fs::recursive_directory_iterator(path, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                const auto& pth = it->path();
                if (pth.string().find("/.git") != std::string::npos || pth.string().find("/build") != std::string::npos || pth.string().find("/orion_home") != std::string::npos) continue;
                if (it->is_regular_file() && textual(pth)) files.push_back(pth);
            }
        } else {
            for (auto& e : fs::directory_iterator(path, ec))
                if (e.is_regular_file() && textual(e.path())) files.push_back(e.path());
        }
    } else if (fs::is_regular_file(path, ec)) {
        files.push_back(path);
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        const std::string key = fs::absolute(f).string();
        if (consumed_.count(key)) continue;
        consumed_[key] = 0;
    }
    const size_t before = bytes_.size();
    refresh();
    added = bytes_.size() - before;
    return added;
}

bool Dataset::refresh() {
    bool grew = false;
    for (auto& [file, done] : consumed_) {
        std::error_code ec;
        const uint64_t sz = fs::file_size(file, ec);
        if (ec || sz <= done) continue;
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        in.seekg((std::streamoff)done);
        std::string buf(sz - done, '\0');
        in.read(buf.data(), (std::streamsize)buf.size());
        buf.resize((size_t)in.gcount());
        if (done == 0 && !bytes_.empty()) {
            const uint8_t nl = '\n';
            appendBytes(&nl, 1);
        }
        appendBytes(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
        done += buf.size();
        grew = grew || !buf.empty();
    }
    return grew;
}

void Dataset::fillCtx(size_t t, int ctx, int* out) const {
    for (int c = 0; c < ctx; ++c) {
        const long long idx = (long long)t - ctx + c;
        out[c] = idx >= 0 ? bytes_[(size_t)idx] : 0;
    }
}

void Dataset::sample(Split s, int B, int ctx, std::mt19937_64& rng, std::vector<int>& ids, std::vector<int>& tgt) const {
    const auto& P = pos(s);
    if (P.empty()) throw std::runtime_error("corpus vacío para este split (hacen falta más datos)");
    ids.resize((size_t)B * ctx);
    tgt.resize(B);
    std::uniform_int_distribution<size_t> d(0, P.size() - 1);
    for (int b = 0; b < B; ++b) {
        const size_t t = P[d(rng)];
        fillCtx(t, ctx, ids.data() + (size_t)b * ctx);
        tgt[b] = bytes_[t];
    }
}

void Dataset::evalSet(Split s, int ctx, size_t maxN, std::vector<int>& ids, std::vector<int>& tgt) const {
    const auto& P = pos(s);
    const size_t n = std::min(maxN, P.size());
    ids.resize(n * ctx);
    tgt.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t t = P[(i * P.size()) / n];
        fillCtx(t, ctx, ids.data() + i * ctx);
        tgt[i] = bytes_[t];
    }
}

std::vector<int> Dataset::contextOf(const std::string& text, int ctx) {
    std::vector<int> out(ctx, 0);
    const int n = (int)text.size();
    for (int c = 0; c < ctx; ++c) {
        const int idx = n - ctx + c;
        out[c] = idx >= 0 ? (unsigned char)text[idx] : 0;
    }
    return out;
}

}  // namespace orion
