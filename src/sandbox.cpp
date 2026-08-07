// arbiter/src/sandbox.cpp

#include "sandbox.h"
#include "logger.h"
#include "metrics.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace arbiter {

namespace {

// Single-quote for embedding `s` inside an `sh -c` script.
std::string shell_single_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Wrap a user command so the container itself enforces the wall-clock
// deadline when GNU `timeout` is on PATH inside the image.  Falls back
// to a plain `sh -c` when `timeout` is missing — the parent-side
// SIGKILL of `docker exec` + kill_exec_survivors remains the backstop.
std::string wrap_exec_with_timeout(const std::string& command,
                                   int timeout_seconds) {
    if (timeout_seconds <= 0) return command;
    const std::string q = shell_single_quote(command);
    return "if command -v timeout >/dev/null 2>&1; then "
           "exec timeout -s KILL " + std::to_string(timeout_seconds) +
           "s sh -c " + q + "; "
           "else exec sh -c " + q + "; fi";
}

// Run argv, capture stdout+stderr into `out`. SIGKILL on timeout; -1 on fork failure.
int run_capture(const std::vector<std::string>& argv,
                int timeout_seconds,
                size_t output_cap,
                std::string& out,
                bool& timed_out_out,
                bool* truncated_out = nullptr) {
    timed_out_out = false;
    out.clear();
    if (argv.empty()) {
        out = "ERR: empty argv";
        return -1;
    }

    int pipe_fd[2] = {-1, -1};
    if (::pipe(pipe_fd) != 0) {
        out = std::string("ERR: pipe failed: ") + ::strerror(errno);
        return -1;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        out = std::string("ERR: fork failed: ") + ::strerror(errno);
        return -1;
    }

    if (pid == 0) {
        // Child: stdout + stderr → pipe.
        while (::dup2(pipe_fd[1], STDOUT_FILENO) < 0 && errno == EINTR) {}
        while (::dup2(pipe_fd[1], STDERR_FILENO) < 0 && errno == EINTR) {}
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);

        std::vector<char*> argp;
        argp.reserve(argv.size() + 1);
        for (auto& s : argv) argp.push_back(const_cast<char*>(s.c_str()));
        argp.push_back(nullptr);
        ::execvp(argp[0], argp.data());
        // execvp only returns on failure.  Emit a recognisable line so
        // the parent can distinguish from real subprocess output.
        std::fprintf(stderr, "ERR: execvp failed: %s\n", ::strerror(errno));
        ::_exit(127);
    }

    // Parent
    ::close(pipe_fd[1]);
    int read_fd = pipe_fd[0];
    int flags = ::fcntl(read_fd, F_GETFL);
    if (flags >= 0) ::fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);

    auto start    = std::chrono::steady_clock::now();
    auto deadline = (timeout_seconds > 0)
        ? start + std::chrono::seconds(timeout_seconds)
        : std::chrono::steady_clock::time_point::max();

    char buf[4096];
    bool eof = false;
    while (!eof) {
        // Compute remaining time before poll.  When the deadline is
        // "max", use -1 (block until I/O) — combined with periodic
        // waitpid below, the child still drains correctly.
        int poll_ms = -1;
        if (timeout_seconds > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                timed_out_out = true;
                ::kill(pid, SIGKILL);
                break;
            }
            poll_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count());
            if (poll_ms < 0) poll_ms = 0;
            if (poll_ms > 1000) poll_ms = 1000;
        } else {
            // No deadline: poll in 1 s windows so we wake periodically
            // to call waitpid (catches the child exiting after closing
            // stdout, before its zombie is reaped).
            poll_ms = 1000;
        }

        pollfd pfd{read_fd, POLLIN, 0};
        int rc = ::poll(&pfd, 1, poll_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) {
            // Poll timeout — re-loop to re-check the wall-clock deadline.
            // Also check if the child already exited (it can close stdout
            // and exit without us seeing POLLHUP if the kernel buffered).
            int status_check = 0;
            pid_t r = ::waitpid(pid, &status_check, WNOHANG);
            if (r == pid) {
                // Child is gone — flip the pipe back to blocking so a
                // transient EAGAIN cannot abort the drain before the
                // remaining buffered bytes (and truncation) are seen.
                int fl = ::fcntl(read_fd, F_GETFL);
                if (fl >= 0) ::fcntl(read_fd, F_SETFL, fl & ~O_NONBLOCK);
                ssize_t k;
                while ((k = ::read(read_fd, buf, sizeof(buf))) > 0) {
                    if (out.size() < output_cap) {
                        size_t to_take = std::min(
                            static_cast<size_t>(k), output_cap - out.size());
                        out.append(buf, to_take);
                        if (truncated_out &&
                            to_take < static_cast<size_t>(k)) {
                            *truncated_out = true;
                        }
                    } else if (truncated_out) {
                        *truncated_out = true;
                    }
                }
                ::close(read_fd);
                if (WIFEXITED(status_check)) return WEXITSTATUS(status_check);
                if (WIFSIGNALED(status_check))
                    return 128 + WTERMSIG(status_check);
                return status_check;
            }
            continue;
        }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t k = ::read(read_fd, buf, sizeof(buf));
            if (k > 0) {
                if (out.size() < output_cap) {
                    size_t to_take = std::min(
                        static_cast<size_t>(k), output_cap - out.size());
                    out.append(buf, to_take);
                    if (truncated_out &&
                        to_take < static_cast<size_t>(k)) {
                        *truncated_out = true;
                    }
                } else if (truncated_out) {
                    *truncated_out = true;
                }
                continue;
            }
            if (k == 0) { eof = true; break; }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) break;
    }

    ::close(read_fd);
    int status = 0;
    if (timed_out_out) {
        ::waitpid(pid, &status, 0);
        return 124; // conventional timeout exit code
    }
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        break;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

