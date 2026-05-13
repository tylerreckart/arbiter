#pragma once
// arbiter/include/api_server.h
//
// HTTP API surface for multi-agent orchestration.  Exposes the Orchestrator
// as a streaming SSE endpoint so external clients send a prompt in one POST
// and receive the agent's thinking, tool calls, sub-agent output, token
// usage, and any files the pipeline produced as separate SSE events.
//
// Endpoints:
//   GET  /v1/health              — 200 "ok", no auth, liveness probe
//   POST /v1/orchestrate         — main orchestration call, SSE response (tenant auth)
//   GET  /v1/admin/tenants       — list tenants (admin auth)
//   POST /v1/admin/tenants       — create tenant, returns plaintext token (admin auth)
//   GET  /v1/admin/tenants/{id}  — one tenant (admin auth)
//   PATCH /v1/admin/tenants/{id} — update {disabled} (admin auth)
//
// Billing — eligibility checks, cost tracking, caps, invoicing — runs
// through an external billing service when configured (see
// `billing_url` below).  The runtime exposes no usage ledger of its
// own; operators wanting commercial deployment must implement the
// billing protocol against a service of their choosing.
//
// Auth:
//   Tenant routes  — Bearer token that maps to a Tenant in the TenantStore.
//   Admin  routes  — Bearer admin token.  Admin token is distinct from tenant
//                    tokens and only works on /v1/admin/*; tenant tokens are
//                    rejected on admin routes and vice versa.
//
// Concurrency:
//   One thread per connection.  Each request gets a fresh Orchestrator —
//   concurrent requests don't share agent history or callback state.
//   Agent configs are re-loaded from disk per request; at a few JSON
//   files this is cheap next to the LLM call latency.
//
// Policy defaults (configurable per deploy):
//   • `/exec` disabled — agents can write files, fetch URLs, delegate
//     to sub-agents, but cannot run shell commands.  The exec branch
//     returns an ERR: tool-result block so the agent adapts.
//   • `/write` intercepted — nothing touches the server filesystem.
//     File content is captured in memory and emitted as `event: file`
//     on the SSE stream; the client chooses whether to persist.
//   • Per-response file size cap so a runaway agent can't OOM the
//     server (default 10 MiB across all files in a single response).
//
// Production deployment expects the TLS termination + rate-limit + DDoS
// handling to live in a reverse proxy (nginx / caddy / cloudflare) in
// front of the server.  The bind defaults to 127.0.0.1 to make that
// architecture the path of least resistance.

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace arbiter {

class BillingClient;
class IdempotencyCache;
class Metrics;
class NotificationBus;
class Orchestrator;
class ProviderCircuitBreaker;
class RequestEventBus;
class SandboxManager;
class Scheduler;
class TenantLimiter;
class TenantStore;
struct Tenant;

// Tracks in-flight orchestrations so a cancel request on thread Y can
// reach into the Orchestrator created on thread X and hit cancel().  Keyed
// by a short hex request_id assigned at stream-receive time.
//
// Entries are added by an RAII guard inside the request handler and
// removed when the guard falls out of scope, so a racing cancel on an
// already-finished request is a no-op (map lookup misses) rather than a
// dangling pointer.
struct InFlightRegistry {
    struct Entry {
        Orchestrator* orch     = nullptr;
        int64_t       tenant_id = 0;
    };
    std::mutex                           mu;
    std::unordered_map<std::string, Entry> by_id;
};

struct ApiServerOptions {
    int         port         = 8080;
    std::string bind         = "127.0.0.1";   // bind 0.0.0.0 only behind a proxy
    std::string agents_dir;                   // absolute path, e.g. ~/.arbiter/agents

    // Parent directory for per-tenant memory.  Each request's orchestrator
    // gets `memory_root + "/t<tenant_id>"` so /mem read/write for tenant A
    // can never see tenant B's notes.  Created on-demand by the memory
    // commands; empty memory is an empty dir, not an error.
    std::string memory_root;

    std::map<std::string, std::string> api_keys;   // provider name → key
    bool        exec_disabled     = true;     // /exec policy
    size_t      file_max_bytes    = 10 * 1024 * 1024;   // per-response cap

    // ── Per-tenant sandbox ───────────────────────────────────────────
    // When `sandbox_enabled` is true and the runtime + image are
    // available, /v1/orchestrate wires a per-tenant container that
    // confines /exec to a workspace volume and lets /write + /read
    // share that workspace.  When the SandboxManager fails its
    // usability check at startup the server logs a warning and
    // continues with /exec disabled (graceful degradation).
    bool        sandbox_enabled         = false;
    std::string sandbox_runtime         = "docker";   // v1: docker only
    std::string sandbox_image;                          // required when enabled
    std::string sandbox_workspaces_root;                // host path
    std::string sandbox_network         = "none";
    int         sandbox_memory_mb       = 512;
    double      sandbox_cpus            = 1.0;
    int         sandbox_pids_limit      = 256;
    int         sandbox_exec_timeout_seconds = 30;
    int64_t     sandbox_workspace_max_bytes = 1ll * 1024 * 1024 * 1024;
    int         sandbox_idle_seconds        = 1800;
    // Non-owning runtime handle.  Populated by ApiServer's constructor
    // when it builds a usable SandboxManager; null otherwise.  Lives in
    // opts so downstream request handlers and the orchestrator-builder
    // factories can wire ExecInvoker without an extra parameter on every
    // signature.  Lifetime is bound to the ApiServer's `sandbox_`
    // unique_ptr — do not delete through this pointer.
    SandboxManager* sandbox             = nullptr;

    // Non-owning runtime handle to the server's IdempotencyCache.
    // Populated by ApiServer's constructor; null when used outside an
    // ApiServer (e.g. CLI-mode build_blocking_orchestrator).  Same
    // ownership story as `sandbox` above.
    IdempotencyCache* idempotency       = nullptr;

    // Non-owning runtime handle to the server's Metrics registry.
    // Populated by ApiServer's ctor; null in CLI / one-shot contexts.
    // Counter increments are no-ops when null so call sites don't
    // need to guard.
    Metrics*          metrics           = nullptr;

    // Process-wide provider circuit breaker.  Wired into every
    // per-request ApiClient.  Null in CLI / one-shot contexts.
    ProviderCircuitBreaker* circuit_breaker = nullptr;

    // Plaintext admin token for /v1/admin/*.  Empty ⇒ admin endpoints
    // return 503 (disabled).  `cmd_api` loads/generates this before
    // constructing the server.
    std::string admin_token;

    // Mirror SSE events to stderr as they're emitted, with timestamps and
    // per-stream labels.  request_received / done / error always log; the
    // chatty per-delta events (text, thinking, tool_call, stream_start/end,
    // token_usage, sub_agent_response) only log when verbose is on.  Off by
    // default; flipped on by `arbiter --api --verbose` or env
    // `ARBITER_API_VERBOSE=1`.
    bool log_verbose = false;

    // Path to the MCP server registry JSON.  Empty ⇒ no MCP servers
    // configured (the /mcp slash command returns ERR with a clear
    // message).  See docs/api/concepts/mcp.md for the schema.
    std::string mcp_servers_path;

    // Path to the A2A remote-agent registry JSON.  Empty (or missing
    // file) ⇒ no remote agents configured; /a2a returns ERR with a
    // clear message.  Schema: { "agents": { "<name>": { "url": "...",
    // "auth": { "type": "bearer", "token_env": "..." } } } }.  See
    // docs/cli/a2a-agents.md.
    std::string a2a_agents_path;

    // Web-search provider config.  When `search_api_key` is non-empty
    // and the provider is recognized, /search <query> [top=N] dispatches
    // an HTTPS call against the provider's API.  Empty key ⇒ /search
    // returns a clean ERR.  Default provider is "brave"; the only one
    // implemented in v1, with Tavily/Exa slots reserved.
    std::string search_provider = "brave";
    std::string search_api_key;

    // Public-facing base URL the server is reachable at (e.g.
    // "https://arbiter.example.com").  Used to populate `url` fields in
    // A2A agent cards so remote clients dial the right endpoint.  Empty ⇒
    // derive per-request from the inbound Host header with scheme http://;
    // operators terminating TLS in front of arbiter should set this
    // explicitly so cards advertise the public https:// origin.
    std::string public_base_url;

    // External billing service base URL (e.g. "http://localhost:4000").
    // When set, every /v1/orchestrate call:
    //   • exchanges the bearer for a workspace_id via /v1/runtime/auth/validate
    //   • pre-flights against /v1/runtime/quota/check
    //   • fires post-turn telemetry to /v1/runtime/usage/record
    // Empty ⇒ skip billing entirely; requests pass through to the
    // configured provider API keys with no eligibility check.  This is
    // the documented escape hatch for self-hosted deploys without an
    // external billing service.  Loaded from $ARBITER_BILLING_URL.
    std::string billing_url;
};

// Build a synchronous-friendly Orchestrator wired with the same memory,
// MCP, search, and A2A bridges that /v1/orchestrate uses, but without the
// SSE / file-interceptor plumbing that depends on a per-request streaming
// sink.  Used by the A2A `message/send` (synchronous) path and by the
// background Scheduler.  The returned Orchestrator is fully self-contained
// and tenant-scoped; the caller invokes `orch->send(agent_id, prompt)` to
// run one turn.  On failure, returns null and populates `err_out`.
std::unique_ptr<Orchestrator>
build_blocking_orchestrator(const ApiServerOptions& opts,
                             TenantStore& tenants,
                             const Tenant& tenant,
                             std::string& err_out);

class ApiServer {
public:
    ApiServer(ApiServerOptions opts, TenantStore& tenants);
    ~ApiServer();

    ApiServer(const ApiServer&)            = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Open the listening socket, spawn the accept thread, return.
    void start();

    // Stop accepting, shut down the listen socket, join the accept thread.
    // In-flight connection threads are detached and self-terminate.
    void stop();

    bool running() const { return running_.load(); }
    int  port()    const { return bound_port_; }

    // Non-owning peek at the sandbox manager.  Always non-null when
    // `ARBITER_SANDBOX_IMAGE` was set at startup (the manager is held
    // even on usability failure so the unusable reason stays queryable
    // after the banner clears scrollback); null when the feature
    // wasn't requested at all.  Caller checks `usable()` for the
    // wired-vs-disabled split.
    const SandboxManager* sandbox_manager() const { return sandbox_.get(); }

private:
    void accept_loop();
    void handle_connection(int client_fd);

    ApiServerOptions  opts_;
    TenantStore&      tenants_;
    InFlightRegistry  in_flight_;
    // Lazily constructed iff opts_.billing_url is set.  In disabled
    // mode the pointer stays null and every billing-touching helper
    // short-circuits, so the runtime keeps routing to provider keys.
    std::unique_ptr<BillingClient> billing_;

    // Scheduling subsystem.  The bus is created up front so request handlers
    // can subscribe (the SSE notifications endpoint) before the scheduler is
    // ticking; the scheduler thread starts in start() and joins in stop().
    std::unique_ptr<NotificationBus> notifications_;
    std::unique_ptr<Scheduler>       scheduler_;
    // Per-request live-tail bus.  Subscribers (the resubscribe SSE
    // handler, A2A tasks/resubscribe) attach by request_id and receive
    // each newly-persisted event as it lands.  Always constructed; the
    // SSE writer publishes here whenever persistence is wired.
    std::unique_ptr<RequestEventBus> request_events_;
    // Per-tenant rate / concurrency limiter.  Always constructed (defaults
    // come from env at startup); a zeroed config means "unlimited" so
    // operators not using this surface pay no cost.
    std::unique_ptr<TenantLimiter>   limiter_;
    // Per-tenant /exec sandbox.  Constructed iff opts.sandbox_enabled
    // AND usable() — null otherwise.  When null, /exec falls back to
    // the legacy disabled behaviour (cmd_exec gated by exec_disabled).
    std::unique_ptr<SandboxManager>  sandbox_;
    // Idempotency-Key dedup cache for the write-creating POST routes.
    // Always constructed; absence of an Idempotency-Key header on a
    // request skips it entirely.
    std::unique_ptr<IdempotencyCache> idempotency_;
    // In-process Prometheus-style metrics registry.  Always
    // constructed; rendered at GET /v1/metrics.
    std::unique_ptr<Metrics>          metrics_;
    // Process-wide circuit breaker, shared by every per-request
    // ApiClient.  Always constructed; bound to metrics for
    // arbiter_provider_circuit_open_total bookkeeping.
    std::unique_ptr<ProviderCircuitBreaker> circuit_breaker_;

    int               listen_fd_  = -1;
    int               bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread       accept_thread_;

    // In-flight connection drain.  Each connection thread bumps
    // `active_connections_` on entry and decrements + notifies on exit.
    // `stop()` waits on `drain_cv_` for the counter to reach zero, up to
    // a deadline configurable via `ARBITER_DRAIN_SECONDS` (default 30s).
    // Connections still running past the deadline are abandoned — the
    // OS tears the sockets down when the process exits.
    std::atomic<int>        active_connections_{0};
    std::mutex              drain_mu_;
    std::condition_variable drain_cv_;
};

} // namespace arbiter
