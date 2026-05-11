#pragma once
// arbiter/include/metrics.h
//
// In-process Prometheus-style counter + gauge registry.  Wired by the
// API server, the orchestrator, the sandbox manager, and the API
// client; rendered as text format at `GET /v1/metrics`.
//
// Cardinality concerns: tenant_id appears as a label on the per-request
// counters.  Operators with thousands of tenants who scrape frequently
// will see a non-trivial /metrics body — acceptable for the multi-
// tenant scales arbiter targets (hundreds of tenants, single-node).
// Higher-cardinality deployments should aggregate at the proxy or move
// to a push-based exporter.
//
// Concurrency: all increments are atomic and lock-free in the common
// case.  Rendering takes a shared lock briefly to snapshot the label
// map; readers see a consistent point-in-time view but a counter that
// races a render may report yesterday's number on this scrape and the
// new number next scrape — which is exactly how Prometheus expects
// counters to behave.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace arbiter {

class Metrics {
public:
    Metrics();

    // ── Per-request counters / gauges ──────────────────────────────────
    // Fired by handle_orchestrate (and the other write-creating POST
    // routes) at three points: when the request enters the handler,
    // when it terminates, and continuously while it's in flight (gauge).

    void inc_request_started(int64_t tenant_id, const std::string& route);
    void inc_request_completed(int64_t tenant_id, const std::string& route,
                                bool ok);
    void add_request_duration_ms(int64_t tenant_id,
                                  const std::string& route, int64_t ms);

    // Gauge: number of requests currently in flight, tenant-scoped.
    void inc_in_flight(int64_t tenant_id);
    void dec_in_flight(int64_t tenant_id);

    // ── Provider counters ──────────────────────────────────────────────
    // Fired by ApiClient on each upstream call.  provider is the
    // canonical short id ("anthropic", "openai", "gemini", "ollama").

    void inc_provider_call(const std::string& provider);
    void inc_provider_retry(const std::string& provider);
    void inc_provider_5xx(const std::string& provider);
    void inc_provider_429(const std::string& provider);
    void inc_provider_circuit_open(const std::string& provider);

    // ── Sandbox counters / gauges ──────────────────────────────────────
    // Fired by SandboxManager.

    void inc_sandbox_exec();
    void inc_sandbox_exec_timeout();
    void inc_sandbox_container_started();
    void inc_sandbox_container_reaped();
    void inc_sandbox_container_rebuilt();
    void set_sandbox_containers_running(int n);

    // ── Idempotency counters ──────────────────────────────────────────
    void inc_idempotency_replay();
    void inc_idempotency_miss();

    // ── Rate-limit counters ───────────────────────────────────────────
    void inc_rate_limited(int64_t tenant_id, const std::string& reason);

    // Render the registry as Prometheus text format.  Returns the full
    // body; caller writes it to the wire after the SSE-free HTTP
    // headers.  Cheap enough to call on every scrape (typically every
    // 15-30s).
    std::string render() const;

private:
    // Counter with optional (tenant, route)-style labels.  We model
    // every metric as (name, labels) → atomic int64; the registry is a
    // map keyed by the rendered label tuple.  At the scales arbiter
    // targets the map is dozens to low hundreds of rows, so a single
    // shared mutex on inserts is fine; reads of existing rows are
    // lock-free relaxed loads.
    struct Series {
        std::atomic<int64_t> value{0};
        std::string          rendered_labels;  // "{tenant=\"42\",route=\"orchestrate\"}"
    };

    Series& get_or_create(const std::string& name,
                          const std::string& labels);
    void    inc(const std::string& name, const std::string& labels, int64_t by = 1);
    void    add(const std::string& name, const std::string& labels, int64_t by);
    void    set(const std::string& name, const std::string& labels, int64_t v);

    // Per-metric type metadata, used in the HELP/TYPE header lines.
    enum class Kind { Counter, Gauge };
    void register_metric(const std::string& name, Kind kind,
                          const std::string& help);

    struct MetricMeta {
        Kind        kind;
        std::string help;
    };

    mutable std::mutex                                   mu_;
    std::unordered_map<std::string, MetricMeta>          metas_;
    // Outer key: metric name.  Inner map: labels-string → Series.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::unique_ptr<Series>>> series_;
};

} // namespace arbiter
