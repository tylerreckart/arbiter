#pragma once
// Raw stdin RAII + TUI fatal-signal restore. Used by the interactive REPL
// so a crash / SIGTERM cannot leave sticky DEC modes or raw termios in the
// host shell.

#include <csignal>
#include <termios.h>

namespace arbiter {

// Armed for the whole interactive Session lifetime (not just while mouse is
// on). Fatal paths must restore the host tty even when mouse was never
// enabled. Exposed so cmd_interactive can arm after Session::start.
extern volatile sig_atomic_t g_tui_armed;

// RAII raw-mode stdin. Restores cooked termios on every exit path
// (normal return, exception unwind) — matching SetupTui's destructor.
struct StdinRawModeGuard {
    struct termios saved{};
    bool active = false;

    StdinRawModeGuard();
    ~StdinRawModeGuard();

    StdinRawModeGuard(const StdinRawModeGuard&) = delete;
    StdinRawModeGuard& operator=(const StdinRawModeGuard&) = delete;
};

// Prepare ~/.arbiter/tui-fatal.log path and install SIGTERM/HUP/INT/SEGV/…
// handlers plus a terminate handler that emergency-resets the tty.
void install_tui_fatal_handlers();

// Install SIGWINCH → sets a flag drained by the output pump.
void install_sigwinch_handler();

// If a WINCH arrived since the last call, clear the flag and return true.
bool consume_sigwinch();

}  // namespace arbiter