// True if the binary `name` is found on PATH.
bool which(const std::string& name) {
    if (name.empty()) return false;
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p = path;
    std::stringstream ss(p);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        fs::path candidate = fs::path(dir) / name;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec))
            return true;
    }
    return false;
}

// Workspace-path safety check.  Rejects absolute paths, empty paths,
// traversal segments, and any null/control characters.  Returns the
// canonical relative form on success.
bool sanitize_rel(const std::string& raw, std::string& clean_out,
                  std::string& err_out) {
    if (raw.empty()) { err_out = "empty path"; return false; }
    if (raw.size() > 1024) { err_out = "path too long"; return false; }
    if (raw.front() == '/') { err_out = "absolute paths rejected"; return false; }
    if (raw.find('\0') != std::string::npos) {
        err_out = "null byte in path"; return false;
    }
    // Reject backslash to keep the contract platform-stable; Windows
    // sandboxing isn't a supported configuration in v1.
    if (raw.find('\\') != std::string::npos) {
        err_out = "backslash in path"; return false;
    }

    std::vector<std::string> parts;
    std::string cur;
    for (char c : raw) {
        if (c == '/') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            err_out = "control byte in path"; return false;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) { err_out = "empty path"; return false; }

    for (auto& seg : parts) {
        if (seg == "." || seg == "..") {
            err_out = "traversal in path"; return false;
        }
        if (seg.size() > 255) {
            err_out = "path component too long"; return false;
        }
    }

    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    clean_out = std::move(out);
    return true;
}

// Resolve a workspace-relative path to an absolute host path that is
// guaranteed to stay under `ws_root`.  Canonicalises existing ancestors
// so a symlink planted inside the workspace (e.g. via /exec ln -s)
// cannot escape to the host filesystem when arbiter opens the path.
bool resolve_within_workspace(const fs::path& ws_root,
                              const std::string& clean_rel,
                              fs::path& target_out,
                              std::string& err_out) {
    std::error_code ec;
    fs::path ws_canon = fs::weakly_canonical(ws_root, ec);
    if (ec) ws_canon = ws_root.lexically_normal();
    const std::string ws_str = ws_canon.string();

    fs::path abs = ws_canon / clean_rel;
    fs::path existing = abs;
    std::vector<std::string> missing_names;
    while (!existing.empty()) {
        std::error_code e2;
        if (fs::exists(existing, e2)) break;
        // Collect leaf names as strings — `path / empty_path` on libstdc++
        // yields a trailing slash (`ok.txt/`), which then makes
        // create_directories() mkdir the leaf as a directory.
        missing_names.push_back(existing.filename().string());
        if (!existing.has_parent_path() || existing.parent_path() == existing) {
            existing.clear();
            break;
        }
        existing = existing.parent_path();
    }

    fs::path resolved;
    if (!existing.empty()) {
        std::error_code e3;
        fs::path canon = fs::canonical(existing, e3);
        if (e3) {
            err_out = "invalid path: " + e3.message();
            return false;
        }
        resolved = canon;
        for (auto it = missing_names.rbegin(); it != missing_names.rend(); ++it)
            resolved /= *it;
    } else {
        resolved = fs::weakly_canonical(abs, ec);
        if (ec) {
            err_out = "invalid path: " + ec.message();
            return false;
        }
        resolved = resolved.lexically_normal();
    }
    const std::string resolved_str = resolved.lexically_normal().string();
    // Drop a trailing slash so prefix checks and ofstream see a file path.
    std::string normalized = resolved_str;
    while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
    resolved = fs::path(normalized);

    const std::string final_str = resolved.string();
    if (final_str.size() < ws_str.size() ||
        final_str.compare(0, ws_str.size(), ws_str) != 0 ||
        (final_str.size() > ws_str.size() &&
         final_str[ws_str.size()] != '/')) {
        err_out = "path escapes workspace";
        return false;
    }
    target_out = std::move(resolved);
    return true;
}

