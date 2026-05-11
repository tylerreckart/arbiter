#pragma once
// arbiter/include/sandbox.h
//
// Per-tenant container sandbox for /exec, /write and /read.
//
// One persistent container per tenant_id, named `arbiter-sandbox-t<tid>`,
// keeps a host-mounted workspace at `<root>/t<tid>/`.  The directory is
// the single source of truth that all three writs share:
//
//   /exec    runs `docker exec` against the container with cwd /workspace
//   /write   writes the file into the host workspace dir (the container
//            sees it on next /exec via the bind mount); the SSE `file`
//            event still fires for the live UI
//   /read    falls back to the host workspace dir when no DB artifact
//            matches the path
//
// Resource caps (memory, cpus, pids, --network=none) are applied at
// container start.  Per-exec wall-clock is enforced parent-side by
// SIGKILLing the `docker exec` driver process when the deadline elapses;
// best-effort container-side killing is a follow-up.
//
// Containers are started lazily on first ensure_container() per tenant
// and stopped only via explicit stop_container() / stop_all() (e.g.
// ApiServer shutdown).  Idle reaping is intentionally not implemented
// here — operators run with `docker container prune` cadence of their
// choice; the v1 contract is "warm until shutdown".

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

    // Per-tenant workspace disk quota, bytes.  Enforced at /write time:
    // a write that would push the workspace over the cap is rejected
    // with a clean ERR.  Reads remain available, so the agent can list
    // what's there and clean up.  0 = no quota.  Default 1 GiB.
    int64_t workspace_max_bytes = 1ll * 1024 * 1024 * 1024;

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
    bool        ok          = true;   // infra-level OK (container ran, exec returned)
    std::string error;                // populated when ok=false
};

class SandboxManager {
public:
    explicit SandboxManager(SandboxConfig cfg);
    ~SandboxManager();

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
    SandboxExecResult exec(int64_t tenant_id, const std::string& command);

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

    // Guards `running_` against concurrent ensure_container / stop calls
    // when two requests on the same tenant race a cold-start.
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

    // Reaper thread.  Spawned in the ctor when usable_ && idle_seconds>0.
    // Joined in the dtor.
    std::thread                                reaper_thread_;
    std::condition_variable                    reaper_cv_;
    bool                                       reaper_stop_ = false;

    // Helpers (implementation detail).
    std::string container_name_for(int64_t tenant_id) const;
    std::string workspace_path_for(int64_t tenant_id) const;
    bool        container_is_running(const std::string& name) const;
    bool        container_is_responsive(const std::string& name) const;
    bool        start_container(int64_t tenant_id, std::string& err_out);
    void        touch_access(int64_t tenant_id);
    void        reaper_loop();
};

} // namespace arbiter
