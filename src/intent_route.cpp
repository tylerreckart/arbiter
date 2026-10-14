// arbiter/src/intent_route.cpp — Tiered live intent routing.
//
// Cutoffs come from IntentRouteConfig (see intent_route_defaults). This
// file does not introduce a second confidence or timeout constant.
// The request path returns the cached Luna decision, otherwise the
// embedding choice, otherwise the default agent. Luna runs on a worker
// the caller does not join.

#include "intent_route.h"
#include "embed_route.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <openssl/sha.h>
#include <sstream>
#include <utility>

namespace arbiter {

namespace {

std::string lower_copy(const char* v) {
    std::string s = v ? v : "";
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_copy(std::string s) {
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool flag_on(const std::string& s) {
    return s == "luna" || s == "on" || s == "true" || s == "1";
}

bool flag_yes(const std::string& s) {
    return s == "on" || s == "true" || s == "1" || s == "yes";
}

std::string sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), digest);
    static const char* kHex = "0123456789abcdef";
    std::string out(SHA256_DIGEST_LENGTH * 2, '0');
    for (unsigned i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

constexpr std::size_t kDecisionLogCap = 128;
constexpr std::size_t kWatchCap = 64;

double env_temperature(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    double x = std::strtod(v, &end);
    if (end == v || !std::isfinite(x) || x <= 0.0 || x > 5.0) return fallback;
    return x;
}

double env_unit(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    double x = std::strtod(v, &end);
    if (end == v || !std::isfinite(x)) return fallback;
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    return x;
}

int env_int(const char* name, int fallback, int lo, int hi) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    long x = std::strtol(v, &end, 10);
    if (end == v) return fallback;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return static_cast<int>(x);
}

std::string env_string(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return trim_copy(v);
}

std::string fmt_num(double v, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

std::string normalize_text(const std::string& text, int max_chars) {
    if (max_chars < 1) max_chars = 1;
    std::string out;
    out.reserve(std::min(text.size(), static_cast<std::size_t>(max_chars)));
    bool pending_space = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            if (static_cast<int>(out.size()) >= max_chars) break;
            out.push_back(' ');
            pending_space = false;
        }
        if (static_cast<int>(out.size()) >= max_chars) break;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string one_line(const std::string& text, int max_chars) {
    std::string out;
    if (max_chars < 1) return out;
    bool pending_space = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            if (static_cast<int>(out.size()) >= max_chars) break;
            out.push_back(' ');
            pending_space = false;
        }
        if (static_cast<int>(out.size()) >= max_chars) break;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string cache_key_for(const IntentInput& in, const IntentRouteConfig& cfg) {
    std::string key = normalize_text(in.text, cfg.normalize_max_chars);
    std::vector<std::string> ids;
    ids.reserve(in.roster.size());
    for (const auto& entry : in.roster) {
        if (!entry.id.empty()) ids.push_back(entry.id);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    key.push_back('\n');
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) key.push_back(',');
        key += ids[i];
    }
    return key;
}

bool split_band(const std::string& id, std::string& agent, std::string& band) {
    const std::string cheap = "|cheap";
    const std::string strong = "|strong";
    if (id.size() > cheap.size() &&
        id.compare(id.size() - cheap.size(), cheap.size(), cheap) == 0) {
        agent = id.substr(0, id.size() - cheap.size());
        band = "cheap";
        return !agent.empty();
    }
    if (id.size() > strong.size() &&
        id.compare(id.size() - strong.size(), strong.size(), strong) == 0) {
        agent = id.substr(0, id.size() - strong.size());
        band = "strong";
        return !agent.empty();
    }
    return false;
}

struct AgentMass {
    double total = 0;
    double cheap = 0;
    double strong = 0;
};

struct Marginal {
    std::string agent;
    double probability = 0;
    double confidence = 0;
    double difficulty = 0;
    bool difficulty_known = false;
    std::size_t n_agents = 0;
};

Marginal marginalize(const DecisionResult& result) {
    Marginal out;
    std::vector<std::string> order;
    std::unordered_map<std::string, AgentMass> mass;
    double cheap_all = 0;
    double strong_all = 0;
    bool any_band = false;
    bool saw_cheap = false;
    bool saw_strong = false;
    for (const auto& [id, maybe_p] : result.distribution) {
        std::string agent;
        std::string band;
        const bool joint = split_band(id, agent, band);
        if (joint) any_band = true;
        else agent = id;
        if (agent.empty()) continue;
        if (std::find(order.begin(), order.end(), agent) == order.end())
            order.push_back(agent);
        // Unknown labels are not zeros. They stay out of the mass so a
        // single returned logprob is not renormalized to 1, and a missing
        // band is not treated as probability 0 when scoring difficulty.
        if (!maybe_p || !std::isfinite(*maybe_p)) continue;
        const double p = *maybe_p;
        if (joint) {
            if (band == "cheap") {
                mass[agent].cheap += p;
                cheap_all += p;
                saw_cheap = true;
            } else {
                mass[agent].strong += p;
                strong_all += p;
                saw_strong = true;
            }
        }
        mass[agent].total += p;
    }
    double best_p = -1;
    for (const auto& agent : order) {
        double p = mass[agent].total;
        if (p > best_p) {
            best_p = p;
            out.agent = agent;
            out.probability = p;
        }
    }
    out.n_agents = order.size();
    if (out.agent.empty() || out.n_agents == 0) return out;
    out.confidence = decision_confidence(out.probability, out.n_agents);
    if (any_band && saw_cheap && saw_strong) {
        double denom = strong_all + cheap_all;
        if (denom > 0) {
            out.difficulty = strong_all / denom;
            out.difficulty_known = true;
        }
    }
    return out;
}

std::vector<DecisionOption> build_route_options(
    const std::vector<IntentRosterEntry>& roster, int desc_chars) {
    std::vector<IntentRosterEntry> sorted = roster;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const IntentRosterEntry& a, const IntentRosterEntry& b) {
                         return a.id < b.id;
                     });
    std::vector<std::string> ids;
    std::vector<std::string> roles;
    for (const auto& entry : sorted) {
        if (entry.id.empty() || entry.id == "index") continue;
        ids.push_back(entry.id);
        roles.push_back(one_line(entry.role, desc_chars));
    }
    const bool joint = !ids.empty() && ids.size() * 2 <= kMaxLunaOptions;
    if (!joint && ids.size() > kMaxLunaOptions) {
        ids.resize(kMaxLunaOptions);
        roles.resize(kMaxLunaOptions);
    }
    std::vector<DecisionOption> out;
    if (joint) {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            for (const char* band : {"cheap", "strong"}) {
                DecisionOption opt;
                opt.id = ids[i] + "|" + band;
                opt.label = single_token_label(out.size());
                opt.description = roles[i];
                if (!opt.description.empty()) opt.description += " · ";
                opt.description += band;
                out.push_back(std::move(opt));
            }
        }
    } else {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            DecisionOption opt;
            opt.id = ids[i];
            opt.label = single_token_label(out.size());
            opt.description = roles[i];
            out.push_back(std::move(opt));
        }
    }
    return out;
}

