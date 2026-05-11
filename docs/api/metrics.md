# `GET /v1/metrics`

**Auth:** none — _Status:_ stable

Prometheus-format scrape endpoint. Returns the current value of every counter and gauge the runtime tracks: per-tenant request flow, per-provider call health, sandbox container lifecycle, idempotency cache effectiveness, rate-limit rejections.

The endpoint is **unauthenticated** by design — the typical deployment lives behind the same reverse proxy that gates the tenant routes, with the proxy restricting `/v1/metrics` to the metrics scraper's source IP. Operators wanting tighter control should add an allow/deny rule at the proxy or run arbiter on a network where the scraper is the only client.

## Request

```bash
curl http://arbiter.example.com/v1/metrics
```

No path params, no query params, no body, no auth header.

## Response

`Content-Type: text/plain; version=0.0.4; charset=utf-8` (Prometheus text exposition format, version 0.0.4). `Connection: close`.

```
# HELP arbiter_requests_started_total Requests admitted into the orchestrator.
# TYPE arbiter_requests_started_total counter
arbiter_requests_started_total{tenant="1",route="orchestrate"} 12
arbiter_requests_started_total{tenant="1",route="messages"} 47
arbiter_requests_started_total{tenant="2",route="orchestrate"} 3
# HELP arbiter_in_flight Requests currently being processed.
# TYPE arbiter_in_flight gauge
arbiter_in_flight{tenant="1"} 2
arbiter_in_flight{tenant="2"} 0
...
```

Every registered metric emits its `HELP` + `TYPE` headers even on a fresh-start scrape with no observations — dashboards don't NaN out on the first poll.

## Metric reference

### Request flow

| Metric                                  | Type    | Labels                          | Increments when |
|-----------------------------------------|---------|---------------------------------|-----------------|
| `arbiter_requests_started_total`        | counter | `tenant`, `route`               | A request is admitted into the orchestrator. |
| `arbiter_requests_completed_total`      | counter | `tenant`, `route`, `ok`         | A request terminates. `ok="true"` when the `done` SSE event reported `ok=true`. |
| `arbiter_request_duration_ms_sum`       | counter | `tenant`, `route`               | Cumulative wall-clock duration in ms. `rate()` this to get average latency. |
| `arbiter_in_flight`                     | gauge   | `tenant`                        | Inc/dec'd around each request's handler scope. |

`route` is one of `orchestrate`, `messages`, `agent_chat`.

### Provider health

| Metric                                  | Type    | Labels      | Increments when |
|-----------------------------------------|---------|-------------|-----------------|
| `arbiter_provider_calls_total`          | counter | `provider`  | Each upstream API call attempt (per request, not per retry). |
| `arbiter_provider_retries_total`        | counter | `provider`  | A retry fires (attempt 2+ inside the retry loop). |
| `arbiter_provider_5xx_total`            | counter | `provider`  | Upstream returned a 5xx or threw at the socket level. |
| `arbiter_provider_429_total`            | counter | `provider`  | Upstream returned a 429 / `rate_limit_error` / `RESOURCE_EXHAUSTED`. |
| `arbiter_provider_circuit_open_total`   | counter | `provider`  | The circuit breaker transitions to Open for that provider. See [Operations → Circuit breaker](concepts/operations.md#provider-circuit-breaker). |

`provider` is one of `anthropic`, `openai`, `gemini`, `ollama`.

### Sandbox

| Metric                                       | Type    | Labels | Increments when |
|----------------------------------------------|---------|--------|-----------------|
| `arbiter_sandbox_exec_total`                 | counter | —      | Each `/exec` dispatch inside a tenant container. |
| `arbiter_sandbox_exec_timeout_total`         | counter | —      | The per-exec wall-clock kill fires. |
| `arbiter_sandbox_container_started_total`    | counter | —      | A tenant container is cold-started. |
| `arbiter_sandbox_container_reaped_total`     | counter | —      | The idle reaper stops a tenant container. |
| `arbiter_sandbox_container_rebuilt_total`    | counter | —      | The self-heal probe finds an unresponsive survivor and rebuilds. |
| `arbiter_sandbox_containers_running`         | gauge   | —      | Current count of warm tenant containers. |

### Idempotency

| Metric                                  | Type    | Labels | Increments when |
|-----------------------------------------|---------|--------|-----------------|
| `arbiter_idempotency_replay_total`      | counter | —      | A request hit the idempotency cache and replayed an existing run. |
| `arbiter_idempotency_miss_total`        | counter | —      | A request supplied `Idempotency-Key` but no cache entry existed (new run). |

Subtract `miss` from `started` to get "requests without an Idempotency-Key" if you care about adoption.

### Rate / concurrency limiter

| Metric                                  | Type    | Labels                 | Increments when |
|-----------------------------------------|---------|------------------------|-----------------|
| `arbiter_rate_limited_total`            | counter | `tenant`, `reason`     | A request is rejected by the per-tenant limiter. `reason` is `concurrent_request_limit` or `rate_limit`. |

## Cardinality

The runtime targets multi-tenant deployments at the **hundreds of tenants** scale. Per-tenant cardinality on `arbiter_requests_*_total` and `arbiter_in_flight` is linear in tenant count; a 500-tenant deployment scraped every 15 s sends roughly 50 KB of metrics per scrape. Comfortable.

Operators with thousands of tenants should either drop the `tenant` label at the scraper (sum the series at ingestion) or move to a push-based exporter — the in-process pull endpoint isn't the right shape for that scale. A scoped-down `/v1/metrics?aggregate=tenant` is a future option if demand warrants it.

## Prometheus scrape config

```yaml
scrape_configs:
  - job_name: arbiter
    metrics_path: /v1/metrics
    static_configs:
      - targets: ['arbiter.internal:8080']
    scrape_interval: 15s
```

## Failure modes

| Status | When |
|--------|------|
| 200    | Normal. Body is the exposition format above. |
| 404    | Wrong path. |
| 5xx    | Daemon down. |

## See also

- [Operational notes](concepts/operations.md) — JSON logging, circuit breaker, drain mechanics.
- [`GET /v1/health`](health.md) — liveness probe; pair with `/v1/metrics` for monitoring.
- [`GET /v1/admin/audit`](admin/audit.md) — admin mutation log.
