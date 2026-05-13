// arbiter/src/metrics.cpp

#include "metrics.h"

#include <sstream>

namespace arbiter {

namespace {

// Escape a label value per Prometheus text format: backslash, double
// quote, and newline are escaped; other bytes pass through.  Used only
// for the `route` and `reason` strings — tenant id is numeric, provider
// names are lowercase ASCII, so most labels don't need this.
std::string escape_label(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 4);
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            default:   out += c;
        }
    }
    return out;
}

// Build a Prometheus label block "{k1=\"v1\",k2=\"v2\"}" from a list
// of (key, value) pairs.  An empty list yields the empty string so
// metrics without labels stay compact.
std::string make_labels(
    std::initializer_list<std::pair<const char*, std::string>> kvs) {
    if (kvs.size() == 0) return {};
    std::string out = "{";
    bool first = true;
    for (auto& kv : kvs) {
        if (!first) out += ',';
        first = false;
        out += kv.first;
        out += "=\"";
        out += escape_label(kv.second);
        out += '"';
    }
    out += '}';
    return out;
}

std::string tenant_label(int64_t tenant_id) {
    return make_labels({{"tenant", std::to_string(tenant_id)}});
}

std::string tenant_route_label(int64_t tenant_id, const std::string& route) {
    return make_labels({{"tenant", std::to_string(tenant_id)},
                         {"route", route}});
}

std::string tenant_route_ok_label(int64_t tenant_id,
                                    const std::string& route, bool ok) {
    return make_labels({{"tenant", std::to_string(tenant_id)},
                         {"route", route},
                         {"ok",    ok ? "true" : "false"}});
}

std::string provider_label(const std::string& provider) {
    return make_labels({{"provider", provider}});
}

std::string tenant_reason_label(int64_t tenant_id,
                                  const std::string& reason) {
    return make_labels({{"tenant", std::to_string(tenant_id)},
                         {"reason", reason}});
}

} // namespace

Metrics::Metrics() {
    // Register every metric up front so /metrics emits stable HELP +
    // TYPE lines even before the first event fires.  Prometheus
    // tolerates absent metrics but operators usually want to see the
    // full surface in a fresh-start scrape so dashboards don't NaN out
    // on the first observation.
    register_metric("arbiter_requests_started_total",   Kind::Counter,
        "Requests admitted into the orchestrator.");
    register_metric("arbiter_requests_completed_total", Kind::Counter,
        "Requests that finished (per ok=true|false).");
    register_metric("arbiter_request_duration_ms_sum",  Kind::Counter,
        "Cumulative request wall-clock duration in milliseconds.");
    register_metric("arbiter_in_flight",                Kind::Gauge,
        "Requests currently being processed.");
    register_metric("arbiter_provider_calls_total",     Kind::Counter,
        "Upstream provider API calls dispatched.");
    register_metric("arbiter_provider_retries_total",   Kind::Counter,
        "Provider retries triggered by 429 / 5xx.");
    register_metric("arbiter_provider_5xx_total",       Kind::Counter,
        "Provider responses with a 5xx status.");
    register_metric("arbiter_provider_429_total",       Kind::Counter,
        "Provider responses with a 429 status.");
    register_metric("arbiter_provider_circuit_open_total", Kind::Counter,
        "Circuit breaker openings per provider.");
    register_metric("arbiter_sandbox_exec_total",       Kind::Counter,
        "Sandbox /exec invocations.");
    register_metric("arbiter_sandbox_exec_timeout_total", Kind::Counter,
        "Sandbox /exec runs killed by the wall-clock timeout.");
    register_metric("arbiter_sandbox_container_started_total", Kind::Counter,
        "Sandbox container cold starts.");
    register_metric("arbiter_sandbox_container_reaped_total", Kind::Counter,
        "Sandbox containers stopped by the idle reaper.");
    register_metric("arbiter_sandbox_container_rebuilt_total", Kind::Counter,
        "Sandbox containers rebuilt after a self-heal probe failure.");
    register_metric("arbiter_sandbox_containers_running", Kind::Gauge,
        "Sandbox containers currently warm.");
    register_metric("arbiter_idempotency_replay_total", Kind::Counter,
        "Requests served from the idempotency cache as replays.");
    register_metric("arbiter_idempotency_miss_total", Kind::Counter,
        "Idempotency-Key headers seen with no prior request.");
    register_metric("arbiter_rate_limited_total",       Kind::Counter,
        "Requests rejected by the per-tenant rate / concurrency limiter.");
}

void Metrics::register_metric(const std::string& name, Kind kind,
                               const std::string& help) {
    std::lock_guard<std::mutex> lk(mu_);
    metas_.emplace(name, MetricMeta{kind, help});
    // Pre-seed an empty inner map so render() produces a "no-data"
    // header for newly-registered metrics.
    series_.try_emplace(name);
}

