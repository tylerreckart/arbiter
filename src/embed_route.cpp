// arbiter/src/embed_route.cpp — WordPiece, centroid softmax, asset fetch.
//
// Scoring does not know about ONNX. The session lives in embed_onnx.cpp
// (or the stub). A missing model is an error result, which the live router
// treats as "use the heuristic".

#include "embed_route.h"

#include "embed_onnx.h"
#include "embed_pins.h"
#include "logger.h"

#include <openssl/evp.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace arbiter {

namespace {

constexpr int kMaxRoutingExamples = 8;
constexpr int kMaxWordpieceChars = 100;
constexpr int kModelMaxSequence = 512;

std::string trim_cr(std::string s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

bool next_cp(const std::string& s, std::size_t& i, uint32_t& cp) {
    if (i >= s.size()) return false;
    const auto c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        cp = c0;
        ++i;
        return true;
    }
    int need = 0;
    uint32_t v = 0;
    if ((c0 & 0xE0) == 0xC0) { need = 1; v = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { need = 2; v = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { need = 3; v = c0 & 0x07; }
    else { cp = 0xFFFD; ++i; return true; }
    if (i + static_cast<std::size_t>(need) >= s.size()) {
        cp = 0xFFFD;
        ++i;
        return true;
    }
    for (int n = 1; n <= need; ++n) {
        const auto cx = static_cast<unsigned char>(s[i + static_cast<std::size_t>(n)]);
        if ((cx & 0xC0) != 0x80) {
            cp = 0xFFFD;
            ++i;
            return true;
        }
        v = (v << 6) | (cx & 0x3F);
    }
    i += static_cast<std::size_t>(need) + 1;
    cp = v;
    return true;
}

void append_cp(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool is_ws(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
           cp == 0xA0 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200A);
}

bool is_control(uint32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\r') return false;
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
}

bool is_punct(uint32_t cp) {
    if (cp <= 127) return std::ispunct(static_cast<int>(cp)) != 0;
    if (cp >= 0x2000 && cp <= 0x206F) return true;
    if (cp >= 0x3000 && cp <= 0x303F) return true;
    return false;
}

bool is_cjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF);
}

std::string clean_text(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    uint32_t cp = 0;
    while (next_cp(text, i, cp)) {
        if (cp == 0 || cp == 0xFFFD || is_control(cp)) continue;
        if (is_cjk(cp)) {
            out.push_back(' ');
            append_cp(out, cp);
            out.push_back(' ');
            continue;
        }
        if (is_ws(cp)) out.push_back(' ');
        else append_cp(out, cp);
    }
    return out;
}

std::vector<std::string> split_ws(const std::string& text) {
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : text) {
        if (ch == ' ') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 128)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::vector<std::string> split_punct(const std::string& token) {
    std::vector<std::string> out;
    std::string cur;
    std::size_t i = 0;
    uint32_t cp = 0;
    while (next_cp(token, i, cp)) {
        if (is_punct(cp)) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            std::string one;
            append_cp(one, cp);
            out.push_back(one);
        } else {
            append_cp(cur, cp);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int cp_count(const std::string& s) {
    int n = 0;
    std::size_t i = 0;
    uint32_t cp = 0;
    while (next_cp(s, i, cp)) ++n;
    return n;
}

std::vector<std::string> basic_tokens(const std::string& text) {
    auto rough = split_ws(clean_text(text));
    std::vector<std::string> out;
    for (auto& tok : rough) {
        auto lowered = lower_ascii(tok);
        auto pieces = split_punct(lowered);
        for (auto& p : pieces) {
            if (!p.empty()) out.push_back(std::move(p));
        }
    }
    return out;
}

std::vector<std::string> wordpiece_pieces(const WordPieceVocab& vocab,
                                          const std::string& token) {
    if (cp_count(token) > kMaxWordpieceChars) return {"[UNK]"};
    std::vector<uint32_t> chars;
    std::size_t i = 0;
    uint32_t cp = 0;
    while (next_cp(token, i, cp)) chars.push_back(cp);
    std::vector<std::string> subs;
    std::size_t start = 0;
    while (start < chars.size()) {
        std::size_t end = chars.size();
        std::string found;
        bool hit = false;
        while (start < end) {
            std::string sub;
            for (std::size_t k = start; k < end; ++k) append_cp(sub, chars[k]);
            if (start > 0) sub.insert(0, "##");
            if (vocab.ids.find(sub) != vocab.ids.end()) {
                found = std::move(sub);
                hit = true;
                break;
            }
            --end;
        }
        if (!hit) return {"[UNK]"};
        subs.push_back(found);
        start = end;
    }
    return subs;
}

void l2_normalize(std::vector<float>& v) {
    double sum = 0;
    for (float x : v) sum += static_cast<double>(x) * static_cast<double>(x);
    if (!(sum > 0.0) || !std::isfinite(sum)) return;
    const float inv = static_cast<float>(1.0 / std::sqrt(sum));
    for (float& x : v) x *= inv;
}

std::string fingerprint_of(const std::vector<DecisionOption>& options) {
    std::string s;
    for (const auto& opt : options) {
        s += opt.id;
        s.push_back('\n');
        s += opt.description;
        s.push_back('\n');
        for (const auto& ex : opt.examples) {
            s += ex;
            s.push_back('\n');
        }
        s.push_back('\x1e');
    }
    return s;
}

bool embed_text(const EmbedFn& embed, const std::string& text, std::vector<float>& out) {
    if (!embed) return false;
    try {
        out = embed(text);
    } catch (...) {
        out.clear();
        return false;
    }
    return !out.empty();
}

bool build_centroids(const std::vector<DecisionOption>& options,
                     const EmbedFn& embed,
                     std::vector<Centroid>& centroids,
                     std::string& error) {
    centroids.clear();
    if (!embed) {
        error = "no embedding function";
        return false;
    }
    for (const auto& opt : options) {
        if (opt.id.empty()) continue;
        std::vector<std::string> texts;
        if (!opt.description.empty()) texts.push_back(opt.description);
        for (const auto& ex : opt.examples) {
            if (!ex.empty()) texts.push_back(ex);
        }
        if (texts.empty()) texts.push_back(opt.id);
        std::vector<float> mean;
        int n = 0;
        for (const auto& text : texts) {
            std::vector<float> v;
            if (!embed_text(embed, text, v)) continue;
            l2_normalize(v);
            if (mean.empty()) mean.assign(v.size(), 0.f);
            if (v.size() != mean.size()) {
                error = "embedding dimension changed";
                return false;
            }
            for (std::size_t i = 0; i < v.size(); ++i) mean[i] += v[i];
            ++n;
        }
        if (n == 0) {
            error = "no embedding for " + opt.id;
            return false;
        }
        const float inv = 1.f / static_cast<float>(n);
        for (float& x : mean) x *= inv;
        l2_normalize(mean);
        Centroid c;
        c.id = opt.id;
        c.vector = std::move(mean);
        centroids.push_back(std::move(c));
    }
    if (centroids.size() < 2) {
        error = "need at least two specialists";
        return false;
    }
    return true;
}

double clamp_temperature(double temperature) {
    if (!std::isfinite(temperature) || temperature < 1e-4) return 1e-4;
    if (temperature > 5.0) return 5.0;
    return temperature;
}

bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

std::string sha256_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    char buf[1 << 16];
    while (in) {
        in.read(buf, sizeof(buf));
        const auto n = in.gcount();
        if (n > 0 && EVP_DigestUpdate(ctx, buf, static_cast<std::size_t>(n)) != 1) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
    }
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx, out, &len) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);
    static const char* hexd = "0123456789abcdef";
    std::string hex(static_cast<std::size_t>(len) * 2, '0');
    for (unsigned int i = 0; i < len; ++i) {
        hex[i * 2] = hexd[out[i] >> 4];
        hex[i * 2 + 1] = hexd[out[i] & 0xF];
    }
    return hex;
}

