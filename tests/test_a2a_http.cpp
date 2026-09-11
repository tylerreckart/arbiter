// tests/test_a2a_http.cpp — Unary A2A HTTP body cap (#317's SSE caps
// did not cover rpc_call / http_get).  A loopback server streams past
// kHttpMaxBodyBytes; the client must abort and surface a size-limit
// error instead of buffering the whole payload.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "a2a/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using arbiter::a2a::http_get;
using arbiter::a2a::rpc_call;
using arbiter::a2a::kHttpMaxBodyBytes;

namespace {

struct LoopbackServer {
    int         listen_fd = -1;
    int         port      = 0;
    std::thread acceptor;

    // When true, send Content-Length larger than the cap and a tiny body
    // so CURLOPT_MAXFILESIZE_LARGE can refuse before the write callback.
    bool        advertise_oversize_length = false;
    // When true, omit Content-Length and stream just past the cap so the
    // write callback is the one that trips.
    bool        stream_oversize_body      = false;

    std::atomic<bool> client_gone{false};

    void start() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd >= 0);
        int yes = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd, 1) == 0);

        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                              &len) == 0);
        port = ntohs(addr.sin_port);

        acceptor = std::thread([this] { handle_one(); });
    }

    void handle_one() {
        int cs = ::accept(listen_fd, nullptr, nullptr);
        if (cs < 0) return;
        char scratch[4096];
        ::recv(cs, scratch, sizeof(scratch), 0);

        if (advertise_oversize_length) {
            const std::string hdr =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " +
                std::to_string(kHttpMaxBodyBytes + 1) +
                "\r\n\r\n{}";
            ::send(cs, hdr.data(), hdr.size(), 0);
        } else if (stream_oversize_body) {
            const char hdr[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "\r\n";
            ::send(cs, hdr, sizeof(hdr) - 1, 0);
            std::string chunk(64 * 1024, 'x');
            size_t sent = 0;
            const size_t want = kHttpMaxBodyBytes + chunk.size();
            while (sent < want) {
                const ssize_t n = ::send(cs, chunk.data(), chunk.size(), 0);
                if (n <= 0) {
                    client_gone.store(true, std::memory_order_release);
                    break;
                }
                sent += static_cast<size_t>(n);
            }
        } else {
            const char reply[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 2\r\n"
                "\r\n"
                "{}";
            ::send(cs, reply, sizeof(reply) - 1, 0);
        }
        ::close(cs);
    }

    ~LoopbackServer() {
        if (listen_fd >= 0) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
        }
        if (acceptor.joinable()) acceptor.join();
    }
};

} // namespace

TEST_CASE("http_get accepts a small JSON body") {
    LoopbackServer srv;
    srv.start();
    const std::string url = "http://127.0.0.1:" + std::to_string(srv.port) + "/";
    auto r = http_get(url, {}, /*timeout=*/5, /*ssrf_guard=*/false);
    CHECK(r.error.empty());
    CHECK(r.status_code == 200);
    CHECK(r.body == "{}");
}

TEST_CASE("http_get refuses Content-Length over the unary cap") {
    LoopbackServer srv;
    srv.advertise_oversize_length = true;
    srv.start();
    const std::string url = "http://127.0.0.1:" + std::to_string(srv.port) + "/";
    auto r = http_get(url, {}, /*timeout=*/5, /*ssrf_guard=*/false);
    CHECK(r.error.find("size limit") != std::string::npos);
    CHECK(r.body.empty());
}

TEST_CASE("rpc_call refuses a streamed body past the unary cap") {
    LoopbackServer srv;
    srv.stream_oversize_body = true;
    srv.start();
    const std::string url = "http://127.0.0.1:" + std::to_string(srv.port) + "/";
    auto r = rpc_call(url, {}, "{}", /*timeout=*/10, /*ssrf_guard=*/false);
    CHECK(r.error.find("size limit") != std::string::npos);
    CHECK(r.body.empty());
}
