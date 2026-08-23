#pragma once
// arbiter/include/sandbox.h
//
// Per-tenant container sandbox for /exec, /write and /read.
//
// One persistent container per tenant_id, named `arbiter-sandbox-t<tid>`,
// keeps a host-mounted workspace at `<root>/t<tid>/`.  The directory is
// the single source of truth that all three writs share:
//
//   /exec    runs `docker exec` against the container with cwd /workspace.
//            When workspace_max_bytes is set, a pre-exec content snapshot is
//            taken beside the bind mount.  Over-cap, cancelled, or timed-out
//            results restore the tree from that snapshot (deleted files
//            recreated, overwrites reverted, new files removed) so the cap
//            sticks.  Timeout is the parent backstop or the wrapper's
//            reserved status 124 (sentinel watchdog).  137 and 143 are
//            never treated as timeout — those are OOM, self-kill, or
//            external signals.
//            Restore uses openat(O_NOFOLLOW)/mkdirat/unlinkat from the
//            workspace directory fd so a command cannot redirect host-side
//            writes through a parent symlink.
//   /write   writes the file into the host workspace dir (the container
//            sees it on next /exec via the bind mount); the SSE `file`
//            event still fires for the live UI
//   /read    falls back to the host workspace dir when no DB artifact
//            matches the path
//
// Resource caps (memory, cpus, pids, --network=none) are applied at
// container start.  Per-exec wall-clock is enforced two ways: the command
// is wrapped with a sentinel watchdog that exits 124 only if it sent
// SIGKILL, and the parent SIGKILLs the `docker exec` driver process when
// the deadline elapses.  On timeout a best-effort pass kills leftover non-PID-1
// processes inside the warm container.  Same-tenant `/exec` calls are
// serialized so that survivor cleanup cannot clobber a concurrent exec.
//
// Containers are started lazily on first ensure_container() per tenant.
// Idle reaping (ARBITER_SANDBOX_IDLE_SECONDS) stops warm containers after
// the idle threshold; stop_all() still runs on ApiServer shutdown.
// Workspace bytes survive both paths.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace arbiter { class Metrics; }

namespace arbiter {

struct SandboxConfig {
    // Runtime binary.  v1 supports "docker" only.  Set via env
    // ARBITER_SANDBOX_RUNTIME for forward-compat with podman.
    std::string runtime = "docker";

    // Container image.  Required; SandboxManager construction fails when
    // empty.  Operators pick what tooling lives inside.
    std::string image;

    // Host directory under which per-tenant workspaces are created as
    // `<root>/t<tid>/`.  Created on demand with 0700 perms.
    std::string workspaces_root;

    // Docker --network value.  Default "none" — /exec cannot reach the
    // internet.  /fetch and /search run in the arbiter process and are
    // unaffected.
    std::string network = "none";

    // --memory <MB>m.  0 = no cap.
    int memory_mb = 512;

    // --cpus value.  0 = no cap.
    double cpus = 1.0;

    // --pids-limit.  0 = no cap.
    int pids_limit = 256;

    // Per-exec wall-clock kill, seconds.  0 = no parent-side timeout
    // (the agent-side prompts still cap iterations).
    int exec_timeout_seconds = 30;

    // Combined stdout+stderr cap per /exec.  Matches the cmd_exec(host)
    // ceiling so the agent sees uniform behaviour regardless of sandbox.
    int output_max_bytes = 32768;

    // Max bytes loadable via read_from_workspace (/read sandbox fallback).
    // Workspace writes are quota-checked but reads previously had no bound,
    // which could OOM the API process on large files.  0 = no cap.
    int read_max_bytes = 10 * 1024 * 1024;

    // Max bytes returned by list_workspace (/list sandbox fallback).  Applied
    // during the directory walk; 0 = no cap.  ApiServer aligns this with
    // file_max_bytes when set.
    int list_max_bytes = 10 * 1024 * 1024;

    // Max file entries emitted by list_workspace.  Applied during the
    // directory walk (not after collecting every path) so pathological
    // trees cannot exhaust memory before truncation.  Guards many tiny
    // files when list_max_bytes alone is not enough.  0 = no cap.
    int list_max_files = 65536;