bool matches_pin(const std::string& path, const char* sha) {
    return sha256_file(path) == sha;
}

std::string cache_dir() {
    const char* home = std::getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.cache/arbiter/models/bge-small-en-v1.5";
    return "/tmp/arbiter-models/bge-small-en-v1.5";
}

bool env_disabled(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    std::string s(v);
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s != "0" && s != "false" && s != "off";
}

std::size_t write_body(char* ptr, std::size_t size, std::size_t nmemb, void* ud) {
    const auto bytes = size * nmemb;
    auto* body = static_cast<std::string*>(ud);
    if (body->size() + bytes > 80u * 1024u * 1024u) return 0;
    body->append(ptr, bytes);
    return bytes;
}

struct DownloadProgress {
    int last_pct = -1;
};

int download_progress(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t, curl_off_t) {
    if (!clientp || dltotal <= 0 || dlnow < 0) return 0;
    auto* st = static_cast<DownloadProgress*>(clientp);
    const int pct = static_cast<int>((dlnow * 100) / dltotal);
    if (pct == st->last_pct) return 0;
    st->last_pct = pct;
    std::fprintf(stderr, "\rDownloading routing model... %3d%%", pct);
    if (pct >= 100) std::fputc('\n', stderr);
    return 0;
}

bool download_verified(const char* url, const std::string& dest, const char* sha,
                       std::string& error, bool show_progress) {
    std::string body;
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl init failed";
        return false;
    }
    DownloadProgress progress;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "arbiter-embed-fetch/1.0");
    if (show_progress) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    }
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (show_progress && progress.last_pct >= 0 && progress.last_pct < 100)
        std::fputc('\n', stderr);
    if (rc != CURLE_OK) {
        error = curl_easy_strerror(rc);
        return false;
    }
    const std::string tmp = dest + ".partial";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            error = "cannot write " + tmp;
            return false;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) {
            error = "cannot write " + tmp;
            return false;
        }
    }
    if (!matches_pin(tmp, sha)) {
        std::remove(tmp.c_str());
        error = "checksum mismatch";
        return false;
    }
    if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
        std::remove(tmp.c_str());
        error = "cannot rename downloaded file";
        return false;
    }
    return true;
}

