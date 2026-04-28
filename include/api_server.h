#pragma once
// index/include/api_server.h
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
//   PATCH /v1/admin/tenants/{id} — update {disabled, monthly_cap_usd} (admin auth)
//   GET  /v1/admin/usage         — usage log, filters: tenant_id, since, until, limit
//
// Auth:
//   Tenant routes  — Bearer token that maps to a Tenant in the TenantStore.
//   Admin  routes  — Bearer admin token.  Admin token is distinct from tenant
//                    tokens and only works on /v1/admin/*; tenant tokens are
//                    rejected on admin routes and vice versa.  This lets a
//                    sibling service (billing/dashboards) read the ledger
//                    without being able to impersonate a tenant.
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
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace index_ai {

class Orchestrator;
class TenantStore;

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
};

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

private:
    void accept_loop();
    void handle_connection(int client_fd);

    ApiServerOptions  opts_;
    TenantStore&      tenants_;
    InFlightRegistry  in_flight_;
    int               listen_fd_  = -1;
    int               bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread       accept_thread_;
};

} // namespace index_ai