// Best-effort mime sniff from extension.  The workspace fallback for
// /read needs a media_type so the dispatcher can decide image-vs-text
// framing; we don't try magic-byte detection in v1.
std::string mime_for(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "text/plain";
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    if (ext == "png")                       return "image/png";
    if (ext == "jpg" || ext == "jpeg")      return "image/jpeg";
    if (ext == "gif")                       return "image/gif";
    if (ext == "webp")                      return "image/webp";
    if (ext == "json")                      return "application/json";
    if (ext == "html" || ext == "htm")      return "text/html";
    if (ext == "css")                       return "text/css";
    if (ext == "js")                        return "application/javascript";
    if (ext == "md")                        return "text/markdown";
    if (ext == "xml")                       return "application/xml";
    if (ext == "pdf")                       return "application/pdf";
    return "text/plain";
}

} // namespace

// ---------------------------------------------------------------------------
// SandboxManager
// ---------------------------------------------------------------------------

SandboxManager::SandboxManager(SandboxConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.image.empty()) {
        unusable_reason_ = "sandbox enabled but no image configured "
                           "(set ARBITER_SANDBOX_IMAGE)";
        usable_ = false;
        return;
    }
    if (cfg_.workspaces_root.empty()) {
        unusable_reason_ = "sandbox enabled but no workspaces root configured";
        usable_ = false;
        return;
    }
    if (!which(cfg_.runtime)) {
        unusable_reason_ = "sandbox runtime '" + cfg_.runtime +
                           "' not found on PATH";
        usable_ = false;
        return;
    }
    std::error_code ec;
    fs::create_directories(cfg_.workspaces_root, ec);
    if (ec) {
        unusable_reason_ = "cannot create workspaces root '" +
                            cfg_.workspaces_root + "': " + ec.message();
        usable_ = false;
        return;
    }
    usable_ = true;

    if (cfg_.idle_seconds > 0) {
        reaper_thread_ = std::thread(&SandboxManager::reaper_loop, this);
    }
}

SandboxManager::~SandboxManager() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        reaper_stop_ = true;
    }
    reaper_cv_.notify_all();
    if (reaper_thread_.joinable()) reaper_thread_.join();
    stop_all();
}

void SandboxManager::touch_access(int64_t tenant_id) {
    std::lock_guard<std::mutex> lk(mu_);
    last_access_[tenant_id] = std::chrono::steady_clock::now();
}

std::shared_ptr<std::mutex> SandboxManager::start_mutex_for(int64_t tenant_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& slot = start_mu_[tenant_id];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}

void SandboxManager::reaper_loop() {
    using clock = std::chrono::steady_clock;
    // Tick cadence is bounded by idle_seconds so a 60s idle threshold
    // doesn't make us wait 5 minutes to react.  Floor at 30s so we
    // don't burn CPU on a misconfigured tiny idle window.
    const auto tick = std::chrono::seconds(
        std::max(30, cfg_.idle_seconds / 4));
    while (true) {
        std::vector<int64_t> to_stop;
        {
            std::unique_lock<std::mutex> lk(mu_);
            reaper_cv_.wait_for(lk, tick, [this]{ return reaper_stop_; });
            if (reaper_stop_) return;
            auto now = clock::now();
            auto threshold = std::chrono::seconds(cfg_.idle_seconds);
            for (auto& kv : running_) {
                auto it = last_access_.find(kv.first);
                // No recorded access = pre-reaper era; stamp now so we
                // don't reap immediately on the next pass.
                if (it == last_access_.end()) {
                    last_access_[kv.first] = now;
                    continue;
                }
                if (now - it->second >= threshold) {
                    to_stop.push_back(kv.first);
                }
            }
        }
        for (int64_t tid : to_stop) {
            Logger::global().info("sandbox_container_reaped", {
                {"tenant_id", std::to_string(tid)},
                {"idle_seconds", std::to_string(cfg_.idle_seconds)},
            });
            stop_container(tid);
            if (metrics_) metrics_->inc_sandbox_container_reaped();
            std::lock_guard<std::mutex> lk(mu_);
            last_access_.erase(tid);
        }
        if (metrics_) {
            std::lock_guard<std::mutex> lk(mu_);
            metrics_->set_sandbox_containers_running(
                static_cast<int>(running_.size()));
        }
    }
}