bool ensure_dir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

std::string compiled_model_path() {
#ifdef ARBITER_EMBED_MODEL_PATH
    return ARBITER_EMBED_MODEL_PATH;
#else
    return {};
#endif
}

std::string compiled_vocab_path() {
#ifdef ARBITER_EMBED_VOCAB_PATH
    return ARBITER_EMBED_VOCAB_PATH;
#else
    return {};
#endif
}

double env_temperature() {
    const char* v = std::getenv("ARBITER_INTENT_EMBED_TEMPERATURE");
    if (!v || !*v) return kEmbedDefaultTemperature;
    char* end = nullptr;
    const double x = std::strtod(v, &end);
    if (end == v || !std::isfinite(x) || x <= 0.0 || x > 5.0)
        return kEmbedDefaultTemperature;
    return x;
}

std::string lower_copy(const char* v) {
    std::string s = v ? v : "";
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool wordpiece_load_lines(WordPieceVocab& vocab, const std::vector<std::string>& lines) {
    vocab = WordPieceVocab{};
    int id = 0;
    for (const auto& raw : lines) {
        vocab.ids.emplace(trim_cr(raw), id);
        ++id;
    }
    auto take = [&](const char* tok, int& slot) {
        auto it = vocab.ids.find(tok);
        if (it == vocab.ids.end()) return false;
        slot = it->second;
        return true;
    };
    return take("[CLS]", vocab.cls_id) && take("[SEP]", vocab.sep_id) &&
           take("[UNK]", vocab.unk_id) && take("[PAD]", vocab.pad_id);
}

bool wordpiece_load_file(WordPieceVocab& vocab, const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    if (!lines.empty() && lines.front().size() >= 3 &&
        static_cast<unsigned char>(lines.front()[0]) == 0xEF) {
        lines.front().erase(0, 3);
    }
    return wordpiece_load_lines(vocab, lines);
}

std::vector<int64_t> wordpiece_encode(const WordPieceVocab& vocab,
                                      const std::string& text,
                                      int max_len) {
    std::vector<int64_t> ids;
    if (vocab.cls_id < 0 || vocab.sep_id < 0 || vocab.unk_id < 0) return ids;
    if (max_len < 2) max_len = 2;
    if (max_len > kModelMaxSequence) max_len = kModelMaxSequence;
    ids.push_back(vocab.cls_id);
    for (const auto& tok : basic_tokens(text)) {
        for (const auto& piece : wordpiece_pieces(vocab, tok)) {
            if (static_cast<int>(ids.size()) + 1 >= max_len) break;
            auto it = vocab.ids.find(piece);
            ids.push_back(it == vocab.ids.end() ? vocab.unk_id : it->second);
        }
        if (static_cast<int>(ids.size()) + 1 >= max_len) break;
    }
    ids.push_back(vocab.sep_id);
    return ids;
}

DecisionResult score_embedding_centroids(const std::vector<float>& query,
                                         const std::vector<Centroid>& centroids,
                                         double temperature) {
    DecisionResult result;
    result.backend = "embed-centroid";
    result.requested_model = "BAAI/bge-small-en-v1.5";
    result.model = result.requested_model;
    if (query.empty() || centroids.empty()) {
        result.error = "empty embedding";
        return result;
    }
    std::vector<float> q = query;
    l2_normalize(q);
    const double temp = clamp_temperature(temperature);
    std::vector<double> sims;
    sims.reserve(centroids.size());
    double max_sim = -1e300;
    for (const auto& c : centroids) {
        if (c.vector.size() != q.size() || c.id.empty()) {
            result.error = "centroid dimension mismatch";
            return result;
        }
        double dot = 0;
        std::vector<float> v = c.vector;
        l2_normalize(v);
        for (std::size_t i = 0; i < q.size(); ++i)
            dot += static_cast<double>(q[i]) * static_cast<double>(v[i]);
        sims.push_back(dot);
        if (dot > max_sim) max_sim = dot;
    }
    std::vector<double> weights;
    weights.reserve(sims.size());
    double sum = 0;
    for (double s : sims) {
        const double w = std::exp((s - max_sim) / temp);
        weights.push_back(w);
        sum += w;
    }
    if (!(sum > 0.0) || !std::isfinite(sum)) {
        result.error = "softmax failed";
        return result;
    }
    std::size_t winner = 0;
    result.distribution.reserve(centroids.size());
    for (std::size_t i = 0; i < centroids.size(); ++i) {
        const double p = weights[i] / sum;
        result.distribution.emplace_back(centroids[i].id, p);
        if (p > *result.distribution[winner].second) winner = i;
    }
    result.ok = true;
    result.choice = centroids[winner].id;
    result.probability = *result.distribution[winner].second;
    result.confidence = decision_confidence(result.probability, centroids.size());
    result.labels_observed = static_cast<int>(centroids.size());
    std::ostringstream note;
    note.setf(std::ios::fixed);
    note.precision(3);
    note << "nearest centroid, softmax temperature " << temp;
    result.note = note.str();
    return result;
}

void EmbeddingCentroidBackend::set_embed(EmbedFn fn) { embed_ = std::move(fn); }

void EmbeddingCentroidBackend::set_temperature(double temperature) {
    temperature_ = clamp_temperature(temperature);
}

bool EmbeddingCentroidBackend::ensure_centroids(const std::vector<DecisionOption>& options,
                                               std::string& error) {
    const std::string fp = fingerprint_of(options);
    if (fp == fingerprint_ && centroids_.size() >= 2) return true;
    std::vector<Centroid> built;
    if (!build_centroids(options, embed_, built, error)) return false;
    fingerprint_ = fp;
    centroids_ = std::move(built);
    return true;
}

bool EmbeddingCentroidBackend::precompute(const std::vector<DecisionOption>& options,
                                         std::string& error) {
    return ensure_centroids(options, error);
}

DecisionResult EmbeddingCentroidBackend::decide(const DecisionRequest& request) {
    const auto started = std::chrono::steady_clock::now();
    DecisionResult result;
    result.backend = "embed-centroid";
    result.requested_model = "BAAI/bge-small-en-v1.5";
    std::string error;
    if (request.state.empty()) {
        result.error = "empty request";
    } else if (!ensure_centroids(request.options, error)) {
        result.error = error.empty() ? "centroids unavailable" : error;
    } else {
        std::vector<float> query;
        if (!embed_text(embed_, request.state, query)) {
            result.error = "query embedding failed";
        } else {
            result = score_embedding_centroids(query, centroids_, temperature_);
        }
    }
    result.latency_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    return result;
}

EmbeddingRouter& EmbeddingRouter::instance() {
    static EmbeddingRouter router;
    return router;
}

bool EmbeddingRouter::prepare() {
    std::lock_guard<std::mutex> lock(mu_);
    if (OnnxTextEmbedder::instance().ready()) return true;
    if (!OnnxTextEmbedder::instance().prepare()) {
        error_ = OnnxTextEmbedder::instance().last_error();
        if (error_.empty()) error_ = "embedding model unavailable";
        return false;
    }
    error_.clear();
    return true;
}

bool EmbeddingRouter::ready() const {
    return OnnxTextEmbedder::instance().ready();
}

const std::string& EmbeddingRouter::last_error() const {
    return error_;
}

void EmbeddingRouter::precompute(const std::vector<DecisionOption>& options) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!OnnxTextEmbedder::instance().ready()) return;
    if (!embed_set_) {
        backend_.set_embed([](const std::string& text) {
            return OnnxTextEmbedder::instance().embed(text);
        });
        embed_set_ = true;
    }
    std::string error;
    if (!backend_.precompute(options, error)) error_ = error;
}