Metrics::Series&
Metrics::get_or_create(const std::string& name, const std::string& labels) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& inner = series_[name];
    auto it = inner.find(labels);
    if (it != inner.end()) return *it->second;
    auto s = std::make_unique<Series>();
    s->rendered_labels = labels;
    auto& ref = *s;
    inner.emplace(labels, std::move(s));
    return ref;
}

void Metrics::inc(const std::string& name, const std::string& labels,
                   int64_t by) {
    auto& s = get_or_create(name, labels);
    s.value.fetch_add(by, std::memory_order_relaxed);
}

void Metrics::add(const std::string& name, const std::string& labels,
                   int64_t by) {
    inc(name, labels, by);
}

void Metrics::set(const std::string& name, const std::string& labels,
                   int64_t v) {
    auto& s = get_or_create(name, labels);
    s.value.store(v, std::memory_order_relaxed);
}

// ── Public increment surface ─────────────────────────────────────────────

void Metrics::inc_request_started(int64_t tenant_id, const std::string& route) {
    inc("arbiter_requests_started_total", tenant_route_label(tenant_id, route));
}

void Metrics::inc_request_completed(int64_t tenant_id,
                                     const std::string& route, bool ok) {
    inc("arbiter_requests_completed_total",
        tenant_route_ok_label(tenant_id, route, ok));
}

void Metrics::add_request_duration_ms(int64_t tenant_id,
                                       const std::string& route, int64_t ms) {
    add("arbiter_request_duration_ms_sum",
        tenant_route_label(tenant_id, route), ms);
}

void Metrics::inc_in_flight(int64_t tenant_id) {
    auto& s = get_or_create("arbiter_in_flight", tenant_label(tenant_id));
    s.value.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::dec_in_flight(int64_t tenant_id) {
    auto& s = get_or_create("arbiter_in_flight", tenant_label(tenant_id));
    s.value.fetch_sub(1, std::memory_order_relaxed);
}

void Metrics::inc_provider_call(const std::string& provider) {
    inc("arbiter_provider_calls_total", provider_label(provider));
}

void Metrics::inc_provider_retry(const std::string& provider) {
    inc("arbiter_provider_retries_total", provider_label(provider));
}

void Metrics::inc_provider_5xx(const std::string& provider) {
    inc("arbiter_provider_5xx_total", provider_label(provider));
}

void Metrics::inc_provider_429(const std::string& provider) {
    inc("arbiter_provider_429_total", provider_label(provider));
}

void Metrics::inc_provider_circuit_open(const std::string& provider) {
    inc("arbiter_provider_circuit_open_total", provider_label(provider));
}

void Metrics::inc_sandbox_exec() {
    inc("arbiter_sandbox_exec_total", "");
}

void Metrics::inc_sandbox_exec_timeout() {
    inc("arbiter_sandbox_exec_timeout_total", "");
}

void Metrics::inc_sandbox_container_started() {
    inc("arbiter_sandbox_container_started_total", "");
}

void Metrics::inc_sandbox_container_reaped() {
    inc("arbiter_sandbox_container_reaped_total", "");
}

void Metrics::inc_sandbox_container_rebuilt() {
    inc("arbiter_sandbox_container_rebuilt_total", "");
}

void Metrics::set_sandbox_containers_running(int n) {
    set("arbiter_sandbox_containers_running", "", n);
}

void Metrics::inc_idempotency_replay() {
    inc("arbiter_idempotency_replay_total", "");
}

void Metrics::inc_idempotency_miss() {
    inc("arbiter_idempotency_miss_total", "");
}

void Metrics::inc_rate_limited(int64_t tenant_id, const std::string& reason) {
    inc("arbiter_rate_limited_total",
        tenant_reason_label(tenant_id, reason));
}

// ── Rendering ────────────────────────────────────────────────────────────

std::string Metrics::render() const {
    std::ostringstream out;
    std::lock_guard<std::mutex> lk(mu_);
    // Iterate in metas_ order so the output is stable for a given
    // registry build.  Hash-map iteration order is implementation-
    // defined but the relative position of HELP / TYPE / samples for a
    // single metric is preserved either way, which is what counts.
    for (const auto& [name, meta] : metas_) {
        out << "# HELP " << name << ' ' << meta.help << "\n";
        out << "# TYPE " << name << ' '
            << (meta.kind == Kind::Counter ? "counter" : "gauge") << "\n";
        auto it = series_.find(name);
        if (it != series_.end()) {
            for (const auto& [labels_str, s] : it->second) {
                out << name << s->rendered_labels << ' '
                    << s->value.load(std::memory_order_relaxed) << "\n";
            }
        }
    }
    return out.str();
}

} // namespace arbiter