std::string SandboxManager::container_name_for(int64_t tenant_id) const {
    return "arbiter-sandbox-t" + std::to_string(tenant_id);
}

std::string SandboxManager::workspace_path_for(int64_t tenant_id) const {
    return cfg_.workspaces_root + "/t" + std::to_string(tenant_id);
}

std::string SandboxManager::ensure_workspace(int64_t tenant_id) {
    std::string p = workspace_path_for(tenant_id);
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) {
        std::fprintf(stderr, "[sandbox] mkdir %s failed: %s\n",
                     p.c_str(), ec.message().c_str());
        return "";
    }
    // 0700 — workspace contents are tenant-private even on a shared host.
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::replace, ec);
    return p;
}

bool SandboxManager::container_is_running(const std::string& name) const {
    std::vector<std::string> argv{
        cfg_.runtime, "inspect", "-f", "{{.State.Running}}", name
    };
    std::string out;
    bool timed_out = false;
    int rc = run_capture(argv, /*timeout_seconds=*/10, /*output_cap=*/256,
                          out, timed_out);
    if (rc != 0 || timed_out) return false;
    // `docker inspect` prints `true\n` or `false\n`.
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out == "true";
}

// `docker exec <name> true` — catches containers that are "running" per inspect
// but unresponsive at exec level. Only called on re-attach and after infra failures.
bool SandboxManager::container_is_responsive(const std::string& name) const {
    std::vector<std::string> argv{
        cfg_.runtime, "exec", name, "true"
    };
    std::string out;
    bool timed_out = false;
    int rc = run_capture(argv, /*timeout_seconds=*/5, /*output_cap=*/256,
                          out, timed_out);
    return rc == 0 && !timed_out;
}

bool SandboxManager::start_container(int64_t tenant_id, std::string& err_out) {
    const std::string name = container_name_for(tenant_id);
    const std::string ws   = ensure_workspace(tenant_id);
    if (ws.empty()) {
        err_out = "workspace mkdir failed";
        return false;
    }

    // If a previous run left a stopped container with this name, remove
    // it first so `docker run` doesn't fail with "name in use".
    {
        std::vector<std::string> rm{cfg_.runtime, "rm", "-f", name};
        std::string ignore;
        bool to = false;
        run_capture(rm, /*timeout_seconds=*/10, /*output_cap=*/512, ignore, to);
    }

    std::vector<std::string> argv{
        cfg_.runtime, "run", "-d",
        "--name", name,
        "--network", cfg_.network,
        "-v", ws + ":/workspace",
        "-w", "/workspace",
        "--read-only",
        "--tmpfs", "/tmp:rw,size=64m",
        "--restart", "no",
    };
    if (cfg_.memory_mb > 0) {
        argv.push_back("--memory");
        argv.push_back(std::to_string(cfg_.memory_mb) + "m");
    }
    if (cfg_.cpus > 0.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", cfg_.cpus);
        argv.push_back("--cpus");
        argv.push_back(buf);
    }
    if (cfg_.pids_limit > 0) {
        argv.push_back("--pids-limit");
        argv.push_back(std::to_string(cfg_.pids_limit));
    }
    // Run as the host user so files written by /exec inherit ownership
    // we can subsequently read back via the bind mount.  Skipped on
    // macOS where Docker Desktop already maps to the host user.
#ifndef ARBITER_MACOS
    argv.push_back("--user");
    argv.push_back(std::to_string(::getuid()) + ":" +
                   std::to_string(::getgid()));
#endif
    argv.push_back(cfg_.image);
    // Keep the container alive so we can `docker exec` against it.
    argv.push_back("sleep");
    argv.push_back("infinity");

    std::string out;
    bool to = false;
    int rc = run_capture(argv, /*timeout_seconds=*/30,
                          /*output_cap=*/4096, out, to);
    if (rc != 0 || to) {
        err_out = "docker run failed: " + out;
        return false;
    }
    return true;
}