    // Per-tenant workspace disk quota, bytes.  Enforced on host-visible
    // workspace mutations: /write rejects writes that would exceed the
    // cap; /exec holds the per-tenant quota mutex, snapshots workspace
    // contents, then restores that snapshot if the command left the
    // workspace over the cap or was cancelled (so shell redirects and
    // partial cancelled writes cannot bypass /write).  0 = no quota.
    // Default 1 GiB.
    int64_t workspace_max_bytes = 1ll * 1024 * 1024 * 1024;

    // Test-only: milliseconds to sleep after a successful quota check and
    // before the write lands.  Unit tests set this to widen the measure-
    // then-write race window so parallel /write regressions are caught
    // deterministically.  0 in production (default).
    int quota_check_pause_ms = 0;

    // Test-only: milliseconds to sleep after docker exec returns while the
    // per-tenant quota mutex is still held.  Unit tests use this to prove
    // parallel /write cannot interleave with /exec (#136).  0 in production.
    int quota_exec_pause_ms = 0;

    // Idle-reaping threshold, seconds.  A background reaper stops
    // tenant containers whose last sandbox operation (/exec, /write,
    // /read, /list) was longer ago than this.  Workspace files are
    // untouched — next sandbox operation cold-starts a fresh
    // container.  0 = no reaping (containers warm until shutdown).
    // Default 1800 = 30 minutes.
    int idle_seconds = 1800;
};

struct SandboxExecResult {
    // Combined stdout + stderr (capped to output_max_bytes).  Already
    // includes the "[exit N]" suffix for non-zero exits and a
    // "[truncated at X KB]" trailer when capped — same shape as the
    // host cmd_exec(), so the dispatcher can write it verbatim.
    std::string output;
    int         exit_status = 0;
    bool        timed_out   = false;
    bool        canceled    = false;
    bool        ok          = true;   // infra-level OK (container ran, exec returned)
    std::string error;                // populated when ok=false
};

class SandboxManager {
public:
    explicit SandboxManager(SandboxConfig cfg);
    ~SandboxManager();

    // Attach a Metrics registry for per-container lifecycle counters
    // (cold-start, reaped, rebuilt, exec total, exec timeout, gauge of
    // warm containers).  Non-owning pointer; null = no metrics emitted.
    void set_metrics(Metrics* m) { metrics_ = m; }

    SandboxManager(const SandboxManager&)            = delete;
    SandboxManager& operator=(const SandboxManager&) = delete;

    // True when the configured runtime binary is reachable on PATH and
    // the image string is non-empty.  Callers (ApiServer) gate sandbox
    // wiring on this so a misconfigured deploy degrades to /exec
    // disabled rather than failing every request.
    bool usable() const { return usable_; }

    // Reason usable() returned false at construction.  Empty when usable().
    const std::string& unusable_reason() const { return unusable_reason_; }

    // The configured per-exec wall-clock cap.  Exposed so the dispatcher
    // can surface it in tool-result framing.
    int exec_timeout_seconds() const { return cfg_.exec_timeout_seconds; }

    // Full config snapshot.  Used by `cli.cpp` to render the sandbox
    // line in the startup banner — the constructor's stderr log gets
    // wiped by the banner's `\033[2J` clear, so the banner re-surfaces
    // status by querying this manager directly.
    const SandboxConfig& config() const { return cfg_; }

    // Host path of the tenant's workspace directory.  Idempotent;
    // creates the directory on first call.  Returns empty string on
    // mkdir failure (logged to stderr).
    std::string ensure_workspace(int64_t tenant_id);

    // Lazily start the tenant's container if not already running.
    // Returns true on success, false on infra failure (mkdir failure,
    // docker run non-zero, etc.).  `err_out` is populated on failure.
    bool ensure_container(int64_t tenant_id, std::string& err_out);

    // Run a shell command inside the tenant's container.  Lazily starts
    // the container if not yet running.  Always returns a result; on
    // infra failure the body is an "ERR: ..." string and ok=false.
    // timeout_seconds_override: when > 0, replaces cfg_.exec_timeout_seconds
    // for this call.  cancel: when set, polls every 250ms and SIGKILLs the
    // docker-exec driver on cancel (same cadence as reconcile host verify).
    SandboxExecResult exec(int64_t tenant_id, const std::string& command,
                           int timeout_seconds_override = 0,
                           std::atomic<bool>* cancel = nullptr);