DecisionResult EmbeddingRouter::decide(const DecisionRequest& request, double temperature) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!OnnxTextEmbedder::instance().ready()) {
        DecisionResult result;
        result.backend = "embed-centroid";
        result.error = error_.empty() ? OnnxTextEmbedder::instance().last_error()
                                      : error_;
        if (result.error.empty()) result.error = "embedding model unavailable";
        return result;
    }
    if (!embed_set_) {
        backend_.set_embed([](const std::string& text) {
            return OnnxTextEmbedder::instance().embed(text);
        });
        embed_set_ = true;
    }
    backend_.set_temperature(temperature);
    return backend_.decide(request);
}

std::vector<DecisionOption> embed_options_from_roster(
    const std::vector<IntentRosterEntry>& roster) {
    std::vector<IntentRosterEntry> sorted;
    sorted.reserve(roster.size());
    for (const auto& entry : roster) {
        if (entry.id.empty() || entry.id == "index") continue;
        sorted.push_back(entry);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const IntentRosterEntry& a, const IntentRosterEntry& b) {
                  return a.id < b.id;
              });
    std::vector<DecisionOption> out;
    out.reserve(sorted.size());
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const auto& entry = sorted[i];
        DecisionOption opt;
        opt.id = entry.id;
        opt.label = single_token_label(i);
        if (!entry.role.empty()) opt.description = entry.role;
        if (!entry.goal.empty()) {
            if (!opt.description.empty()) opt.description += ". ";
            opt.description += entry.goal;
        }
        for (const auto& ex : entry.routing_examples) {
            if (ex.empty()) continue;
            if (static_cast<int>(opt.examples.size()) >= kMaxRoutingExamples) break;
            opt.examples.push_back(ex);
        }
        out.push_back(std::move(opt));
    }
    return out;
}

