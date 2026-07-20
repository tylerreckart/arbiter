#pragma once
// Shared SSRF connect-time guard for outbound libcurl clients (/fetch,
// A2A federation, etc.).  Applied via CURLOPT_OPENSOCKETFUNCTION so
// every connect — including after redirects — is re-validated.

#include <curl/curl.h>

#include <cstdint>
#include <string>

struct in6_addr;
struct sockaddr;

namespace arbiter {

bool ssrf_is_blocked_v4(uint32_t ip_host_order);
bool ssrf_is_blocked_v6(const struct in6_addr* a6);
bool ssrf_is_blocked_address(const struct sockaddr* sa);

// CURLOPT_OPENSOCKETFUNCTION entry point.  Returns CURL_SOCKET_BAD when
// the resolved peer is in a blocked range.
curl_socket_t ssrf_safe_opensocket(void* clientp, curlsocktype purpose,
                                   struct curl_sockaddr* addr);

// Install opensocket + http(s)-only protocol / redirect protocol limits
// on an easy handle.  Callers still set FOLLOWLOCATION themselves.
void curl_apply_ssrf_guard(CURL* curl);

// Pre-flight hostname resolution check for callers that cannot hook
// opensocket (external resolvers).  Empty string = ok; otherwise a
// short refusal reason.
std::string ssrf_preflight_url(const std::string& url);

} // namespace arbiter