const char* kRouteQuestion =
    "Which specialist should handle this request, and at what effort? "
    "A label ending in cheap means a lighter model; strong means a heavier model.";

void apply_model_tier(IntentRouteTimings& timings, const IntentRouteConfig& cfg) {
    if (!timings.choose_model) {
        timings.model_tier.clear();
        return;
    }
    const double score = timings.difficulty_known ? timings.difficulty
                                                  : cfg.default_difficulty;
    timings.model_tier = score >= cfg.strong_difficulty ? "strong" : "cheap";
}

void log_decision(const IntentDecisionRecord& rec) {
    Logger::global().info("intent_decision", {
        {"request_hash", rec.request_hash},
        {"embed_choice", rec.embed_choice},
        {"embed_prob", rec.embed_prob_known ? fmt_num(rec.embed_prob, 3) : std::string()},
        {"luna_choice", rec.luna_choice},
        {"luna_prob", rec.luna_prob_known ? fmt_num(rec.luna_prob, 3) : std::string()},
        {"route_ms", fmt_num(rec.route_ms, 1)},
        {"luna_ms", fmt_num(rec.luna_ms, 1)},
        {"final_agent", rec.final_agent},
        {"advisor_verdict", rec.advisor_verdict},
        {"tier", rec.tier},
    });
}