EmbedAssetPaths resolve_embed_assets() {
    EmbedAssetPaths out;
    const char* env_model = std::getenv("ARBITER_EMBED_MODEL");
    const char* env_vocab = std::getenv("ARBITER_EMBED_VOCAB");
    const bool custom = (env_model && *env_model) || (env_vocab && *env_vocab);
    if (custom) {
        if (!(env_model && *env_model) || !(env_vocab && *env_vocab)) {
            out.error = "set both ARBITER_EMBED_MODEL and ARBITER_EMBED_VOCAB";
            return out;
        }
        if (!file_exists(env_model) || !file_exists(env_vocab)) {
            out.error = "embedding model path does not exist";
            return out;
        }
        out.model = env_model;
        out.vocab = env_vocab;
        return out;
    }

    const std::string compiled_model = compiled_model_path();
    const std::string compiled_vocab = compiled_vocab_path();
    if (matches_pin(compiled_model, kEmbedModelSha256) &&
        matches_pin(compiled_vocab, kEmbedVocabSha256)) {
        out.model = compiled_model;
        out.vocab = compiled_vocab;
        return out;
    }

    const std::string dir = cache_dir();
    const std::string model = dir + "/model_quantized.onnx";
    const std::string vocab = dir + "/vocab.txt";
    if (matches_pin(model, kEmbedModelSha256) && matches_pin(vocab, kEmbedVocabSha256)) {
        out.model = model;
        out.vocab = vocab;
        return out;
    }
    if (env_disabled("ARBITER_OFFLINE") || env_disabled("ARBITER_EMBED_NO_FETCH")) {
        static std::atomic<bool> noted{false};
        if (!noted.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                "Routing model is not installed and downloads are disabled. "
                "Using the heuristic router.\n");
        }
        out.error = "embedding model absent and fetch is disabled";
        return out;
    }
    if (!ensure_dir(dir)) {
        out.error = "cannot create embedding cache directory";
        return out;
    }
    std::string error;
    if (!matches_pin(model, kEmbedModelSha256)) {
        std::fprintf(stderr,
            "Downloading routing model (bge-small-en-v1.5, 34 MB). "
            "This is stored under ~/.cache/arbiter and happens once.\n");
        if (!download_verified(kEmbedModelUrl, model, kEmbedModelSha256, error, true)) {
            std::fprintf(stderr,
                "Could not download the routing model (%s). Using the heuristic router.\n",
                error.empty() ? "download failed" : error.c_str());
            out.error = error.empty() ? "model download failed" : error;
            return out;
        }
    }
    if (!matches_pin(vocab, kEmbedVocabSha256) &&
        !download_verified(kEmbedVocabUrl, vocab, kEmbedVocabSha256, error, false)) {
        std::fprintf(stderr,
            "Could not download the routing model (%s). Using the heuristic router.\n",
            error.empty() ? "vocab download failed" : error.c_str());
        out.error = error.empty() ? "vocab download failed" : error;
        return out;
    }
    out.model = model;
    out.vocab = vocab;
    return out;
}

