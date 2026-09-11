// tests/test_api_client_pool_cancel.cpp — Cancel must unblock a complete()
// parked on the per-provider connection-pool CV once kMaxConnsPerProvider
// slots are leased.  Own binary so provider_for() pins OLLAMA_HOST to this
// file's loopback HoldServer (same reason unit_api_client_pool is separate).
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
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Holds every accepted connection open until release_all().  Handlers are
// atomics-only (no mutex+CV) so TSan doesn't trip on the test fixture.
struct HoldServer {
    int listen_fd = -1;
    int port      = 0;

    std::atomic<int>  open_now{0};
    std::atomic<bool> release{false};
    std::atomic<bool> stop_accept{false};

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
        addr.sin_port        = 0;
        REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd, 32) == 0);

        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port = ntohs(addr.sin_port);

        acceptor = std::thread([this] { accept_loop(); });
    }

    void accept_loop() {
        while (!stop_accept.load(std::memory_order_acquire)) {
            int cs = ::accept(listen_fd, nullptr, nullptr);
            if (cs < 0) break;
            handlers.emplace_back([this, cs] { handle(cs); });
        }
    }

    void handle(int cs) {
        char scratch[4096];
        ::recv(cs, scratch, sizeof(scratch), 0);
        open_now.fetch_add(1, std::memory_order_acq_rel);

        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(2ms);
        }

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

    void release_all() {
        release.store(true, std::memory_order_release);
    }

    void arm_hold() {
        release.store(false, std::memory_order_release);
    }

    void stop() {
        stop_accept.store(true, std::memory_order_release);
        if (listen_fd >= 0) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
            listen_fd = -1;
        }
        release_all();
        if (acceptor.joinable()) acceptor.join();
        for (auto& h : handlers)
            if (h.joinable()) h.join();
    }
};

// Process-lifetime server.  Both cases share one port because provider_for()
// reads OLLAMA_HOST once.  arm_hold() resets the release bit between cases.
struct SharedHold {
    HoldServer server;
    SharedHold() {
        server.start();
        const std::string host = "http://127.0.0.1:" + std::to_string(server.port);
        ::setenv("OLLAMA_HOST", host.c_str(), /*overwrite=*/1);
    }
    ~SharedHold() { server.stop(); }
};

SharedHold& shared_hold() {
    static SharedHold h;
    return h;
}

// Must match ApiClient::kMaxConnsPerProvider (private).
constexpr int kPoolCap = 16;

void wait_until(const std::atomic<bool>& flag, std::chrono::milliseconds cap) {
    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (!flag.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
}

bool wait_open(HoldServer& server, int n, std::chrono::milliseconds cap) {
    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (server.open_now.load(std::memory_order_acquire) < n
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    return server.open_now.load(std::memory_order_acquire) >= n;
}

// Joins every worker even if a REQUIRE fires mid-case (~thread on a
// joinable handle would otherwise std::terminate).
struct SaturationGuard {
    HoldServer&              server;
    arbiter::ApiClient       client;
    std::vector<std::thread> holders;
    std::thread              waiter;

    explicit SaturationGuard(HoldServer& s) : server(s), client({}) {
        server.arm_hold();
        holders.reserve(static_cast<size_t>(kPoolCap));
        for (int i = 0; i < kPoolCap; ++i) {
            holders.emplace_back([this] {
                arbiter::ApiRequest req;
                req.model    = "ollama/test-model";
                req.messages = {arbiter::Message{"user", "hold"}};
                client.stream(req, [](const std::string&) {});
            });
        }
    }

    ~SaturationGuard() {
        server.release_all();
        if (waiter.joinable()) waiter.join();
        for (auto& t : holders)
            if (t.joinable()) t.join();
    }

    SaturationGuard(const SaturationGuard&) = delete;
    SaturationGuard& operator=(const SaturationGuard&) = delete;
};

} // namespace

TEST_CASE("per-token cancel unblocks complete() waiting on a saturated pool") {
    auto& hold = shared_hold();
    SaturationGuard g(hold.server);
    REQUIRE(wait_open(hold.server, kPoolCap, 5s));

    auto token = std::make_shared<arbiter::CancelToken>();
    std::atomic<bool> scope_ready{false};
    std::atomic<bool> waiter_done{false};
    arbiter::ApiResponse waiter_resp;

    g.waiter = std::thread([&] {
        arbiter::RequestCancelScope scope(g.client, token);
        scope_ready.store(true, std::memory_order_release);
        arbiter::ApiRequest req;
        req.model    = "ollama/test-model";
        req.messages = {arbiter::Message{"user", "waiter"}};
        waiter_resp = g.client.complete(req);
        waiter_done.store(true, std::memory_order_release);
    });

    wait_until(scope_ready, 2s);
    REQUIRE(scope_ready.load());
    // Let complete() reach the pool CV (or observe an already-cancelled token).
    std::this_thread::sleep_for(50ms);

    g.client.cancel(*token);
    wait_until(waiter_done, 2s);
    CHECK(waiter_done.load());
    CHECK(waiter_resp.error_type == "cancelled");
    CHECK(waiter_resp.ok == false);
}

TEST_CASE("process-wide cancel() unblocks a pool-CV waiter without a token") {
    auto& hold = shared_hold();
    SaturationGuard g(hold.server);
    REQUIRE(wait_open(hold.server, kPoolCap, 5s));

    std::atomic<bool> entered{false};
    std::atomic<bool> waiter_done{false};
    arbiter::ApiResponse waiter_resp;

    g.waiter = std::thread([&] {
        entered.store(true, std::memory_order_release);
        arbiter::ApiRequest req;
        req.model    = "ollama/test-model";
        req.messages = {arbiter::Message{"user", "waiter"}};
        waiter_resp = g.client.complete(req);
        waiter_done.store(true, std::memory_order_release);
    });

    wait_until(entered, 2s);
    std::this_thread::sleep_for(50ms);

    g.client.cancel();
    wait_until(waiter_done, 2s);
    CHECK(waiter_done.load());
    CHECK(waiter_resp.error_type == "cancelled");
    CHECK(waiter_resp.ok == false);
}
