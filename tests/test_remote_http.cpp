// tests/test_remote_http.cpp — Unary DELETE/PATCH body cap for
// RemoteApiClient (--connect).  GET/POST go through a2a::http; these two
// verbs used a one-shot curl write callback that appended unbounded.
// A loopback server streams past kRemoteHttpMaxBodyBytes; the client
// must abort and surface a size-limit error instead of buffering it.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "remote/api_client.h"

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
using arbiter::RemoteApiClient;
using arbiter::RemoteConnectConfig;
using arbiter::kRemoteHttpMaxBodyBytes;

namespace {

struct LoopbackServer {
    int         listen_fd = -1;
    int         port      = 0;
    std::thread acceptor;

    // When true, send Content-Length larger than the cap and a tiny body
    // so CURLOPT_MAXFILESIZE_LARGE can refuse before the write callback.
    bool advertise_oversize_length = false;
    // When true, omit Content-Length and stream just past the cap so the
    // write callback is the one that trips.
    bool stream_oversize_body      = false;
    // Default: small 204 (DELETE) / 200 (PATCH) JSON.
    bool reply_204                 = false;

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
                std::to_string(kRemoteHttpMaxBodyBytes + 1) +
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
            const size_t want = kRemoteHttpMaxBodyBytes + chunk.size();
            while (sent < want) {
                const ssize_t n = ::send(cs, chunk.data(), chunk.size(), 0);
                if (n <= 0) {
                    client_gone.store(true, std::memory_order_release);
                    break;
                }
                sent += static_cast<size_t>(n);
            }
        } else if (reply_204) {
            const char reply[] =
                "HTTP/1.1 204 No Content\r\n"
                "Content-Length: 0\r\n"
                "\r\n";
            ::send(cs, reply, sizeof(reply) - 1, 0);
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

RemoteApiClient make_client(int port) {
    RemoteConnectConfig cfg;
    cfg.base_url = "http://127.0.0.1:" + std::to_string(port);
    cfg.token = "atr_test";
    return RemoteApiClient(std::move(cfg));
}

} // namespace

TEST_CASE("delete_conversation accepts a small 204") {
    LoopbackServer srv;
    srv.reply_204 = true;
    srv.start();
    auto client = make_client(srv.port);
    std::string err;
    CHECK(client.delete_conversation("1", &err));
    CHECK(err.empty());
}

TEST_CASE("patch_conversation_title accepts a small 200 JSON body") {
    LoopbackServer srv;
    srv.start();
    auto client = make_client(srv.port);
    std::string err;
    CHECK(client.patch_conversation_title("1", "Renamed", &err));
    CHECK(err.empty());
}

TEST_CASE("delete_conversation refuses Content-Length over the unary cap") {
    LoopbackServer srv;
    srv.advertise_oversize_length = true;
    srv.start();
    auto client = make_client(srv.port);
    std::string err;
    CHECK_FALSE(client.delete_conversation("1", &err));
    CHECK(err.find("size limit") != std::string::npos);
}

TEST_CASE("patch_conversation_title refuses Content-Length over the unary cap") {
    LoopbackServer srv;
    srv.advertise_oversize_length = true;
    srv.start();
    auto client = make_client(srv.port);
    std::string err;
    CHECK_FALSE(client.patch_conversation_title("1", "x", &err));
    CHECK(err.find("size limit") != std::string::npos);
}

TEST_CASE("delete_conversation refuses a streamed body past the unary cap") {
    LoopbackServer srv;
    srv.stream_oversize_body = true;
    srv.start();
    auto client = make_client(srv.port);
    std::string err;
    CHECK_FALSE(client.delete_conversation("1", &err));
    CHECK(err.find("size limit") != std::string::npos);
}

TEST_CASE("patch_conversation_title refuses a streamed body past the unary cap") {
    LoopbackServer srv;
    srv.stream_oversize_body = true;
    srv.start();
    auto client = make_client(srv.port);
    std::string err;
    CHECK_FALSE(client.patch_conversation_title("1", "x", &err));
    CHECK(err.find("size limit") != std::string::npos);
}
