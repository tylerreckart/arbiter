#pragma once
// include/a2a/http.h — Tiny HTTP client for the A2A consumer side.
//
// libcurl wrapper covering the two shapes arbiter's A2A client needs:
//
//   • rpc_call   — synchronous JSON-RPC POST.  Response body returned
//                  whole; caller parses it as a JSON-RPC envelope.
//
//   • rpc_stream — streaming JSON-RPC POST (SSE response).  Each fully
//                  formed SSE event fires the on_event callback inline
//                  on the curl write thread.  A cancel flag checked
//                  between events lets callers interrupt cleanly.
//
//   • http_get   — plain GET, used to fetch agent cards from
//                  /.well-known/agent-card.json or per-agent endpoints.
//
// All functions are blocking on the calling thread.  curl_global_init
// is the caller's responsibility — arbiter calls it once at startup.

#include "a2a/sse_reader.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace arbiter::a2a {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpResponse {
    long          status_code = 0;          // 0 ⇒ transport failure (see error)
    std::string   body;                     // empty on streaming calls
    std::string   error;                    // non-empty ⇒ error context
};

// Shared call options.  `ssrf_guard` defaults on for A2A / agent-driven
// fetches.  Operator-chosen endpoints (TUI `--connect`) pass false so a
// home-server / LAN / loopback API URL is reachable.
struct HttpCallOpts {
    long timeout_secs = 60;
    bool ssrf_guard   = true;
};

// Synchronous JSON POST.  `body` ships verbatim with Content-Type
// application/json (caller adds extra headers as needed).  Timeout is
// total request time in seconds.
HttpResponse rpc_call(const std::string& url,
                       const std::vector<HttpHeader>& extra_headers,
                       const std::string& body,
                       long timeout_secs = 60,
                       bool ssrf_guard = true);

inline HttpResponse rpc_call(const std::string& url,
                              const std::vector<HttpHeader>& extra_headers,
                              const std::string& body,
                              HttpCallOpts opts) {
    return rpc_call(url, extra_headers, body, opts.timeout_secs, opts.ssrf_guard);
}

// Plain GET, used for agent-card discovery.  `accept` defaults to JSON.
HttpResponse http_get(const std::string& url,
                       const std::vector<HttpHeader>& extra_headers,
                       long timeout_secs = 30,
                       bool ssrf_guard = true);

inline HttpResponse http_get(const std::string& url,
                              const std::vector<HttpHeader>& extra_headers,
                              HttpCallOpts opts) {
    return http_get(url, extra_headers, opts.timeout_secs, opts.ssrf_guard);
}

// Streaming POST.  Headers, body, and timeout match rpc_call.  Each
// fully-parsed SSE event fires `on_event(name, data)`.  After every
// event the function checks `cancel` and aborts cleanly when set —
// libcurl returns CURLE_ABORTED_BY_CALLBACK and the response body
// reflects whatever arrived before the cancel.
//
// The status_code on the returned HttpResponse is the HTTP status from
// the response *headers* (200 for a successful stream).  body is the
// *raw* response bytes (mostly useless for the caller but exposed for
// diagnostics — SSE consumers care only about parsed events).  error
// is non-empty on transport failure.
HttpResponse rpc_stream(const std::string& url,
                         const std::vector<HttpHeader>& extra_headers,
                         const std::string& body,
                         SseReader::EventCallback on_event,
                         std::atomic<bool>& cancel,
                         long timeout_secs = 600,
                         bool ssrf_guard = true);

inline HttpResponse rpc_stream(const std::string& url,
                                const std::vector<HttpHeader>& extra_headers,
                                const std::string& body,
                                SseReader::EventCallback on_event,
                                std::atomic<bool>& cancel,
                                HttpCallOpts opts) {
    return rpc_stream(url, extra_headers, body, std::move(on_event), cancel,
                      opts.timeout_secs, opts.ssrf_guard);
}

} // namespace arbiter::a2a