bool distribution_has_known(const DecisionResult& result) {
    for (const auto& entry : result.distribution) {
        if (entry.second && std::isfinite(*entry.second)) return true;
    }
    return false;
}

std::string agent_or_index(const std::string& id) {
    return id.empty() ? "index" : id;
}

void log_route(const IntentRouteOutcome& out) {
    Logger::global().info("intent_route", {
        {"tier", out.timings.tier},
        {"cache_hit", out.timings.cache_hit ? "true" : "false"},
        {"embed_ms", fmt_num(out.timings.embed_ms, 1)},
        {"luna_ms", fmt_num(out.timings.luna_ms, 1)},
        {"total_ms", fmt_num(out.timings.total_ms, 1)},
        {"choice", out.intent.target_agent},
        {"confidence", fmt_num(out.intent.confidence, 3)},
        {"difficulty", out.timings.difficulty_known
                           ? fmt_num(out.timings.difficulty, 3)
                           : std::string()},
        {"model_tier", out.timings.model_tier},
        {"source", out.intent.source},
        {"error", out.timings.error},
    });
}

IntentRouteOutcome finish(IntentRouteOutcome out,
                          std::chrono::steady_clock::time_point started,
                          const IntentRouteConfig& cfg,
                          IntentRouteCache* log_cache) {
    out.timings.total_ms = elapsed_ms(started);
    apply_model_tier(out.timings, cfg);
    if (log_cache && !out.request_hash.empty())
        log_cache->set_route_ms(out.request_hash, out.timings.total_ms);
    log_route(out);
    return out;
}

Intent fallback_intent(const Intent& hint, const IntentRouteConfig& cfg,
                       const std::string& error) {
    Intent out = hint;
    out.source = "fallback";
    out.confidence = 0;
    out.llm_used = false;
    out.malformed = false;
    const std::string def = cfg.default_agent.empty() ? "index" : cfg.default_agent;
    if (def == "index") out.target_agent.clear();
    else out.target_agent = def;
    if (out.target_agent.empty()) out.brief.clear();
    else if (out.brief.empty()) out.brief = "Route to " + out.target_agent;
    (void)error;
    return out;
}

void fill_from_marginal(Intent& intent, const Intent& hint, const Marginal& m,
                        const IntentRouteConfig& cfg, IntentRouteTimings& timings) {
    intent = hint;
    intent.target_agent = m.agent;
    intent.confidence = m.confidence;
    intent.llm_used = false;
    intent.malformed = false;
    if (hint.target_agent != m.agent) {
        intent.kind = "unknown";
        intent.brief = "Route to " + m.agent;
    }
    if (intent.brief.empty()) intent.brief = "Route to " + m.agent;
    timings.difficulty = m.difficulty;
    timings.difficulty_known = m.difficulty_known;
    timings.choose_model = true;
    if (m.confidence >= cfg.luna_min_confidence) {
        intent.source = "luna";
        timings.tier = "luna";
    } else {
        intent.source = "luna_guess";
        timings.tier = "luna_guess";
    }
}

struct BgCall {
    std::shared_ptr<std::atomic<bool>> cancel =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> finished =
        std::make_shared<std::atomic<bool>>(false);
    std::string key;
    std::string hash;
    IntentRouteCache* cache = nullptr;
    int capacity = 0;
    bool allow_store = false;
};

} // namespace

