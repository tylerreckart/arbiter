// src/mcp/subprocess.cpp — POSIX fork+exec with piped stdin/stdout

#include "mcp/subprocess.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__GLIBC__)
#  include <stdio_ext.h>
#endif

extern char** environ;

namespace arbiter::mcp {

namespace {

// Move-or-die helper around dup2 — used inside the child where throwing
// is meaningless.  On failure we _exit so the parent sees an EOF on
// stdout and surfaces the spawn failure as a recv_line timeout.
void must_dup2(int old_fd, int new_fd) {
    while (::dup2(old_fd, new_fd) < 0) {
        if (errno == EINTR) continue;
        ::_exit(127);
    }
}

void set_cloexec(int fd) {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) return;
    ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// Discard inherited stdio FILE buffers without writing them.  After fork
// + dup2(pipe → stdout), a sanitizer runtime (or failed-exec path) can
// otherwise flush the parent's leftover stdout buffer — e.g. doctest's
// banner — into the MCP pipe, which recv_line then surfaces as a false
// protocol line.  _exit skips fflush; these purge APIs drop the bytes.
void purge_stdio_buffers() {
#if defined(__GLIBC__)
    __fpurge(stdin);
    __fpurge(stdout);
    __fpurge(stderr);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
    || defined(__NetBSD__)
    fpurge(stdin);
    fpurge(stdout);
    fpurge(stderr);
#endif
}

// True when an inherited env KEY should be stripped from MCP children.
// Registry `env_extra` is appended after the scrub and is allowed through
// even when the key would otherwise match — operators opt into those.
bool is_secret_env_key(std::string_view key) {
    if (key.empty()) return true;
    auto has_suffix = [&](std::string_view suf) {
        return key.size() >= suf.size() &&
               key.compare(key.size() - suf.size(), suf.size(), suf) == 0;
    };
    auto eq = [&](std::string_view exact) { return key == exact; };

    // Exact high-value names.
    if (eq("OPENROUTER_API_KEY") || eq("OPENAI_API_KEY") ||
        eq("ANTHROPIC_API_KEY") || eq("BRAVE_SEARCH_API_KEY") ||
        eq("ARBITER_ADMIN_TOKEN") || eq("ARBITER_SEARCH_API_KEY") ||
        eq("GITHUB_TOKEN") || eq("GH_TOKEN") || eq("NPM_TOKEN") ||
        eq("AWS_SECRET_ACCESS_KEY") || eq("AWS_SESSION_TOKEN") ||
        eq("AWS_ACCESS_KEY_ID") || eq("GOOGLE_API_KEY") ||
        eq("GEMINI_API_KEY"))
        return true;

    // Prefix families used by arbiter / common provider SDKs.
    if (key.rfind("ARBITER_", 0) == 0) return true;
    if (key.rfind("OPENROUTER_", 0) == 0) return true;

    // Suffix patterns: FOO_API_KEY, FOO_TOKEN, FOO_SECRET, FOO_PASSWORD.
    if (has_suffix("_API_KEY") || has_suffix("_APIKEY") ||
        has_suffix("_ACCESS_TOKEN") || has_suffix("_AUTH_TOKEN") ||
        has_suffix("_SECRET") || has_suffix("_SECRET_KEY") ||
        has_suffix("_PASSWORD") || has_suffix("_PASSWD") ||
        has_suffix("_PRIVATE_KEY"))
        return true;
    // Bare TOKEN / SECRET as a final path segment after underscore.
    if (has_suffix("_TOKEN") || has_suffix("_SECRET")) return true;
    return false;
}

} // namespace

Subprocess::Subprocess(const std::vector<std::string>& argv,
                        const std::vector<std::string>& env_extra) {
    if (argv.empty())
        throw std::runtime_error("Subprocess: empty argv");

    int in_pipe[2]  = {-1, -1};   // parent writes -> child reads
    int out_pipe[2] = {-1, -1};   // child writes  -> parent reads
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
        // If the second pipe failed, the first might be open — close it
        // before throwing so we don't leak fds in the API server's process.
        if (in_pipe[0]  >= 0) ::close(in_pipe[0]);
        if (in_pipe[1]  >= 0) ::close(in_pipe[1]);
        if (out_pipe[0] >= 0) ::close(out_pipe[0]);
        if (out_pipe[1] >= 0) ::close(out_pipe[1]);
        throw std::runtime_error(std::string("pipe failed: ") + ::strerror(errno));
    }