bool SandboxManager::ensure_container(int64_t tenant_id, std::string& err_out) {
    if (!usable_) { err_out = unusable_reason_; return false; }
    const std::string name = container_name_for(tenant_id);

    // Per-tenant critical section, held across the docker CLI (inspect,
    // probe, run) for this tenant only — different tenants hold different
    // mutexes, so cross-tenant warm checks and cold-starts all overlap.
    // stop_container takes the same mutex, so a concurrent reap cannot
    // `docker rm` between our inspect and a successful return.
    auto tenant_mu = start_mutex_for(tenant_id);
    std::lock_guard<std::mutex> start_lk(*tenant_mu);

    bool tracked = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Checked under the tenant mutex: stop_all sets stopping_ before
        // sweeping, so either we see the flag and bail, or we started
        // before the sweep and stop_container waits behind our mutex and
        // removes whatever we start.
        if (stopping_) {
            err_out = "sandbox manager is shutting down";
            return false;
        }
        tracked = running_.find(tenant_id) != running_.end();
    }
    if (tracked) {
        // We think it's running; verify and re-create if it died.  No
        // responsive-probe here — that's a 30-100ms tax on every /exec
        // and the next /exec will catch a wedged container via the
        // post-exec eviction path anyway.
        if (container_is_running(name)) {
            // Re-assert the map entry: exec()'s self-heal (which takes
            // only mu_, not the tenant start mutex) may have erased it
            // while our inspect was in flight.  Without this the live
            // container would go untracked — invisible to the idle
            // reaper and leaked past stop_all.
            std::lock_guard<std::mutex> lk(mu_);
            running_[tenant_id] = name;
            return true;
        }
        std::lock_guard<std::mutex> lk(mu_);
        running_.erase(tenant_id);
    } else if (container_is_running(name)) {
        // Stale survivor from a previous server run with the same
        // workspace root — re-attach only after a `docker exec true`
        // roundtrip confirms it's still functional.  A daemon restart
        // or a kernel-side namespace teardown can leave the inspect
        // saying "Running" while exec hangs; rebuild in that case.
        if (container_is_responsive(name)) {
            std::lock_guard<std::mutex> lk(mu_);
            running_[tenant_id] = name;
            return true;
        }
        std::fprintf(stderr,
            "[sandbox] survivor container '%s' inspect=running but "
            "exec-probe failed; rebuilding\n", name.c_str());
        if (metrics_) metrics_->inc_sandbox_container_rebuilt();
        std::vector<std::string> rm{cfg_.runtime, "rm", "-f", name};
        std::string ignore; bool to = false;
        run_capture(rm, /*timeout_seconds=*/10, /*output_cap=*/512,
                     ignore, to);
    }

    if (!start_container(tenant_id, err_out)) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        running_[tenant_id] = name;
        if (metrics_) {
            metrics_->inc_sandbox_container_started();
            metrics_->set_sandbox_containers_running(
                static_cast<int>(running_.size()));
        }
    }
    return true;
}