IntentRouteConfig intent_route_defaults() {
    IntentRouteConfig cfg;
    cfg.live = false;
    cfg.luna_min_confidence = 0.55;
    cfg.strong_difficulty = 0.5;
    cfg.default_difficulty = 0.5;
    cfg.timeout_ms = 2000;
    cfg.luna_reroute = false;
    cfg.cache_ttl_ms = 60000;
    cfg.cache_entries = 64;
    cfg.normalize_max_chars = 512;
    cfg.option_desc_chars = 80;
    cfg.default_agent = "index";
    cfg.luna_model = kLunaModelId;
    cfg.cheap_model.clear();
    cfg.strong_model.clear();
    cfg.embed_min_confidence = kEmbedDefaultMinConfidence;
    cfg.embed_temperature = kEmbedDefaultTemperature;
    return cfg;
}

IntentRouteConfig intent_route_config_from_env() {
    IntentRouteConfig cfg = intent_route_defaults();
    cfg.live = intent_live_luna_enabled();
    cfg.luna_min_confidence =
        env_unit("ARBITER_INTENT_LUNA_MIN_CONFIDENCE", cfg.luna_min_confidence);
    cfg.strong_difficulty =
        env_unit("ARBITER_INTENT_STRONG_DIFFICULTY", cfg.strong_difficulty);
    cfg.default_difficulty =
        env_unit("ARBITER_INTENT_DEFAULT_DIFFICULTY", cfg.default_difficulty);
    cfg.timeout_ms = env_int("ARBITER_INTENT_LUNA_TIMEOUT_MS", cfg.timeout_ms, 1, 600000);
    {
        const char* v = std::getenv("ARBITER_INTENT_LUNA_REROUTE");
        if (v && *v) cfg.luna_reroute = flag_yes(lower_copy(v));
    }
    cfg.cache_ttl_ms = env_int("ARBITER_INTENT_CACHE_TTL_MS", cfg.cache_ttl_ms, 0, 86400000);
    cfg.cache_entries = env_int("ARBITER_INTENT_CACHE_ENTRIES", cfg.cache_entries, 1, 10000);
    cfg.normalize_max_chars =
        env_int("ARBITER_INTENT_CACHE_KEY_CHARS", cfg.normalize_max_chars, 16, 8192);
    std::string def = env_string("ARBITER_INTENT_DEFAULT_AGENT", cfg.default_agent);
    if (!def.empty()) cfg.default_agent = def;
    std::string model = env_string("ARBITER_INTENT_LUNA_MODEL", cfg.luna_model);
    if (!model.empty()) cfg.luna_model = model;
    cfg.cheap_model = env_string("ARBITER_INTENT_CHEAP_MODEL", cfg.cheap_model);
    cfg.strong_model = env_string("ARBITER_INTENT_STRONG_MODEL", cfg.strong_model);
    cfg.embed_min_confidence =
        env_unit("ARBITER_INTENT_EMBED_MIN_CONFIDENCE", cfg.embed_min_confidence);
    cfg.embed_temperature =
        env_temperature("ARBITER_INTENT_EMBED_TEMPERATURE", cfg.embed_temperature);
    return cfg;
}

bool intent_live_luna_enabled() {
    const char* v = std::getenv("ARBITER_INTENT_LIVE");
    if (!v || !*v) return false;
    return flag_on(lower_copy(v));
}

IntentRouteCache::IntentRouteCache() = default;

IntentRouteCache::~IntentRouteCache() {
    std::vector<Worker> pending;
    {
        std::lock_guard<std::mutex> lock(workers_mu_);
        pending.swap(workers_);
    }
    for (auto& w : pending) {
        if (w.cancel) w.cancel->store(true, std::memory_order_release);
    }
    for (auto& w : pending) {
        if (w.thread.joinable()) w.thread.join();
    }
}

std::optional<DecisionResult> IntentRouteCache::lookup(const std::string& key, int ttl_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    if (ttl_ms <= 0) {
        lru_.erase(it->second.second);
        map_.erase(it);
        return std::nullopt;
    }
    const auto age = std::chrono::steady_clock::now() - it->second.first.stored;
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
    if (age_ms > ttl_ms) {
        lru_.erase(it->second.second);
        map_.erase(it);
        return std::nullopt;
    }
    lru_.splice(lru_.begin(), lru_, it->second.second);
    return it->second.first.result;
}

