// arbiter/src/ssrf_guard.cpp — shared outbound SSRF denylist

#include "ssrf_guard.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace arbiter {

bool ssrf_is_blocked_v4(uint32_t ip_host_order) {
    const uint8_t a = (ip_host_order >> 24) & 0xff;
    const uint8_t b = (ip_host_order >> 16) & 0xff;
    if (a == 0)   return true;                       // 0.0.0.0/8
    if (a == 10)  return true;                       // 10/8 RFC1918
    if (a == 127) return true;                       // loopback
    if (a == 169 && b == 254) return true;           // link-local + AWS metadata
    if (a == 172 && b >= 16 && b <= 31) return true; // 172.16/12 RFC1918
    if (a == 192 && b == 168) return true;           // 192.168/16 RFC1918
    if (a == 100 && b >= 64 && b <= 127) return true;// 100.64/10 CGNAT
    if (a >= 224) return true;                       // multicast + reserved
    return false;
}

bool ssrf_is_blocked_v6(const struct in6_addr* a6) {
    const uint8_t* b = (const uint8_t*)a6;

    if (IN6_IS_ADDR_UNSPECIFIED(a6)) return true;
    if (IN6_IS_ADDR_LOOPBACK(a6))    return true;
    if (IN6_IS_ADDR_V4MAPPED(a6)) {
        uint32_t ip = ntohl(((const uint32_t*)a6)[3]);
        return ssrf_is_blocked_v4(ip);
    }
    {
        bool zero_high = true;
        for (int i = 0; i < 12; ++i) if (b[i]) { zero_high = false; break; }
        if (zero_high && (b[12] || b[13] || b[14] || b[15])) {
            uint32_t ip = ntohl(((const uint32_t*)a6)[3]);
            return ssrf_is_blocked_v4(ip);
        }
    }
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) {
        if (b[4] == 0x00 && b[5] == 0x00 &&
            b[6] == 0x00 && b[7] == 0x00 &&
            b[8] == 0x00 && b[9] == 0x00 &&
            b[10] == 0x00 && b[11] == 0x00) {
            uint32_t ip = ntohl(((const uint32_t*)a6)[3]);
            return ssrf_is_blocked_v4(ip);
        }
        if (b[4] == 0x00 && b[5] == 0x01) {
            uint32_t ip = ntohl(((const uint32_t*)a6)[3]);
            return ssrf_is_blocked_v4(ip);
        }
    }
    if (b[0] == 0x01 && b[1] == 0x00 &&
        b[2] == 0x00 && b[3] == 0x00 &&
        b[4] == 0x00 && b[5] == 0x00 &&
        b[6] == 0x00 && b[7] == 0x00) return true;
    if (b[0] == 0x20 && b[1] == 0x01 &&
        b[2] == 0x00 && b[3] == 0x00) {
        uint32_t ip = ntohl(((const uint32_t*)a6)[3]) ^ 0xffffffffu;
        if (ssrf_is_blocked_v4(ip)) return true;
    }
    if (b[0] == 0x20 && b[1] == 0x01 &&
        b[2] == 0x0d && b[3] == 0xb8) return true;
    if (b[0] == 0x20 && b[1] == 0x02) {
        uint32_t ip = (uint32_t(b[2]) << 24) | (uint32_t(b[3]) << 16) |
                      (uint32_t(b[4]) << 8)  |  uint32_t(b[5]);
        if (ssrf_is_blocked_v4(ip)) return true;
    }
    if (IN6_IS_ADDR_LINKLOCAL(a6)) return true;
    if (IN6_IS_ADDR_SITELOCAL(a6)) return true;
    if (IN6_IS_ADDR_MULTICAST(a6)) return true;
    if ((b[0] & 0xfe) == 0xfc)     return true;
    return false;
}

bool ssrf_is_blocked_address(const struct sockaddr* sa) {
    if (!sa) return true;
    if (sa->sa_family == AF_INET) {
        uint32_t ip = ntohl(((const struct sockaddr_in*)sa)->sin_addr.s_addr);
        return ssrf_is_blocked_v4(ip);
    }
    if (sa->sa_family == AF_INET6) {
        return ssrf_is_blocked_v6(&((const struct sockaddr_in6*)sa)->sin6_addr);
    }
    return true;
}

curl_socket_t ssrf_safe_opensocket(void* /*clientp*/, curlsocktype purpose,
                                   struct curl_sockaddr* addr) {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    if (ssrf_is_blocked_address(&addr->addr)) return CURL_SOCKET_BAD;
    return ::socket(addr->family, addr->socktype, addr->protocol);
}

void curl_apply_ssrf_guard(CURL* curl) {
    if (!curl) return;
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, ssrf_safe_opensocket);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
}

namespace {

std::string url_host(const std::string& url) {
    auto sep = url.find("://");
    if (sep == std::string::npos) return {};
    size_t start = sep + 3;
    size_t end = url.size();
    for (size_t i = start; i < url.size(); ++i) {
        if (url[i] == '/' || url[i] == '?' || url[i] == '#') { end = i; break; }
    }
    std::string authority = url.substr(start, end - start);
    auto at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);

    std::string host;
    if (!authority.empty() && authority.front() == '[') {
        auto rb = authority.find(']');
        if (rb == std::string::npos) return {};
        host = authority.substr(1, rb - 1);
    } else {
        auto colon = authority.find(':');
        host = authority.substr(0, colon);
    }
    for (auto& c : host) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    while (!host.empty() && host.back() == '.') host.pop_back();
    return host;
}

bool is_blocked_metadata_host(const std::string& host) {
    static const char* kBlocked[] = {
        "metadata.google.internal",
        "metadata.goog",
        "metadata",
        "instance-data",
        "instance-data.ec2.internal",
        "kubernetes.default.svc",
        "kubernetes.default.svc.cluster.local",
    };
    for (const auto* h : kBlocked) {
        if (host == h) return true;
    }
    return false;
}

} // namespace

std::string ssrf_preflight_url(const std::string& url) {
    const bool is_http  = url.size() >= 7 && url.compare(0, 7, "http://")  == 0;
    const bool is_https = url.size() >= 8 && url.compare(0, 8, "https://") == 0;
    if (!is_http && !is_https) return "URL must start with http:// or https://";

    std::string host = url_host(url);
    if (host.empty()) return "could not parse host from URL";

    if (is_blocked_metadata_host(host))
        return "host on metadata-service denylist";

    struct in_addr v4{};
    struct in6_addr v6{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        if (ssrf_is_blocked_v4(ntohl(v4.s_addr)))
            return "literal IPv4 address resolves to a blocked range";
    } else if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        if (ssrf_is_blocked_v6(&v6))
            return "literal IPv6 address resolves to a blocked range";
    }

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        if (res) freeaddrinfo(res);
        return std::string("could not resolve host: ") + gai_strerror(rc);
    }
    bool any_blocked = false;
    for (auto* p = res; p; p = p->ai_next) {
        if (ssrf_is_blocked_address(p->ai_addr)) { any_blocked = true; break; }
    }
    freeaddrinfo(res);
    if (any_blocked)
        return "host resolves to a private, loopback, link-local, or "
               "metadata-adjacent address";
    return {};
}

} // namespace arbiter