    // Drop a file into the tenant's workspace.  Used by the /write
    // interceptor so the same byte sequence the client receives is also
    // visible to subsequent /exec calls inside the container.  Path is
    // a workspace-relative path; absolute or traversing paths are
    // rejected.  Returns true + workspace-relative path on success.
    bool write_to_workspace(int64_t tenant_id,
                             const std::string& rel_path,
                             const std::string& content,
                             std::string& err_out);

    // Read a workspace-relative file.  Returns true + content + mime
    // (best-effort, derived from extension) on success.  Used by the
    // /read path-fallback when the DB artifact store doesn't have a
    // matching row.
    bool read_from_workspace(int64_t tenant_id,
                              const std::string& rel_path,
                              std::string& content_out,
                              std::string& mime_out,
                              std::string& err_out);

    // Workspace listing for /list.  One line per file: "<rel_path>
    // <size> bytes".  Returns empty string when the workspace is empty
    // or doesn't yet exist.
    std::string list_workspace(int64_t tenant_id);

    // Sum of all regular-file sizes in the tenant's workspace, bytes.
    // Used by write_to_workspace for quota enforcement and exposed for
    // operators / tests inspecting usage.  Returns 0 when the workspace
    // doesn't exist.
    int64_t measure_workspace_bytes(int64_t tenant_id) const;

    // Tear down the tenant's container (workspace files remain so a
    // subsequent /exec restarts with the same state).  Idempotent.
    void stop_container(int64_t tenant_id);

    // Tear down every running container managed by this instance.
    // Called by ApiServer::stop().
    void stop_all();

private:
    SandboxConfig                              cfg_;
    bool                                       usable_ = false;
    std::string                                unusable_reason_;
    Metrics*                                   metrics_ = nullptr;

    // Guards `running_` / `last_access_` map mutations only.  Docker CLI
    // (inspect / run / rm) must not run under this lock — concurrent
    // tenants would otherwise serialize behind a ~30s `docker run`.
    // Same-tenant start/stop races use `start_mu_` instead.
    std::mutex                                 mu_;
    // tenant_id → container name.  Container names are deterministic so
    // a server restart can re-attach without losing track, but we keep
    // the in-memory set so we know which ones to stop on shutdown.
    std::unordered_map<int64_t, std::string>   running_;
    // tenant_id → last sandbox operation timestamp.  Updated on every
    // exec / write / read / list that succeeds.  Reaper thread compares
    // against cfg_.idle_seconds.  Guarded by mu_.
    std::unordered_map<int64_t, std::chrono::steady_clock::time_point>
                                                last_access_;
    // Per-tenant start/stop critical section.  Held across docker CLI
    // for that tenant only, so cross-tenant cold-starts overlap.
    // shared_ptr so callers can release mu_ before locking the tenant
    // mutex (never lock start_mu while holding mu_).
    std::unordered_map<int64_t, std::shared_ptr<std::mutex>> start_mu_;
    // Set by stop_all() before it sweeps.  ensure_container refuses to
    // cold-start once set, so a start racing shutdown can't create a
    // container after its tenant was already swept.  Guarded by mu_.
    bool                                       stopping_ = false;

    // Reaper thread.  Spawned in the ctor when usable_ && idle_seconds>0.
    // Joined in the dtor.
    std::thread                                reaper_thread_;
    std::condition_variable                    reaper_cv_;
    bool                                       reaper_stop_ = false;

    // Helpers (implementation detail).
    std::string container_name_for(int64_t tenant_id) const;
    std::string workspace_path_for(int64_t tenant_id) const;
    std::shared_ptr<std::mutex> start_mutex_for(int64_t tenant_id);
    bool        container_is_running(const std::string& name) const;
    bool        container_is_responsive(const std::string& name) const;
    bool        start_container(int64_t tenant_id, std::string& err_out);
    void        touch_access(int64_t tenant_id);
    void        reaper_loop();
};

} // namespace arbiter
