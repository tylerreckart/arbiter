// tests/test_api_client_pool.cpp — Regression test for issue #48: the shared
// ApiClient must let concurrent callers stream in parallel, not serialize
// every request behind one global connection mutex.
//
// This lives in its own test binary on purpose.  provider_for() builds the
// provider registry once, lazily, reading OLLAMA_HOST at first use; a separate
// binary guarantees the registry initializes against the loopback server this
// test stands up rather than a default localhost:11434 already pinned by some
// earlier case in test_api_client.cpp.
//
// The proof of concurrency is a barrier, not a timing measurement, so it is
// deterministic — no sleeps in the pass path.  A tiny loopback HTTP server
// accepts two connections; each handler blocks until *both* are simultaneously
// open before replying.  With the per-provider connection pool, two concurrent
// stream() calls each lease a distinct Conn, both connect, the barrier
// releases, and both return.  With the old single-mutex design the second
// stream() can't even open its socket until the first fully returns, so the
// barrier never reaches two — the handlers fall through a generous safety
// timeout and the test observes a max overlap of 1 and fails cleanly (it does
// not hang).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "api_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// A minimal loopback server that accepts exactly `expected` connections and
// records the peak number open at once.  Each connection handler waits on a
// barrier until every expected connection is open (or a safety cap elapses)
// before sending a minimal HTTP/200 SSE reply and closing, so a client that
// truly overlaps its requests drives the peak to `expected`.
struct OverlapServer {
    int         listen_fd = -1;
    int         port      = 0;
    int         expected  = 2;

    // Pure atomics — no mutex.  A prior unique_lock + condition_variable
    // barrier tripped TSan "double lock" under halt_on_error=1 on CI
    // (mutex lived on the main thread stack); the overlap proof only
    // needs a peak counter and a wait-until-N-open gate.
    std::atomic<int> open_now{0};
    std::atomic<int> peak{0};

    std::thread              acceptor;
    std::vector<std::thread> handlers;

    void start() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd >= 0);
        int yes = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;  // ephemeral
        REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd, expected) == 0);

        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port = ntohs(addr.sin_port);

        handlers.reserve(static_cast<size_t>(expected));
        acceptor = std::thread([this] { accept_loop(); });
    }

    void accept_loop() {
        for (int i = 0; i < expected; ++i) {
            int cs = ::accept(listen_fd, nullptr, nullptr);
            if (cs < 0) break;
            handlers.emplace_back([this, cs] { handle(cs); });
        }
    }

    void handle(int cs) {
        // Drain whatever the client sends first so its write() can't block.
        char scratch[4096];
        ::recv(cs, scratch, sizeof(scratch), 0);

        const int now = open_now.fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev_peak = peak.load(std::memory_order_relaxed);
        while (now > prev_peak
               && !peak.compare_exchange_weak(prev_peak, now,
                                              std::memory_order_relaxed)) {
        }

        // Barrier: hold the connection open until every expected peer is
        // also open.  The 5s cap only trips if the client failed to
        // overlap (regression) — it keeps the test from hanging.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (open_now.load(std::memory_order_acquire) < expected
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }

        // Non-chunked body with no Content-Length: the client streams until
        // EOF, which the close() below delivers.  resp.ok stays true, so
        // stream() returns without retrying.
        static const char kReply[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "\r\n"
            "data: [DONE]\n\n";
        ::send(cs, kReply, sizeof(kReply) - 1, 0);
        ::shutdown(cs, SHUT_RDWR);
        ::close(cs);

        open_now.fetch_sub(1, std::memory_order_acq_rel);
    }

    void stop() {
        if (acceptor.joinable()) acceptor.join();
        for (auto& h : handlers)
            if (h.joinable()) h.join();
        if (listen_fd >= 0) ::close(listen_fd);
    }
};

} // namespace

TEST_CASE("concurrent stream() calls overlap instead of serializing (issue #48)") {
    OverlapServer server;
    server.expected = 2;
    server.start();

    // Point the ollama provider at the loopback server.  Set before any
    // provider_for() call in this binary so the lazily-built registry picks it
    // up; ollama needs no API key and uses the plaintext (http) path.
    const std::string host = "http://127.0.0.1:" + std::to_string(server.port);
    ::setenv("OLLAMA_HOST", host.c_str(), /*overwrite=*/1);

    arbiter::ApiClient client({});  // no keys needed for ollama

    auto make_call = [&] {
        arbiter::ApiRequest req;
        req.model    = "ollama/test-model";
        req.messages = {arbiter::Message{"user", "hi"}};
        client.stream(req, [](const std::string&) {});
    };

    std::thread a(make_call);
    std::thread b(make_call);
    a.join();
    b.join();
    server.stop();

    // Both requests must have reached the server, and both must have been
    // in flight at the same moment.  peak == 1 means the second call waited
    // for the first to finish — the serialization this fix removes.
    CHECK(server.peak.load() == 2);
}
