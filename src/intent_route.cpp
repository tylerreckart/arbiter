// arbiter/src/intent_route.cpp — Tiered live intent routing.
//
// Cutoffs come from IntentRouteConfig (see intent_route_defaults). This
// file does not introduce a second confidence or timeout constant.

#include "intent_route.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
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

void log_route(const IntentRouteOutcome& out) {
    Logger::global().info("intent_route", {
        {"tier", out.timings.tier},
        {"cache_hit", out.timings.cache_hit ? "true" : "false"},
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
                          const IntentRouteConfig& cfg) {
    out.timings.total_ms = elapsed_ms(started);
    apply_model_tier(out.timings, cfg);
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

struct CallState {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    DecisionResult result;
    std::shared_ptr<std::atomic<bool>> cancel =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> finished =
        std::make_shared<std::atomic<bool>>(false);
    std::string key;
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
    cfg.timeout_ms = 400;
    cfg.cache_ttl_ms = 60000;
    cfg.cache_entries = 64;
    cfg.normalize_max_chars = 512;
    cfg.option_desc_chars = 80;
    cfg.default_agent = "index";
    cfg.luna_model = kLunaModelId;
    cfg.cheap_model.clear();
    cfg.strong_model.clear();
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

void IntentRouteCache::store(const std::string& key, const DecisionResult& result, int capacity) {
    if (key.empty() || capacity < 1 || !result.ok || result.choice.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
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
    if (intent.source == "luna" || intent.source == "luna_guess" ||
        intent.source == "fallback")
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
                                     const ContextLoadFn& context_load) {
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
        return finish(std::move(out), started, route_cfg);
    }

    const std::string key = cache_key_for(in, route_cfg);
    if (route_cfg.cache_ttl_ms > 0) {
        if (auto hit = cache.lookup(key, route_cfg.cache_ttl_ms)) {
            Marginal m = marginalize(*hit);
            IntentRouteOutcome out;
            Intent hint = heuristic_classify(in);
            if (!m.agent.empty() && hit->ok) {
                fill_from_marginal(out.intent, hint, m, route_cfg, out.timings);
                out.timings.tier = "cache";
                out.timings.cache_hit = true;
                out.timings.luna_ms = 0;
                return finish(std::move(out), started, route_cfg);
            }
        }
    }

    Intent hint = heuristic_classify(in);
    if (in.source_hint == "event") hint.source = "event";
    const bool heuristic_confident =
        !hint.target_agent.empty() &&
        hint.confidence >= intent_cfg.min_confidence;
    if (heuristic_confident) {
        IntentRouteOutcome out;
        out.intent = hint;
        out.timings.tier = "heuristic";
        return finish(std::move(out), started, route_cfg);
    }

    auto give_up = [&](IntentRouteOutcome out, const std::string& error) {
        out.intent = fallback_intent(hint, route_cfg, error);
        out.timings.tier = "fallback";
        out.timings.error = error;
        return finish(std::move(out), started, route_cfg);
    };

    if (!luna) {
        IntentRouteOutcome out;
        return give_up(std::move(out), "no luna backend");
    }

    const auto options = build_route_options(in.roster, route_cfg.option_desc_chars);
    if (options.size() < 2) {
        IntentRouteOutcome out;
        return give_up(std::move(out), "need at least two specialists");
    }

    DecisionRequest request;
    request.state = in.text;
    request.question = kRouteQuestion;
    request.options = options;

    auto state = std::make_shared<CallState>();
    state->key = key;
    state->cache = &cache;
    state->capacity = route_cfg.cache_entries;
    state->allow_store = route_cfg.cache_ttl_ms > 0;

    const auto luna_started = std::chrono::steady_clock::now();
    std::thread worker([state, luna, request]() {
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
        {
            std::lock_guard<std::mutex> lock(state->mu);
            state->result = result;
            state->done = true;
        }
        state->cv.notify_all();
        if (state->allow_store && result.ok && !result.choice.empty() && state->cache)
            state->cache->store(state->key, result, state->capacity);
        if (state->finished)
            state->finished->store(true, std::memory_order_release);
    });

    std::string speculative = !hint.target_agent.empty() ? hint.target_agent
                                                         : route_cfg.default_agent;
    if (speculative.empty()) speculative = "index";
    if (context_load) {
        try {
            context_load(speculative);
        } catch (...) {}
    }

    const auto deadline = luna_started + std::chrono::milliseconds(
        route_cfg.timeout_ms < 1 ? 1 : route_cfg.timeout_ms);
    bool ready = false;
    {
        std::unique_lock<std::mutex> lock(state->mu);
        if (state->done) {
            ready = true;
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (now < deadline) {
                ready = state->cv.wait_for(lock, deadline - now, [&] { return state->done; });
            }
        }
    }

    if (!ready) {
        state->cancel->store(true, std::memory_order_release);
        const double luna_ms = elapsed_ms(luna_started);
        cache.adopt_worker(std::move(worker), state->cancel, state->finished);
        IntentRouteOutcome out;
        out.used_luna = true;
        out.timings.luna_ms = luna_ms;
        return give_up(std::move(out), "luna timeout");
    }

    DecisionResult result = state->result;
    if (worker.joinable()) worker.join();
    const double luna_ms = result.latency_ms > 0 ? result.latency_ms : elapsed_ms(luna_started);

    IntentRouteOutcome out;
    out.used_luna = true;
    out.timings.luna_ms = luna_ms;
    if (!result.ok || result.choice.empty()) {
        const std::string error = result.error.empty() ? "luna returned no choice" : result.error;
        return give_up(std::move(out), error);
    }
    Marginal m = marginalize(result);
    if (m.agent.empty()) {
        return give_up(std::move(out), "luna distribution had no agent");
    }
    fill_from_marginal(out.intent, hint, m, route_cfg, out.timings);
    if (route_cfg.cache_ttl_ms > 0)
        cache.store(key, result, route_cfg.cache_entries);
    return finish(std::move(out), started, route_cfg);
}

} // namespace arbiter