bool intent_shadow_embed_enabled() {
    const char* v = std::getenv("ARBITER_INTENT_SHADOW");
    if (!v || !*v) return false;
    return lower_copy(v) == "embed";
}

void shadow_embed_decision(const std::string& text,
                           const std::vector<IntentRosterEntry>& roster,
                           const Intent& alongside) {
    if (!intent_shadow_embed_enabled()) return;
    try {
        auto& router = EmbeddingRouter::instance();
        if (!router.prepare()) {
            Logger::global().info("intent_shadow", {
                {"backend", "embed-centroid"},
                {"skipped", "model_unavailable"},
                {"error", router.last_error()},
                {"heuristic_agent", alongside.target_agent},
            });
            return;
        }
        DecisionRequest req;
        req.state = text;
        req.question = "Which specialist should handle this request?";
        req.options = embed_options_from_roster(roster);
        if (req.options.size() < 2) {
            Logger::global().info("intent_shadow", {
                {"backend", "embed-centroid"},
                {"skipped", "roster"},
                {"heuristic_agent", alongside.target_agent},
            });
            return;
        }
        router.precompute(req.options);
        DecisionResult result = router.decide(req, env_temperature());
        std::ostringstream conf;
        conf.setf(std::ios::fixed);
        conf.precision(3);
        conf << result.confidence;
        std::ostringstream ms;
        ms.setf(std::ios::fixed);
        ms.precision(1);
        ms << result.latency_ms;
        Logger::global().info("intent_shadow", {
            {"backend", "embed-centroid"},
            {"choice", result.choice},
            {"confidence", conf.str()},
            {"latency_ms", ms.str()},
            {"ok", result.ok ? "true" : "false"},
            {"error", result.error},
            {"heuristic_kind", alongside.kind},
            {"heuristic_agent", alongside.target_agent},
        });
    } catch (...) {}
}

} // namespace arbiter