void IntentRouteCache::store_unlocked(const std::string& key,
                                       const DecisionResult& result,
                                       int capacity) {
    if (key.empty() || capacity < 1 || !result.ok || result.choice.empty()) return;
    auto it = map_.find(key);
    if (it != map_.end()) {
        it->second.first.result = result;
        it->second.first.stored = std::chrono::steady_clock::now();
        lru_.splice(lru_.begin(), lru_, it->second.second);
        return;
    }
    lru_.push_front(key);
    map_.emplace(key, std::make_pair(Slot{result, std::chrono::steady_clock::now()},
                                     lru_.begin()));
    while (static_cast<int>(map_.size()) > capacity && !lru_.empty()) {
        map_.erase(lru_.back());
        lru_.pop_back();
    }
}

void IntentRouteCache::store(const std::string& key, const DecisionResult& result, int capacity) {
    std::lock_guard<std::mutex> lock(mu_);
    store_unlocked(key, result, capacity);
}

void IntentRouteCache::adopt_worker(std::thread worker,
                                    std::shared_ptr<std::atomic<bool>> cancel,
                                    std::shared_ptr<std::atomic<bool>> finished) {
    std::lock_guard<std::mutex> lock(workers_mu_);
    workers_.push_back(Worker{std::move(worker), std::move(cancel), std::move(finished)});
}

void IntentRouteCache::reap() {
    std::vector<std::thread> done;
    {
        std::lock_guard<std::mutex> lock(workers_mu_);
        std::vector<Worker> still;
        still.reserve(workers_.size());
        for (auto& w : workers_) {
            if (w.finished && w.finished->load(std::memory_order_acquire)) {
                done.push_back(std::move(w.thread));
            } else {
                still.push_back(std::move(w));
            }
        }
        workers_.swap(still);
    }
    for (auto& t : done) {
        if (t.joinable()) t.join();
    }
}

void IntentRouteCache::record_decision(IntentDecisionRecord rec) {
    if (rec.request_hash.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    decisions_.push_front(std::move(rec));
    while (decisions_.size() > kDecisionLogCap) decisions_.pop_back();
}

void IntentRouteCache::note_advisor(const std::string& request_hash,
                                    const std::string& verdict) {
    IntentDecisionRecord copy;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& rec : decisions_) {
            if (rec.request_hash != request_hash) continue;
            rec.advisor_verdict = verdict;
            copy = rec;
            found = true;
            break;
        }
    }
    if (found) log_decision(copy);
}

void IntentRouteCache::set_route_ms(const std::string& request_hash, double route_ms) {
    IntentDecisionRecord copy;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& rec : decisions_) {
            if (rec.request_hash != request_hash) continue;
            rec.route_ms = route_ms;
            copy = rec;
            found = true;
            break;
        }
    }
    if (found) log_decision(copy);
}

std::optional<IntentDecisionRecord> IntentRouteCache::find_decision(
    const std::string& request_hash) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& rec : decisions_) {
        if (rec.request_hash == request_hash) return rec;
    }
    return std::nullopt;
}

std::vector<IntentDecisionRecord> IntentRouteCache::decisions() {
    std::lock_guard<std::mutex> lock(mu_);
    return std::vector<IntentDecisionRecord>(decisions_.begin(), decisions_.end());
}

