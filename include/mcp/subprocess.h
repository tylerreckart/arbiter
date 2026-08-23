#pragma once
// include/mcp/subprocess.h — Bidirectional pipe to a child process
//
// Spawns a child via fork/execvp, gives the parent newline-framed
// stdin/stdout access, and ensures the process is killed + reaped on
// destruction.  POSIX only — arbiter's binary itself is POSIX-only
// (epoll/kqueue paths in server.cpp), so no Windows path is needed.
//
// The parent never reads stderr: MCP servers MUST log to stderr only,
// so we redirect the child's stderr to ours (so an `npx` install
// progress bar surfaces in `arbiter --api --verbose` logs) without
// trying to interleave it on the protocol channel.

#include <chrono>
#include <optional>
#include <string>
#include <atomic>
#include <mutex>
#include <sys/types.h>
#include <vector>

namespace arbiter::mcp {

class Subprocess {
public:
    // Spawn `argv[0]` as the executable, with the remaining args.  `env_extra`
    // is appended after a scrubbed copy of the parent environment (secret-
    // shaped keys like *_API_KEY / ARBITER_* are stripped).  Useful for
    // passing PLAYWRIGHT_BROWSERS_PATH etc.  Throws std::runtime_error on
    // fork/exec failure (the child's exec failure is detected by an immediate
    // EOF on stdout, which manifests as recv_line returning std::nullopt
    // before any data — caller treats that as a startup failure).
    Subprocess(const std::vector<std::string>& argv,
                const std::vector<std::string>& env_extra = {});

    ~Subprocess();

    Subprocess(const Subprocess&)            = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    // Send one line.  '\n' is appended automatically.  Returns false on
    // a closed pipe (child exited).  SIGPIPE is blocked around the write
    // and any pending SIGPIPE is drained before the mask is restored, so
    // a dead child becomes EPIPE rather than killing the host process.
    bool send_line(const std::string& line);

    // Read one '\n'-terminated line from the child's stdout, with a
    // wall-clock deadline.  Returns std::nullopt on EOF (child exited),
    // timeout (deadline elapsed before a complete line arrived), cancel,
    // or any other read error.  The trailing '\n' is stripped from the
    // returned value.  When `cancel` is set, poll slices stay at 250ms
    // so Esc / HTTP cancel can abort a long tools/call.
    std::optional<std::string>
    recv_line(std::chrono::milliseconds timeout,
              std::atomic<bool>* cancel = nullptr);

    // True if the child is still alive.  Cheap — just a non-blocking
    // waitpid(WNOHANG).  Caches the exit status once observed.
    // Serialized with terminate() / destructor reap on mu_ so a
    // Manager liveness check cannot waitpid the same pid as an
    // in-flight cancel.
    bool alive() const;

    // Send SIGTERM, give the child up to `grace` to exit, then SIGKILL.
    // Idempotent — safe to call after the child has already exited.
    void terminate(std::chrono::milliseconds grace = std::chrono::milliseconds(500));

private:
    pid_t pid_      = -1;
    int   stdin_fd_ = -1;     // parent writes here → child stdin
    int   stdout_fd_ = -1;    // parent reads here ← child stdout

    std::string read_buf_;    // accumulator for partial lines

    // Guards pid_/exited_/exit_status_ and every waitpid of pid_.
    mutable std::mutex mu_;
    mutable bool exited_   = false;
    mutable int  exit_status_ = 0;

    // Move all bytes available on stdout into read_buf_, blocking up to
    // `timeout`.  Returns false on EOF / read error / timeout-with-no-bytes.
    // Repeated calls drain bytes incrementally until a '\n' surfaces.
    bool drain_into_buf(std::chrono::milliseconds timeout);
};

} // namespace arbiter::mcp
