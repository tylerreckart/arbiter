#include "tui/tty_guard.h"

#include "cli_helpers.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <execinfo.h>

namespace arbiter {

volatile sig_atomic_t g_tui_armed = 0;

namespace {

volatile sig_atomic_t g_winch = 0;

void sigwinch_handler(int) { g_winch = 1; }

// Saved cooked stdin attributes for async-signal-safe restore. Written once
// when raw mode is entered; only read from fatal handlers / RAII teardown.
struct termios g_saved_stdin_tm;
volatile sig_atomic_t g_have_saved_stdin_tm = 0;

// Precomputed path for async-signal-safe fatal logging (~/.arbiter/tui-fatal.log).
char g_fatal_log_path[1024];
volatile sig_atomic_t g_fatal_log_ready = 0;

// Append bytes; ignores partial-write shortfalls (best-effort diagnostics).
void fatal_log_write(int fd, const char* p, std::size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w <= 0) return;
        p += static_cast<std::size_t>(w);
        n -= static_cast<std::size_t>(w);
    }
}

void fatal_log_write_cstr(int fd, const char* s) {
    std::size_t n = 0;
    while (s[n] != '\0') ++n;
    fatal_log_write(fd, s, n);
}

void fatal_log_write_uint(int fd, unsigned long v) {
    char buf[32];
    char* p = buf + sizeof(buf);
    if (v == 0) {
        fatal_log_write(fd, "0", 1);
        return;
    }
    while (v > 0) {
        *--p = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    fatal_log_write(fd, p, static_cast<std::size_t>((buf + sizeof(buf)) - p));
}

const char* fatal_signal_name(int sig) {
    switch (sig) {
        case SIGTERM: return "SIGTERM";
        case SIGHUP:  return "SIGHUP";
        case SIGINT:  return "SIGINT";
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        default:      return "signal";
    }
}

// Async-signal-safe append to ~/.arbiter/tui-fatal.log. Path is prepared
// before handlers are installed; open/write/close only (no libc logging).
void log_fatal_event(const char* kind, int sig) {
    if (!g_fatal_log_ready) return;
    const int fd = ::open(g_fatal_log_path,
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    const auto now = static_cast<unsigned long>(::time(nullptr));
    fatal_log_write_cstr(fd, "tui-fatal ts=");
    fatal_log_write_uint(fd, now);
    fatal_log_write_cstr(fd, " kind=");
    fatal_log_write_cstr(fd, kind);
    if (sig != 0) {
        fatal_log_write_cstr(fd, " signal=");
        fatal_log_write_cstr(fd, fatal_signal_name(sig));
        fatal_log_write_cstr(fd, "(");
        fatal_log_write_uint(fd, static_cast<unsigned long>(sig));
        fatal_log_write_cstr(fd, ")");
    }
    fatal_log_write_cstr(fd, " tty_reset=");
    fatal_log_write_cstr(fd, g_tui_armed ? "yes" : "no");
    fatal_log_write_cstr(fd, "\n");
    // Best-effort stack breadcrumb. backtrace_symbols_fd avoids malloc;
    // backtrace itself is not strictly async-signal-safe but is the usual
    // compromise for crash diagnostics (and beats a one-line log alone).
    {
        void* frames[64];
        const int n = ::backtrace(frames, 64);
        if (n > 0) {
            fatal_log_write_cstr(fd, "backtrace:\n");
            ::backtrace_symbols_fd(frames, n, fd);
        }
    }
    (void)::close(fd);
}

// Best-effort tty reset for fatal signals / terminate. Only async-signal-safe
// calls (write, tcsetattr, signal, raise).
void emergency_tty_reset() {
    // Mouse family off, bracketed paste off, kitty keyboard pop, show cursor,
    // leave alternate screen. Idempotent if OpenTUI already ran its shutdown.
    static const char kReset[] =
        "\033[?1003l\033[?1002l\033[?1000l\033[?1006l"
        "\033[?2004l"
        "\033[<u"
        "\033[?25h\033[?1049l";
    (void)::write(STDOUT_FILENO, kReset, sizeof(kReset) - 1);
    if (g_have_saved_stdin_tm) {
        (void)::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_stdin_tm);
    }
}

void fatal_signal_handler(int sig) {
    log_fatal_event("signal", sig);
    if (g_tui_armed) emergency_tty_reset();
    // Restore default disposition and re-raise so the process still exits
    // with the expected status / core-dump behavior.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

void tui_terminate_handler() {
    log_fatal_event("terminate", 0);
    if (g_tui_armed) emergency_tty_reset();
    // Abort with default disposition so we don't re-enter fatal_signal_handler.
    ::signal(SIGABRT, SIG_DFL);
    std::abort();
}

}  // namespace

StdinRawModeGuard::StdinRawModeGuard() {
    if (::tcgetattr(STDIN_FILENO, &saved) != 0) return;
    active = true;
    g_saved_stdin_tm = saved;
    g_have_saved_stdin_tm = 1;
    struct termios raw = saved;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG | IEXTEN));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

StdinRawModeGuard::~StdinRawModeGuard() {
    // Always disarm: exception unwind skips the normal shutdown tail.
    g_tui_armed = 0;
    if (!active) return;
    // Drain while still raw/no-echo — bytes that arrive after ECHO is
    // restored get echoed by the kernel the instant they're received.
    drain_stdin_spurious(150);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    g_have_saved_stdin_tm = 0;
}

void install_tui_fatal_handlers() {
    // Prepare the log path before arming handlers so the signal path never
    // touches the heap or get_config_dir().
    {
        const std::string dir = get_config_dir();
        const int n = std::snprintf(g_fatal_log_path, sizeof(g_fatal_log_path),
                                    "%s/tui-fatal.log", dir.c_str());
        if (n > 0 && static_cast<std::size_t>(n) < sizeof(g_fatal_log_path)) {
            g_fatal_log_ready = 1;
        }
    }
    // Soft kills that previously left sticky SGR mouse in the host shell.
    ::signal(SIGTERM, fatal_signal_handler);
    ::signal(SIGHUP, fatal_signal_handler);
    ::signal(SIGINT, fatal_signal_handler);
    // Hard crashes / Zig panics (abort). Without these, performShutdownSequence
    // may leave the alt screen while raw termios + mouse stick — the
    // "crashed back to the CLI but input is still TUI" failure mode.
    ::signal(SIGSEGV, fatal_signal_handler);
    ::signal(SIGABRT, fatal_signal_handler);
    ::signal(SIGBUS, fatal_signal_handler);
    ::signal(SIGFPE, fatal_signal_handler);
    std::set_terminate(tui_terminate_handler);
}

void install_sigwinch_handler() {
    ::signal(SIGWINCH, sigwinch_handler);
}

bool consume_sigwinch() {
    if (!g_winch) return false;
    g_winch = 0;
    return true;
}

}  // namespace arbiter