SandboxExecResult SandboxManager::exec(int64_t tenant_id,
                                        const std::string& command) {
    SandboxExecResult r;
    if (!usable_) {
        r.ok = false;
        r.error = unusable_reason_;
        r.output = "ERR: " + r.error;
        return r;
    }
    if (command.empty()) {
        r.ok = false;
        r.error = "empty command";
        r.output = "ERR: empty command";
        return r;
    }

    std::string err;
    if (!ensure_container(tenant_id, err)) {
        r.ok = false;
        r.error = err;
        r.output = "ERR: " + err;
        return r;
    }

    const std::string name = container_name_for(tenant_id);
    // Container-side deadline (GNU timeout when present) + parent-side
    // backstop.  Parent gets a small grace so the in-container timeout
    // can exit with 124 before we SIGKILL the docker-exec driver.
    const std::string wrapped =
        wrap_exec_with_timeout(command, cfg_.exec_timeout_seconds);
    std::vector<std::string> argv{
        cfg_.runtime, "exec",
        "-i",
        "--workdir", "/workspace",
        name,
        "sh", "-c", wrapped
    };
    const int parent_timeout = (cfg_.exec_timeout_seconds > 0)
        ? cfg_.exec_timeout_seconds + 2
        : 0;

    // When a workspace quota is configured, hold the same per-tenant mutex
    // as write_to_workspace for the entire docker exec so a concurrent
    // /write cannot grow the workspace between measure and flush (#136).
    // Post-exec measurement surfaces shell redirects that bypass /write.
    std::shared_ptr<std::mutex> quota_mu;
    std::unique_lock<std::mutex> quota_lk;
    const bool quota_enforced = cfg_.workspace_max_bytes > 0;
    if (quota_enforced) {
        quota_mu = start_mutex_for(tenant_id);
        quota_lk = std::unique_lock<std::mutex>(*quota_mu);
    }

    bool timed_out = false;
    bool truncated = false;
    int rc = run_capture(argv, parent_timeout,
                          static_cast<size_t>(cfg_.output_max_bytes),
                          r.output, timed_out, &truncated);
    // GNU timeout exits 124 on deadline; treat that as a clean timeout
    // even when the parent backstop did not fire.
    if (!timed_out && cfg_.exec_timeout_seconds > 0 && rc == 124) {
        timed_out = true;
    }
    r.timed_out  = timed_out;
    r.exit_status = rc;

    if (timed_out) {
        // Best-effort: kill leftover non-PID-1 processes so a timed-out
        // workload cannot keep burning CPU/memory inside the warm
        // container after docker-exec is gone.
        std::string kill_out;
        bool kill_to = false;
        run_capture(
            {cfg_.runtime, "exec", name, "sh", "-c",
             "for p in /proc/[0-9]*; do "
             "pid=${p#/proc/}; "
             "[ \"$pid\" = \"1\" ] && continue; "
             "kill -9 \"$pid\" 2>/dev/null || true; "
             "done"},
            /*timeout_seconds=*/5,
            /*output_cap=*/4096,
            kill_out, kill_to);
    }

    if (quota_enforced) {
        if (cfg_.quota_exec_pause_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg_.quota_exec_pause_ms));
        }
        const int64_t used = measure_workspace_bytes(tenant_id);
        if (used > cfg_.workspace_max_bytes) {
            r.ok = false;
            r.error = "workspace quota exceeded after /exec (" +
                      std::to_string(cfg_.workspace_max_bytes) +
                      " bytes); used " + std::to_string(used);
            if (r.output.empty()) r.output = "ERR: " + r.error;
            else r.output += "\nERR: " + r.error;
        }
    }
    if (metrics_) {
        metrics_->inc_sandbox_exec();
        if (timed_out) metrics_->inc_sandbox_exec_timeout();
    }

    // Self-heal: if the docker exec drove a Docker-level failure (the
    // container was killed mid-command, or the daemon lost track of
    // it), evict the tenant from `running_` so the NEXT /exec call
    // triggers a fresh `docker run`.  The current call's tool result
    // still surfaces the failure verbatim so the agent learns about
    // it; the recovery is for the next turn.  Patterns we treat as
    // "container is gone": exit 125 from docker-itself, OR stderr
    // containing the daemon's not-found / not-running phrasing.
    auto is_docker_lost = [](int code, const std::string& out) -> bool {
        if (code == 125) return true;
        return out.find("No such container") != std::string::npos
            || out.find("is not running")    != std::string::npos
            || out.find("is restarting")     != std::string::npos
            || out.find("is paused")         != std::string::npos;
    };
    if (rc != 0 && !timed_out && is_docker_lost(rc, r.output)) {
        std::lock_guard<std::mutex> lk(mu_);
        running_.erase(tenant_id);
    }

    // Match cmd_exec's output framing so the dispatcher's downstream
    // handling is identical.
    if (r.output.empty()) r.output = "(no output)";
    if (truncated) {
        const int kb =
            static_cast<int>((cfg_.output_max_bytes + 1023) / 1024);
        r.output += "\n... [truncated at " + std::to_string(kb) + " KB]";
    }
    if (timed_out) {
        r.output += "\n[timed out after " +
                    std::to_string(cfg_.exec_timeout_seconds) + "s]";
    } else if (rc != 0 && rc != -1) {
        r.output += "\n[exit " + std::to_string(rc) + "]";
    }
    if (rc == -1) {
        r.ok = false;
        r.error = r.output;
        // r.output already starts with "ERR: " from run_capture.
    }
    touch_access(tenant_id);
    return r;
}