    // Flush before fork so the child does not inherit unflushed bytes that
    // could later land on the redirected stdout pipe (see purge_stdio_buffers).
    std::fflush(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]);  ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        throw std::runtime_error(std::string("fork failed: ") + ::strerror(errno));
    }

    if (pid == 0) {
        // ── Child ─────────────────────────────────────────────────────
        // Wire up stdio.  Close the parent ends after dup2 so a leftover
        // fd doesn't keep the pipe alive after the child exits.
        must_dup2(in_pipe[0],  STDIN_FILENO);
        must_dup2(out_pipe[1], STDOUT_FILENO);
        // stderr stays the parent's stderr — MCP servers' diagnostics
        // surface in arbiter logs without polluting the protocol channel.

        ::close(in_pipe[0]);  ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);

        purge_stdio_buffers();

        // Inherit parent environ minus secret-shaped keys, then append
        // registry env_extra.  MCP children (often untrusted npx packages)
        // must not see provider/admin credentials from the arbiter process.
        // Done this way (rather than execvpe) because macOS doesn't expose
        // execvpe and we need the PATH-search behaviour for `npx`.
        std::vector<std::string> env_storage;
        for (char** e = environ; e && *e; ++e) {
            const char* eq = std::strchr(*e, '=');
            std::string_view key = eq ? std::string_view(*e, static_cast<size_t>(eq - *e))
                                      : std::string_view(*e);
            if (is_secret_env_key(key)) continue;
            env_storage.emplace_back(*e);
        }
        for (auto& s : env_extra) env_storage.push_back(s);
        std::vector<char*> envp;
        envp.reserve(env_storage.size() + 1);
        for (auto& s : env_storage) envp.push_back(s.data());
        envp.push_back(nullptr);
        environ = envp.data();

        std::vector<char*> argp;
        argp.reserve(argv.size() + 1);
        // execvp expects char* (non-const); argv strings outlive exec
        // because the child's address space is its own copy after fork.
        for (auto& s : argv) argp.push_back(const_cast<char*>(s.c_str()));
        argp.push_back(nullptr);

        ::execvp(argp[0], argp.data());
        // execvp only returns on failure.
        ::_exit(127);
    }

    // ── Parent ─────────────────────────────────────────────────────
    // Close child ends; keep our halves of the pipe.  CLOEXEC so a
    // sibling fork/exec (e.g. /exec-disabled-but-an-internal-popen)
    // doesn't inherit the MCP channel.
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    pid_       = pid;
    stdin_fd_  = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    set_cloexec(stdin_fd_);
    set_cloexec(stdout_fd_);

    // Non-blocking on the read side so recv_line's poll-based deadline
    // works correctly without requiring a separate timer thread.
    int flags = ::fcntl(stdout_fd_, F_GETFL);
    if (flags >= 0) ::fcntl(stdout_fd_, F_SETFL, flags | O_NONBLOCK);
}

Subprocess::~Subprocess() {
    terminate(std::chrono::milliseconds(200));
    if (stdin_fd_  >= 0) ::close(stdin_fd_);
    if (stdout_fd_ >= 0) ::close(stdout_fd_);
}

bool Subprocess::send_line(const std::string& line) {
    if (stdin_fd_ < 0) return false;
    std::string buf = line;
    if (buf.empty() || buf.back() != '\n') buf.push_back('\n');
    const char* p = buf.data();
    size_t      n = buf.size();
    while (n > 0) {
        ssize_t k = ::write(stdin_fd_, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            // EPIPE/EBADF = child exited; broken pipe is normal during
            // shutdown.  Caller treats false as "session is dead".
            return false;
        }
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

bool Subprocess::drain_into_buf(std::chrono::milliseconds timeout) {
    if (stdout_fd_ < 0) return false;
    pollfd pfd{stdout_fd_, POLLIN, 0};
    int  ms = timeout.count() > INT32_MAX ? INT32_MAX
              : static_cast<int>(timeout.count());
    int rc = ::poll(&pfd, 1, ms);
    if (rc <= 0) return false;     // timeout or error
    if (!(pfd.revents & POLLIN) && !(pfd.revents & POLLHUP)) return false;

    char buf[4096];
    while (true) {
        ssize_t k = ::read(stdout_fd_, buf, sizeof(buf));
        if (k > 0) {
            read_buf_.append(buf, static_cast<size_t>(k));
            // Loop reads until EAGAIN — pulls everything currently
            // buffered without re-polling.
            continue;
        }
        if (k == 0) {
            // EOF — child closed stdout (or exited).  If we got bytes
            // earlier in this call they're in read_buf_; surface them
            // as a successful drain.
            if (read_buf_.find('\n') != std::string::npos) return true;
            return false;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // drained
        return false;
    }
}

std::optional<std::string>
Subprocess::recv_line(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        // Already have a complete line buffered?
        auto nl = read_buf_.find('\n');
        if (nl != std::string::npos) {
            std::string line = read_buf_.substr(0, nl);
            read_buf_.erase(0, nl + 1);
            // Strip a trailing '\r' for CRLF-emitting servers (rare on
            // POSIX but harmless to handle).
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return std::nullopt;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        if (!drain_into_buf(remaining)) {
            // Drain returned false — either EOF, timeout, or error.  If
            // we accumulated a partial that never got terminated, drop
            // it; the caller will get nullopt and treat the session as
            // failed.
            return std::nullopt;
        }
    }
}

bool Subprocess::alive() {
    if (exited_ || pid_ < 0) return false;
    int status = 0;
    pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0)  return true;       // still running
    if (r == pid_) {
        exited_      = true;
        exit_status_ = status;
        return false;
    }
    // r < 0 — ECHILD means already reaped (shouldn't happen for our pid),
    // anything else is unexpected.  Mark as exited so we stop polling.
    exited_ = true;
    return false;
}

void Subprocess::terminate(std::chrono::milliseconds grace) {
    if (pid_ < 0 || exited_) return;
    // Closing the write side first lets a well-behaved child exit on
    // EOF without needing a signal.  npm-installed CLIs usually do.
    if (stdin_fd_ >= 0) { ::close(stdin_fd_); stdin_fd_ = -1; }
    ::kill(pid_, SIGTERM);
    auto deadline = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_) { exited_ = true; exit_status_ = status; return; }
        if (r < 0)     { exited_ = true; return; }
        ::usleep(10 * 1000);
    }
    // Grace expired — escalate.
    ::kill(pid_, SIGKILL);
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0) {
        if (errno == EINTR) continue;
        break;
    }
    exited_      = true;
    exit_status_ = status;
}

} // namespace arbiter::mcp
