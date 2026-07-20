#include "pty_harness.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// forkpty / openpty live in different headers depending on libc:
//   • glibc / Linux:  <pty.h>, link -lutil
//   • macOS / BSD:    <util.h> (libSystem, no extra link)
// Both expose the same prototype.
#if defined(__linux__)
#  include <pty.h>
#else
#  include <util.h>
#endif

namespace index_tests {

namespace {

// Stretch PTY deadlines under TSan (or ARBITER_TEST_TIME_SCALE).
// TSan + the full TUI routinely miss the fixed millisecond budgets that
// plain Debug / ASan CI is tuned for.  ASan is left unscaled — it already
// passes with the stock budgets, and read_for() always burns its full
// wait so a blanket 5x would inflate that leg for no gain.
int timeout_scale() {
    static const int scale = [] {
        if (const char* e = std::getenv("ARBITER_TEST_TIME_SCALE")) {
            int v = std::atoi(e);
            if (v > 0 && v <= 50) return v;
        }
#if defined(__SANITIZE_THREAD__)
        return 5;
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
        return 5;
#  else
        return 1;
#  endif
#else
        return 1;
#endif
    }();
    return scale;
}

// Make a fresh temp directory under /tmp with a unique suffix.  Caller owns
// it; we rm -rf in the session destructor.  Using mkdtemp so the name is
// actually unique even if parallel tests ever run.
static std::string make_temp_home() {
    char tmpl[] = "/tmp/index_test_XXXXXX";
    if (!mkdtemp(tmpl)) throw std::runtime_error("mkdtemp failed");
    return std::string(tmpl);
}

// Wipe a directory tree.  We only ever write into dirs we created, so
// shelling out to `rm -rf` is acceptable — the paths are constrained.
static void rm_rf(const std::string& path) {
    if (path.empty() || path[0] != '/') return;   // guard — never rm relative
    std::string cmd = "rm -rf '" + path + "'";
    (void)std::system(cmd.c_str());
}

} // namespace

int scale_timeout_ms(int ms) {
    if (ms <= 0) return ms;
    const int s = timeout_scale();
    if (s <= 1) return ms;
    if (ms > (std::numeric_limits<int>::max)() / s)
        return (std::numeric_limits<int>::max)();
    return ms * s;
}

PtySession::PtySession(int rows, int cols) : rows_(rows), cols_(cols) {
    home_dir_ = make_temp_home();
}

PtySession::PtySession(PtySession&& o) noexcept
    : master_fd_(o.master_fd_),
      pid_(o.pid_),
      rows_(o.rows_),
      cols_(o.cols_),
      env_(std::move(o.env_)),
      output_(std::move(o.output_)),
      home_dir_(std::move(o.home_dir_)),
      drain_thread_(std::move(o.drain_thread_)),
      drain_stop_(o.drain_stop_.load()) {
    o.master_fd_ = -1;
    o.pid_       = -1;
    o.home_dir_.clear();
    o.drain_stop_ = true;
}

PtySession& PtySession::operator=(PtySession&& o) noexcept {
    if (this == &o) return *this;
    stop_drain_thread();
    terminate();
    if (master_fd_ >= 0) ::close(master_fd_);
    rm_rf(home_dir_);

    master_fd_ = o.master_fd_;
    pid_       = o.pid_;
    rows_      = o.rows_;
    cols_      = o.cols_;
    env_       = std::move(o.env_);
    {
        std::lock_guard<std::mutex> lk(output_mu_);
        output_ = std::move(o.output_);
    }
    home_dir_  = std::move(o.home_dir_);
    drain_thread_ = std::move(o.drain_thread_);
    drain_stop_ = o.drain_stop_.load();

    o.master_fd_ = -1;
    o.pid_       = -1;
    o.home_dir_.clear();
    o.drain_stop_ = true;
    return *this;
}

PtySession::~PtySession() {
    stop_drain_thread();
    terminate();
    if (master_fd_ >= 0) ::close(master_fd_);
    if (!home_dir_.empty()) rm_rf(home_dir_);
}

void PtySession::start_drain_thread() {
    drain_stop_ = false;
    drain_thread_ = std::thread([this]() {
        while (!drain_stop_.load()) {
            drain_once(50);
        }
    });
}

void PtySession::stop_drain_thread() {
    drain_stop_ = true;
    if (drain_thread_.joinable()) drain_thread_.join();
}

void PtySession::env(const std::string& key, const std::string& value) {
    env_.emplace_back(key, value);
}

void PtySession::spawn(const std::vector<std::string>& argv) {
    if (argv.empty()) throw std::runtime_error("spawn: empty argv");

    // Defaults — tests can override by calling env() before spawn().
    // Set first so explicit env() wins via order of iteration below.
    // Sanitizer *_OPTIONS are forwarded from the parent when present so
    // ASan/TSan CI legs actually instrument the PTY-spawned arbiter
    // (spawn replaces the full environment and would otherwise drop them).
    bool has_home = false, has_key = false, has_term = false;
    bool has_asan = false, has_tsan = false, has_lsan = false, has_ubsan = false;
    for (auto& kv : env_) {
        if (kv.first == "HOME") has_home = true;
        if (kv.first == "OPENROUTER_API_KEY") has_key = true;
        if (kv.first == "TERM") has_term = true;
        if (kv.first == "ASAN_OPTIONS") has_asan = true;
        if (kv.first == "TSAN_OPTIONS") has_tsan = true;
        if (kv.first == "LSAN_OPTIONS") has_lsan = true;
        if (kv.first == "UBSAN_OPTIONS") has_ubsan = true;
    }
    if (!has_home) env_.emplace_back("HOME", home_dir_);
    if (!has_key)  env_.emplace_back("OPENROUTER_API_KEY", "dummy-key-no-network");
    if (!has_term) env_.emplace_back("TERM", "xterm-256color");
    auto forward_if_set = [&](bool already, const char* key) {
        if (already) return;
        if (const char* v = std::getenv(key)) env_.emplace_back(key, v);
    };
    forward_if_set(has_asan, "ASAN_OPTIONS");
    forward_if_set(has_tsan, "TSAN_OPTIONS");
    forward_if_set(has_lsan, "LSAN_OPTIONS");
    forward_if_set(has_ubsan, "UBSAN_OPTIONS");

    struct winsize ws{};
    ws.ws_row = (unsigned short)rows_;
    ws.ws_col = (unsigned short)cols_;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) throw std::runtime_error("forkpty failed");