bool SandboxManager::write_to_workspace(int64_t tenant_id,
                                         const std::string& rel_path,
                                         const std::string& content,
                                         std::string& err_out) {
    if (!usable_) { err_out = unusable_reason_; return false; }

    std::string clean;
    if (!sanitize_rel(rel_path, clean, err_out)) return false;

    // Serialize quota check + write for this tenant so parallel /write
    // children (e.g. from /parallel) cannot both pass measure_workspace
    // and exceed workspace_max_bytes.  Same mutex as ensure_container.
    auto tenant_mu = start_mutex_for(tenant_id);
    std::lock_guard<std::mutex> write_lk(*tenant_mu);

    std::string ws = ensure_workspace(tenant_id);
    if (ws.empty()) { err_out = "workspace mkdir failed"; return false; }

    fs::path target;
    if (!resolve_within_workspace(fs::path(ws), clean, target, err_out))
        return false;

    // Quota check.  Subtract any pre-existing entry's size before
    // testing the cap so an in-place overwrite of a 100 KB file with
    // 200 KB only "costs" 100 KB against the quota.  Workspace walk
    // is O(N files) per write — fine for typical agent workloads (a
    // few hundred files); operators with much larger workspaces will
    // want the future tracker rework.
    if (cfg_.workspace_max_bytes > 0) {
        std::error_code sec;
        int64_t existing = 0;
        if (fs::exists(target, sec) && fs::is_regular_file(target, sec)) {
            auto sz = fs::file_size(target, sec);
            if (!sec) existing = static_cast<int64_t>(sz);
        }
        int64_t current = measure_workspace_bytes(tenant_id);
        int64_t projected = current - existing +
                            static_cast<int64_t>(content.size());
        if (projected > cfg_.workspace_max_bytes) {
            err_out = "workspace quota exceeded (" +
                      std::to_string(cfg_.workspace_max_bytes) +
                      " bytes); used " + std::to_string(current) +
                      ", attempted " + std::to_string(content.size()) +
                      " (existing " + std::to_string(existing) + ")";
            return false;
        }
        if (cfg_.quota_check_pause_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg_.quota_check_pause_ms));
        }
    }

    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) { err_out = "mkdir parent: " + ec.message(); return false; }
    }
    // Refuse to follow a dangling or escape symlink at the leaf: open
    // with no follow isn't portable via ofstream, so re-check after
    // parent mkdir that the final path still resolves inside the root.
    {
        fs::path again;
        std::string again_err;
        if (!resolve_within_workspace(fs::path(ws), clean, again, again_err)) {
            err_out = again_err;
            return false;
        }
        target = std::move(again);
    }
    std::ofstream f(target, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!f.is_open()) {
        err_out = "open for writing failed: " + target.string();
        return false;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f.good()) { err_out = "write failed: " + target.string(); return false; }
    touch_access(tenant_id);
    return true;
}

int64_t SandboxManager::measure_workspace_bytes(int64_t tenant_id) const {
    std::string ws = workspace_path_for(tenant_id);
    std::error_code ec;
    if (!fs::exists(ws, ec) || !fs::is_directory(ws, ec)) return 0;
    int64_t total = 0;
    const int64_t kSaturation = cfg_.workspace_max_bytes > 0
        ? cfg_.workspace_max_bytes + 1
        : std::numeric_limits<int64_t>::max();
    for (auto& entry : fs::recursive_directory_iterator(ws, ec)) {
        if (ec) break;
        std::error_code sec;
        if (entry.is_regular_file(sec)) {
            auto sz = fs::file_size(entry.path(), sec);
            if (!sec) {
                const int64_t file_sz = static_cast<int64_t>(sz);
                if (total > kSaturation - file_sz) {
                    return kSaturation;
                }
                total += file_sz;
            }
        }
    }
    return total;
}

bool SandboxManager::read_from_workspace(int64_t tenant_id,
                                          const std::string& rel_path,
                                          std::string& content_out,
                                          std::string& mime_out,
                                          std::string& err_out) {
    if (!usable_) { err_out = unusable_reason_; return false; }

    std::string clean;
    if (!sanitize_rel(rel_path, clean, err_out)) return false;

    fs::path target;
    if (!resolve_within_workspace(fs::path(workspace_path_for(tenant_id)),
                                  clean, target, err_out))
        return false;
    std::error_code ec;
    if (!fs::exists(target, ec) || fs::is_directory(target, ec)) {
        err_out = "no such file in workspace: " + clean;
        return false;
    }
    // Regular files only — refuse to read through a symlink leaf that
    // somehow still points outside after the prefix check (defense in depth).
    if (fs::is_symlink(target, ec)) {
        fs::path again;
        std::string again_err;
        if (!resolve_within_workspace(fs::path(workspace_path_for(tenant_id)),
                                      clean, again, again_err)) {
            err_out = again_err;
            return false;
        }
        target = std::move(again);
    }
    std::ifstream f(target, std::ios::in | std::ios::binary);
    if (!f.is_open()) { err_out = "open for reading failed"; return false; }

    const int cap = cfg_.read_max_bytes;
    if (cap > 0) {
        std::error_code sz_ec;
        const auto fsz = fs::file_size(target, sz_ec);
        if (!sz_ec && fsz > static_cast<uintmax_t>(cap)) {
            err_out = "file too large to read (" +
                      std::to_string(static_cast<int64_t>(fsz)) +
                      " bytes; limit " + std::to_string(cap) + ")";
            return false;
        }
        std::array<char, 65536> buf{};
        std::ostringstream ss;
        std::ios::off_type total = 0;
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = static_cast<size_t>(f.gcount());
            if (n == 0) break;
            if (total < 0 ||
                static_cast<int64_t>(n) >
                    static_cast<int64_t>(cap) - total) {
                err_out = "file too large to read (limit " +
                          std::to_string(cap) + " bytes)";
                return false;
            }
            ss.write(buf.data(), static_cast<std::streamsize>(n));
            total += static_cast<std::ios::off_type>(n);
        }
        content_out = ss.str();
    } else {
        std::ostringstream ss;
        ss << f.rdbuf();
        content_out = ss.str();
    }
    mime_out    = mime_for(clean);
    touch_access(tenant_id);
    return true;
}