void IntentRouteCache::track_luna(const std::string& request_hash,
                                  const std::string& sync_agent,
                                  const std::shared_ptr<std::atomic<bool>>& finished) {
    if (request_hash.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    LunaWatch watch;
    watch.sync_agent = sync_agent;
    watch.finished = finished;
    watches_[request_hash] = std::move(watch);
    watch_order_.push_back(request_hash);
    while (watch_order_.size() > kWatchCap) {
        const std::string old = watch_order_.front();
        auto it = watches_.find(old);
        if (it != watches_.end() && !it->second.settled) break;
        if (it != watches_.end()) watches_.erase(it);
        watch_order_.pop_front();
    }
}

void IntentRouteCache::settle_luna(const std::string& request_hash,
                                   const std::string& key,
                                   const DecisionResult* store,
                                   int capacity,
                                   const std::string& luna_choice,
                                   double luna_prob,
                                   bool luna_prob_known,
                                   double luna_ms,
                                   double confidence) {
    IntentDecisionRecord copy;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (store) store_unlocked(key, *store, capacity);
        auto it = watches_.find(request_hash);
        if (it != watches_.end()) {
            it->second.settled = true;
            it->second.ok = store != nullptr && !luna_choice.empty();
            it->second.agent = luna_choice;
            it->second.probability = luna_prob;
            it->second.probability_known = luna_prob_known;
            it->second.confidence = confidence;
        }
        for (auto& rec : decisions_) {
            if (rec.request_hash != request_hash) continue;
            rec.luna_choice = luna_choice;
            rec.luna_prob = luna_prob;
            rec.luna_prob_known = luna_prob_known;
            rec.luna_ms = luna_ms;
            copy = rec;
            found = true;
            break;
        }
    }
    if (found) log_decision(copy);
}

LunaReroute IntentRouteCache::poll_reroute(const std::string& request_hash,
                                           const IntentRouteConfig& cfg) {
    LunaReroute out;
    if (!cfg.luna_reroute || request_hash.empty()) return out;
    IntentDecisionRecord copy;
    bool applied = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = watches_.find(request_hash);
        if (it == watches_.end() || !it->second.settled || it->second.consumed)
            return out;
        LunaWatch& watch = it->second;
        if (!watch.ok || watch.agent.empty() || watch.agent == watch.sync_agent ||
            watch.confidence < cfg.luna_min_confidence)
            return out;
        watch.consumed = true;
        out.apply = true;
        out.agent = watch.agent;
        out.confidence = watch.confidence;
        out.source = "luna";
        for (auto& rec : decisions_) {
            if (rec.request_hash != request_hash) continue;
            rec.final_agent = watch.agent;
            copy = rec;
            applied = true;
            break;
        }
    }
    if (applied) log_decision(copy);
    return out;
}

bool intent_route_should_apply(const IntentConfig& cfg,
                               const Intent& intent,
                               const std::string& requested_agent,
                               bool fresh_ingress) {
    if (!fresh_ingress) return false;
    if (!cfg.apply_routing) return false;
    if (cfg.mode == "off" || cfg.mode.empty()) return false;
    const std::string req = requested_agent.empty() ? "index" : requested_agent;
    if (req != "index") return false;
    if (intent.target_agent.empty() || intent.target_agent == "index") return false;
    if (intent.source == "embed" || intent.source == "luna" ||
        intent.source == "luna_guess" || intent.source == "fallback")
        return true;
    return intent.confidence >= cfg.min_confidence;
}

std::string select_executor_model(const std::string& constitution_model,
                                  const std::string& advisor_model,
                                  double difficulty,
                                  bool difficulty_known,
                                  const IntentRouteConfig& cfg) {
    const double score = difficulty_known ? difficulty : cfg.default_difficulty;
    const bool strong = score >= cfg.strong_difficulty;
    std::string chosen = strong ? cfg.strong_model : cfg.cheap_model;
    if (chosen.empty() && strong) chosen = advisor_model;
    if (chosen.empty()) chosen = constitution_model;
    if (chosen == constitution_model) return {};
    return chosen;
}