    if (pid == 0) {
        // Child.  Replace environment with ours so the parent's env doesn't
        // leak API keys or HOME into test runs.
        std::vector<std::string> owned_env;
        owned_env.reserve(env_.size());
        std::vector<char*> envp;
        envp.reserve(env_.size() + 1);
        for (auto& kv : env_) {
            owned_env.push_back(kv.first + "=" + kv.second);
            envp.push_back(const_cast<char*>(owned_env.back().c_str()));
        }
        envp.push_back(nullptr);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);

        execve(cargv[0], cargv.data(), envp.data());
        // exec failure — surface enough to debug then die so the parent's
        // read() returns EOF and the test can report the failure.
        std::perror("execve");
        _exit(127);
    }

    // Parent.  Non-blocking master so drain_once can poll with a timeout.
    int flags = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, flags | O_NONBLOCK);

    master_fd_ = master;
    pid_       = pid;
    start_drain_thread();
}

int PtySession::drain_once(int timeout_ms) {
    if (master_fd_ < 0) return 0;
    struct pollfd pfd{master_fd_, POLLIN, 0};
    int n = ::poll(&pfd, 1, timeout_ms);
    if (n <= 0) return 0;
    if (!(pfd.revents & POLLIN)) return 0;

    char buf[4096];
    ssize_t got = ::read(master_fd_, buf, sizeof(buf));
    if (got <= 0) return 0;
    {
        std::lock_guard<std::mutex> lk(output_mu_);
        output_.append(buf, (size_t)got);
    }
    return (int)got;
}

std::string PtySession::read_for(int millis) {
    // Intentionally not scaled: read_for is used as a short settle/drain
    // window.  Scaling it 5x under TSan inflates every PTY test and can
    // change assertion semantics (e.g. "must NOT appear yet").  Deadline
    // waits go through read_until / wait_exited / scale_timeout_ms.
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count();
        int remaining = millis - (int)elapsed;
        if (remaining <= 0) break;
        drain_once(std::min(remaining, 50));
    }
    return output();
}

const std::string& PtySession::output() const {
    std::lock_guard<std::mutex> lk(output_mu_);
    output_snapshot_ = output_;
    return output_snapshot_;
}

std::string PtySession::read_until(const std::string& needle, int timeout_ms) {
    timeout_ms = scale_timeout_ms(timeout_ms);
    auto start = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lk(output_mu_);
            if (output_.find(needle) != std::string::npos) return output_;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count();
        int remaining = timeout_ms - (int)elapsed;
        if (remaining <= 0) {
            std::lock_guard<std::mutex> lk(output_mu_);
            throw std::runtime_error(
                "read_until: timeout waiting for '" + needle + "' after "
                + std::to_string(timeout_ms) + "ms; captured "
                + std::to_string(output_.size()) + " bytes");
        }
        drain_once(std::min(remaining, 50));
    }
}

void PtySession::send(const std::string& bytes) {
    if (master_fd_ < 0) return;
    const char* p = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        ssize_t n = ::write(master_fd_, p, remaining);
        if (n < 0) {
            if (errno == EAGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            break;
        }
        p         += n;
        remaining -= (size_t)n;
    }
}

std::string PtySession::strip_ansi(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ) {
        unsigned char c = (unsigned char)raw[i];
        if (c == 0x1B && i + 1 < raw.size()) {
            char next = raw[i + 1];
            if (next == '[') {
                // CSI: ESC [ <params> <final>.  Final byte is 0x40..0x7E.
                i += 2;
                while (i < raw.size() && (unsigned char)raw[i] < 0x40) ++i;
                if (i < raw.size()) ++i;   // eat final byte
                continue;
            }
            if (next == ']') {
                // OSC: ESC ] … ST (BEL or ESC \).
                i += 2;
                while (i < raw.size() && raw[i] != 0x07 && raw[i] != 0x1B) ++i;
                if (i < raw.size() && raw[i] == 0x07) ++i;
                else if (i + 1 < raw.size() && raw[i] == 0x1B && raw[i + 1] == '\\') i += 2;
                continue;
            }
            // Save/restore cursor (ESC 7 / ESC 8), charset selection, etc. —
            // 2-byte sequences we can drop wholesale.
            if (next == '7' || next == '8' || next == '=' || next == '>') { i += 2; continue; }
            // Unknown: skip just the ESC so we don't lose bytes silently.
            ++i;
            continue;
        }
        out.push_back((char)c);
        ++i;
    }
    return out;
}

void PtySession::terminate() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGTERM);

    // Wait up to ~500ms for clean exit.
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_ || r < 0) { pid_ = -1; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
}

bool PtySession::wait_exited(int timeout_ms) {
    if (pid_ <= 0) return true;
    timeout_ms = scale_timeout_ms(timeout_ms);
    auto start = std::chrono::steady_clock::now();
    while (true) {
        int status = 0;
        pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_ || r < 0) {
            pid_ = -1;
            return true;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace index_tests
