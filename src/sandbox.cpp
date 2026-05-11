// arbiter/src/sandbox.cpp

#include "sandbox.h"

#include <algorithm>
#include <cerrno>
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
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace arbiter {

namespace {

// Run argv to completion, capture combined stdout+stderr into `out`,
// and return the exit status.  Wall-clock parent-side timeout SIGKILLs
// the child when it elapses; `timed_out_out` is set in that case.
// `output_cap` caps `out` size; bytes past the cap are dropped on the
// floor (we keep reading so the child doesn't block on a full pipe).
//
// Returns -1 on infra failure (fork/exec); the `out` string holds
// "ERR: <reason>" in that case so callers can surface it verbatim.
int run_capture(const std::vector<std::string>& argv,
                int timeout_seconds,
                size_t output_cap,
                std::string& out,
                bool& timed_out_out) {
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
                // Drain any remaining bytes before declaring EOF.
                ssize_t k;
                while ((k = ::read(read_fd, buf, sizeof(buf))) > 0) {
                    if (out.size() < output_cap) {
                        size_t to_take = std::min(
                            static_cast<size_t>(k), output_cap - out.size());
                        out.append(buf, to_take);
                    }
                }
                ::close(read_fd);
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
}

SandboxManager::~SandboxManager() {
    stop_all();
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
    return out.find("true") == 0;
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

    std::lock_guard<std::mutex> lk(mu_);
    auto it = running_.find(tenant_id);
    if (it != running_.end()) {
        // We think it's running; verify and re-create if it died.
        if (container_is_running(it->second)) return true;
        running_.erase(it);
    } else if (container_is_running(name)) {
        // Stale survivor from a previous server run with the same workspace
        // root — re-attach.
        running_[tenant_id] = name;
        return true;
    }

    if (!start_container(tenant_id, err_out)) return false;
    running_[tenant_id] = name;
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
    std::vector<std::string> argv{
        cfg_.runtime, "exec",
        "-i",
        "--workdir", "/workspace",
        name,
        "sh", "-c", command
    };

    bool timed_out = false;
    int rc = run_capture(argv, cfg_.exec_timeout_seconds,
                          static_cast<size_t>(cfg_.output_max_bytes),
                          r.output, timed_out);
    r.timed_out  = timed_out;
    r.exit_status = rc;

    // Match cmd_exec's output framing so the dispatcher's downstream
    // handling is identical.
    if (r.output.empty()) r.output = "(no output)";
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
    return r;
}

bool SandboxManager::write_to_workspace(int64_t tenant_id,
                                         const std::string& rel_path,
                                         const std::string& content,
                                         std::string& err_out) {
    if (!usable_) { err_out = unusable_reason_; return false; }

    std::string clean;
    if (!sanitize_rel(rel_path, clean, err_out)) return false;

    std::string ws = ensure_workspace(tenant_id);
    if (ws.empty()) { err_out = "workspace mkdir failed"; return false; }

    fs::path target = fs::path(ws) / clean;
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) { err_out = "mkdir parent: " + ec.message(); return false; }
    }
    std::ofstream f(target, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!f.is_open()) {
        err_out = "open for writing failed: " + target.string();
        return false;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f.good()) { err_out = "write failed: " + target.string(); return false; }
    return true;
}

bool SandboxManager::read_from_workspace(int64_t tenant_id,
                                          const std::string& rel_path,
                                          std::string& content_out,
                                          std::string& mime_out,
                                          std::string& err_out) {
    if (!usable_) { err_out = unusable_reason_; return false; }

    std::string clean;
    if (!sanitize_rel(rel_path, clean, err_out)) return false;

    fs::path target = fs::path(workspace_path_for(tenant_id)) / clean;
    std::error_code ec;
    if (!fs::exists(target, ec) || fs::is_directory(target, ec)) {
        err_out = "no such file in workspace: " + clean;
        return false;
    }
    std::ifstream f(target, std::ios::in | std::ios::binary);
    if (!f.is_open()) { err_out = "open for reading failed"; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    content_out = ss.str();
    mime_out    = mime_for(clean);
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
    for (auto& p : files) {
        std::error_code sec;
        auto sz = fs::file_size(p, sec);
        if (sec) continue;
        std::error_code rec;
        auto rel = fs::relative(p, ws, rec);
        if (rec) continue;
        out << rel.string() << "  " << sz << " bytes\n";
    }
    return out.str();
}

void SandboxManager::stop_container(int64_t tenant_id) {
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
    std::vector<int64_t> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ids.reserve(running_.size());
        for (auto& kv : running_) ids.push_back(kv.first);
    }
    for (int64_t id : ids) stop_container(id);
}

} // namespace arbiter