IntentRouteOutcome route_intent_live(const IntentInput& in,
                                     const IntentConfig& intent_cfg,
                                     const IntentRouteConfig& route_cfg,
                                     IntentRouteCache& cache,
                                     const LunaDecideFn& luna,
                                     const ContextLoadFn& context_load,
                                     const EmbedDecideFn& embed) {
    (void)intent_cfg;
    const auto started = std::chrono::steady_clock::now();
    cache.reap();

    const bool explicit_specialist =
        !in.requested_agent.empty() && in.requested_agent != "index";
    if (explicit_specialist) {
        IntentRouteOutcome out;
        out.intent = heuristic_classify(in);
        out.intent.source = "explicit";
        out.intent.target_agent = in.requested_agent;
        if (out.intent.kind == "unknown") out.intent.confidence = 0;
        out.timings.tier = "explicit";
        const std::string key = cache_key_for(in, route_cfg);
        out.request_hash = sha256_hex(key);
        IntentDecisionRecord rec;
        rec.request_hash = out.request_hash;
        rec.final_agent = in.requested_agent;
        rec.tier = "explicit";
        cache.record_decision(rec);
        return finish(std::move(out), started, route_cfg, &cache);
    }

    const std::string key = cache_key_for(in, route_cfg);
    const std::string hash = sha256_hex(key);

    if (route_cfg.cache_ttl_ms > 0) {
        if (auto hit = cache.lookup(key, route_cfg.cache_ttl_ms)) {
            Marginal m = marginalize(*hit);
            if (!m.agent.empty() && hit->ok) {
                IntentRouteOutcome out;
                Intent hint = heuristic_classify(in);
                fill_from_marginal(out.intent, hint, m, route_cfg, out.timings);
                out.timings.tier = "cache";
                out.timings.cache_hit = true;
                out.timings.luna_ms = 0;
                out.request_hash = hash;
                IntentDecisionRecord rec;
                rec.request_hash = hash;
                rec.luna_choice = m.agent;
                rec.luna_prob = m.probability;
                rec.luna_prob_known = distribution_has_known(*hit);
                rec.final_agent = agent_or_index(m.agent);
                rec.tier = "cache";
                cache.record_decision(std::move(rec));
                return finish(std::move(out), started, route_cfg, &cache);
            }
        }
    }

    double embed_ms = 0;
    bool embed_scored = false;
    std::string embed_choice;
    double embed_prob = 0;
    bool embed_prob_known = false;
    double embed_confidence = 0;
    if (embed) {
        DecisionRequest request;
        request.state = in.text;
        request.question = "Which specialist should handle this request?";
        request.options = embed_options_from_roster(in.roster);
        if (request.options.size() >= 2 && !request.state.empty()) {
            DecisionResult scored;
            try {
                scored = embed(request);
            } catch (const std::exception& e) {
                scored.ok = false;
                scored.error = e.what();
            } catch (...) {
                scored.ok = false;
                scored.error = "embedding router failed";
            }
            embed_ms = scored.latency_ms;
            bool known = false;
            for (const auto& opt : request.options) {
                if (opt.id == scored.choice) known = true;
            }
            if (scored.ok && known) {
                embed_scored = true;
                embed_choice = scored.choice;
                embed_prob = scored.probability;
                embed_prob_known = true;
                embed_confidence = scored.confidence;
            }
        }
    }

    Intent hint = heuristic_classify(in);
    if (in.source_hint == "event") hint.source = "event";

    IntentRouteOutcome out;
    out.request_hash = hash;
    out.timings.embed_ms = embed_ms;
    const bool embed_confident =
        embed_scored && embed_confidence >= route_cfg.embed_min_confidence;
    if (embed_scored) {
        out.intent = hint;
        out.intent.target_agent = embed_choice;
        out.intent.confidence = embed_confidence;
        out.intent.source = "embed";
        out.intent.llm_used = false;
        out.intent.malformed = false;
        if (hint.target_agent != embed_choice) {
            out.intent.kind = "unknown";
            out.intent.brief = "Route to " + embed_choice;
        }
        if (out.intent.brief.empty())
            out.intent.brief = "Route to " + embed_choice;
        out.timings.tier = "embed";
        out.timings.choose_model = false;
    } else {
        out.intent = fallback_intent(hint, route_cfg, "embedding unavailable");
        out.timings.tier = "fallback";
        out.timings.error = "embedding unavailable";
        out.timings.choose_model = false;
    }

    const std::string sync_agent = agent_or_index(out.intent.target_agent);
    const bool want_luna = static_cast<bool>(luna) && !embed_confident;
    std::vector<DecisionOption> options;
    if (want_luna)
        options = build_route_options(in.roster, route_cfg.option_desc_chars);

    IntentDecisionRecord rec;
    rec.request_hash = hash;
    rec.tier = out.timings.tier;
    rec.final_agent = sync_agent;
    if (embed_scored) {
        rec.embed_choice = embed_choice;
        rec.embed_prob = embed_prob;
        rec.embed_prob_known = embed_prob_known;
    }
    cache.record_decision(rec);

    if (want_luna && options.size() >= 2) {
        DecisionRequest request;
        request.state = in.text;
        request.question = kRouteQuestion;
        request.options = std::move(options);

        auto state = std::make_shared<BgCall>();
        state->key = key;
        state->hash = hash;
        state->cache = &cache;
        state->capacity = route_cfg.cache_entries;
        state->allow_store = route_cfg.cache_ttl_ms > 0;
        out.used_luna = true;
        out.luna_finished = state->finished;
        cache.track_luna(hash, sync_agent, state->finished);

        const int timeout_ms = route_cfg.timeout_ms < 1 ? 1 : route_cfg.timeout_ms;
        std::thread worker([state, luna, request, timeout_ms]() {
            struct MarkFinished {
                std::shared_ptr<std::atomic<bool>> flag;
                ~MarkFinished() {
                    if (flag) flag->store(true, std::memory_order_release);
                }
            } mark{state->finished};

            const auto luna_started = std::chrono::steady_clock::now();
            const auto deadline = luna_started + std::chrono::milliseconds(timeout_ms);
            std::atomic<bool> call_done{false};
            std::thread killer([&] {
                while (!call_done.load(std::memory_order_acquire)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        state->cancel->store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
            struct StopKiller {
                std::atomic<bool>& call_done;
                std::thread& killer;
                ~StopKiller() {
                    call_done.store(true, std::memory_order_release);
                    if (killer.joinable()) killer.join();
                }
            } stop{call_done, killer};

            DecisionResult result;
            try {
                result = luna(request, *state->cancel);
            } catch (const std::exception& e) {
                result.ok = false;
                result.error = e.what();
            } catch (...) {
                result.ok = false;
                result.error = "luna decision failed";
            }

            const bool cancelled = state->cancel->load(std::memory_order_acquire);
            const bool accept = !cancelled && result.ok && !result.choice.empty();
            const double luna_ms =
                result.latency_ms > 0 ? result.latency_ms : elapsed_ms(luna_started);
            std::string choice;
            double prob = 0;
            bool prob_known = false;
            double confidence = 0;
            if (accept) {
                Marginal m = marginalize(result);
                choice = m.agent;
                prob = m.probability;
                prob_known = distribution_has_known(result);
                confidence = m.confidence;
            }
            const DecisionResult* store = nullptr;
            if (accept && !choice.empty() && state->allow_store) store = &result;
            if (state->cache) {
                state->cache->settle_luna(state->hash, state->key, store, state->capacity,
                                          choice, prob, prob_known, luna_ms, confidence);
            }
        });
        cache.adopt_worker(std::move(worker), state->cancel, state->finished);
    }

    if (context_load) {
        try {
            context_load(sync_agent);
        } catch (...) {}
    }
    return finish(std::move(out), started, route_cfg, &cache);
}

struct TurnBinding {
    IntentRouteCache* cache = nullptr;
    std::string hash;
};

thread_local TurnBinding tl_turn;

void intent_route_bind_turn(IntentRouteCache* cache, const std::string& request_hash) {
    tl_turn.cache = cache;
    tl_turn.hash = request_hash;
}

void intent_route_clear_turn() {
    tl_turn.cache = nullptr;
    tl_turn.hash.clear();
}

LunaReroute poll_luna_reroute_for_turn(const IntentRouteConfig& cfg) {
    if (!tl_turn.cache || tl_turn.hash.empty()) return {};
    return tl_turn.cache->poll_reroute(tl_turn.hash, cfg);
}

void note_intent_advisor(IntentRouteCache& cache,
                         const std::string& request_hash,
                         const std::string& verdict) {
    cache.note_advisor(request_hash, verdict);
}

void note_intent_advisor_for_turn(const std::string& verdict) {
    if (!tl_turn.cache || tl_turn.hash.empty()) return;
    note_intent_advisor(*tl_turn.cache, tl_turn.hash, verdict);
}

} // namespace arbiter