std::string SandboxManager::list_workspace(int64_t tenant_id) {
    std::string ws = workspace_path_for(tenant_id);
    std::error_code ec;
    if (!fs::exists(ws, ec) || !fs::is_directory(ws, ec)) return "";

    std::ostringstream out;
    // Stable order: lexicographic.  Recurse so subdirectories surface.
    std::vector<fs::path> files;
    for (auto& entry : fs::recursive_directory_iterator(ws, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    const int byte_cap  = cfg_.list_max_bytes;
    const int entry_cap = cfg_.list_max_files;
    size_t      listed_bytes = 0;
    size_t      listed_files = 0;
    bool        truncated           = false;
    bool        truncated_by_entries = false;
    for (auto& p : files) {
        if (entry_cap > 0 && listed_files >= static_cast<size_t>(entry_cap)) {
            truncated            = true;
            truncated_by_entries = true;
            break;
        }
        std::error_code sec;
        auto sz = fs::file_size(p, sec);
        if (sec) continue;
        std::error_code rec;
        auto rel = fs::relative(p, ws, rec);
        if (rec) continue;
        const std::string line =
            rel.string() + "  " + std::to_string(sz) + " bytes\n";
        if (byte_cap > 0 &&
            listed_bytes + line.size() > static_cast<size_t>(byte_cap)) {
            truncated = true;
            break;
        }
        out << line;
        listed_bytes += line.size();
        ++listed_files;
    }
    if (truncated) {
        // Report the cap that actually stopped the listing — entry-count
        // vs byte-budget — so operators/agents don't misread the trailer.
        if (truncated_by_entries) {
            out << "... [truncated — listing entry cap reached]\n";
        } else if (byte_cap >= 1024) {
            out << "... [truncated at " << (byte_cap / 1024) << " KB]\n";
        } else {
            out << "... [truncated at " << byte_cap << " bytes]\n";
        }
    }
    touch_access(tenant_id);
    return out.str();
}

void SandboxManager::stop_container(int64_t tenant_id) {
    // Serialize with ensure_container for this tenant so docker rm cannot
    // race a concurrent docker run on the same container name.
    auto tenant_mu = start_mutex_for(tenant_id);
    std::lock_guard<std::mutex> start_lk(*tenant_mu);
    std::string name;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = running_.find(tenant_id);
        if (it == running_.end()) return;
        name = it->second;
        running_.erase(it);
    }
    std::vector<std::string> argv{cfg_.runtime, "rm", "-f", name};
    std::string ignore;
    bool to = false;
    run_capture(argv, /*timeout_seconds=*/15, /*output_cap=*/1024, ignore, to);
}

void SandboxManager::stop_all() {
    // Sweep tenants with a start mutex as well as those in `running_`:
    // a cold-start in flight holds its tenant mutex but isn't registered
    // in `running_` until `docker run` returns.  stop_container blocks
    // on that mutex, so it waits out the start and then removes the
    // freshly started container instead of leaking it past shutdown.
    std::vector<int64_t> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
        ids.reserve(start_mu_.size() + running_.size());
        for (auto& kv : start_mu_) ids.push_back(kv.first);
        for (auto& kv : running_) {
            if (start_mu_.find(kv.first) == start_mu_.end()) {
                ids.push_back(kv.first);
            }
        }
    }
    for (int64_t id : ids) stop_container(id);
}

} // namespace arbiter
