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
#include <sys/types.h>
#include <vector>

namespace arbiter::mcp {

class Subprocess {
public:
    // Spawn `argv[0]` as the executable, with the remaining args.  `env_extra`
    // is appended to the parent environment (KEY=VALUE strings), useful for
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
    // a closed pipe (child exited).
    bool send_line(const std::string& line);

    // Read one '\n'-terminated line from the child's stdout, with a
    // wall-clock deadline.  Returns std::nullopt on EOF (child exited),
    // timeout (deadline elapsed before a complete line arrived), or
    // any other read error.  The trailing '\n' is stripped from the
    // returned value.
    std::optional<std::string>
    recv_line(std::chrono::milliseconds timeout);

    // True if the child is still alive.  Cheap — just a non-blocking
    // waitpid(WNOHANG).  Caches the exit status once observed.
    bool alive();

    // Send SIGTERM, give the child up to `grace` to exit, then SIGKILL.
    // Idempotent — safe to call after the child has already exited.
    void terminate(std::chrono::milliseconds grace = std::chrono::milliseconds(500));

private:
    pid_t pid_      = -1;
    int   stdin_fd_ = -1;     // parent writes here → child stdin
    int   stdout_fd_ = -1;    // parent reads here ← child stdout

    std::string read_buf_;    // accumulator for partial lines

    bool exited_   = false;
    int  exit_status_ = 0;

    // Move all bytes available on stdout into read_buf_, blocking up to
    // `timeout`.  Returns false on EOF / read error / timeout-with-no-bytes.
    // Repeated calls drain bytes incrementally until a '\n' surfaces.
    bool drain_into_buf(std::chrono::milliseconds timeout);
};

} // namespace arbiter::mcp
