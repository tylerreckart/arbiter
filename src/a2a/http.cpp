// src/a2a/http.cpp — libcurl wrapper for the A2A client surface.
//
// Single-shot synchronous calls (rpc_call, http_get) and one streaming
// call (rpc_stream) that pipes the response body through an SseReader.
//
// Threading model: each call runs on the calling thread.  No global
// state; libcurl itself is global-init'd once at process start by
// arbiter's startup code.

#include "a2a/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>

namespace index_ai::a2a {

namespace {

// libcurl write callback for buffering a complete response body.
// CURL signals EOF by closing; our caller reads `body` after perform().
size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* dst = static_cast<std::string*>(userdata);
    dst->append(ptr, size * nmemb);
    return size * nmemb;
}

// Build a curl_slist from our HttpHeader vector.  Caller frees with
// curl_slist_free_all.  Adds Content-Type: application/json by default
// for POST callers; pass-through callers can include their own.
struct curl_slist* build_headers(const std::vector<HttpHeader>& extra,
                                  bool add_json_content_type,
                                  bool add_accept_sse) {
    struct curl_slist* h = nullptr;
    if (add_json_content_type) {
        h = curl_slist_append(h, "Content-Type: application/json");
    }
    if (add_accept_sse) {
        // Accept both event-stream (the streaming path) and JSON (the
        // unary error fallback that A2A servers can produce when a
        // pre-stream check fails).
        h = curl_slist_append(h, "Accept: text/event-stream, application/json");
    }
    for (auto& kv : extra) {
        std::string line = kv.name + ": " + kv.value;
        h = curl_slist_append(h, line.c_str());
    }
    return h;
}

// Common easy-handle setup so the three entry points share the same
// TLS / signal / user-agent posture.
void apply_common_opts(CURL* curl, long timeout_secs) {
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,         1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,   2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          timeout_secs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   std::min<long>(timeout_secs, 15L));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,        "arbiter-a2a/1.0");
}

// Streaming write context.  Holds a reference to the SseReader and the
// cancel flag; libcurl invokes the function with the userdata pointer
// on every chunk arrival.  Returning anything other than the input
// size signals an error to libcurl.
struct StreamCtx {
    SseReader*           reader;
    std::atomic<bool>*   cancel;
    std::string*         raw_body;
    bool                 aborted = false;
};

size_t write_to_sse(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    const size_t n = size * nmemb;

    // Stash the raw bytes for diagnostic surfacing.  Capped: a
    // misbehaving server that streams without ever firing a final
    // event shouldn't ramp memory unbounded.
    constexpr size_t kRawCap = 1 * 1024 * 1024;
    if (ctx->raw_body && ctx->raw_body->size() < kRawCap) {
        const size_t remaining = kRawCap - ctx->raw_body->size();
        ctx->raw_body->append(ptr, std::min(n, remaining));
    }

    // Cancel check: returning 0 from the write callback signals
    // CURLE_ABORTED_BY_CALLBACK to perform().  We check before feeding
    // so a cancel takes effect at the next chunk boundary.
    if (ctx->cancel && ctx->cancel->load(std::memory_order_acquire)) {
        ctx->aborted = true;
        return 0;
    }

    if (ctx->reader) ctx->reader->feed(ptr, n);
    return n;
}

} // namespace

HttpResponse rpc_call(const std::string& url,
                       const std::vector<HttpHeader>& extra_headers,
                       const std::string& body,
                       long timeout_secs) {
    HttpResponse out;
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "curl_easy_init failed";
        return out;
    }

    struct curl_slist* headers = build_headers(extra_headers,
                                                /*json_content=*/true,
                                                /*accept_sse=*/false);

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &out.body);
    apply_common_opts(curl, timeout_secs);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        out.error = curl_easy_strerror(rc);
    }
    return out;
}

HttpResponse http_get(const std::string& url,
                       const std::vector<HttpHeader>& extra_headers,
                       long timeout_secs) {
    HttpResponse out;
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "curl_easy_init failed";
        return out;
    }

    // GETs for agent cards; allow tenant bearers to flow through
    // extra_headers but don't force a Content-Type since there's no body.
    struct curl_slist* headers = build_headers(extra_headers,
                                                /*json_content=*/false,
                                                /*accept_sse=*/false);
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_HTTPGET,       1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &out.body);
    apply_common_opts(curl, timeout_secs);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        out.error = curl_easy_strerror(rc);
    }
    return out;
}

HttpResponse rpc_stream(const std::string& url,
                         const std::vector<HttpHeader>& extra_headers,
                         const std::string& body,
                         SseReader::EventCallback on_event,
                         std::atomic<bool>& cancel,
                         long timeout_secs) {
    HttpResponse out;
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "curl_easy_init failed";
        return out;
    }

    SseReader  reader(std::move(on_event));
    StreamCtx  ctx{&reader, &cancel, &out.body, false};

    struct curl_slist* headers = build_headers(extra_headers,
                                                /*json_content=*/true,
                                                /*accept_sse=*/true);

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_sse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &ctx);
    // Streaming-friendly tweaks: disable the libcurl response buffer so
    // callbacks fire as bytes arrive, and keep TCP_NODELAY on for
    // latency.  Without these the SSE consumer can stall waiting for
    // libcurl's coalesced flush.
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY,   1L);
    apply_common_opts(curl, timeout_secs);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status_code);

    // Drain any unterminated trailing event.  Spec says discard, but
    // for diagnostic flows where a server crashes mid-event we'd rather
    // surface the partial than lose it silently — set force_dispatch.
    reader.flush(/*force_dispatch=*/true);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc == CURLE_ABORTED_BY_CALLBACK && ctx.aborted) {
        // Caller-initiated cancel; not really an error.
        out.error = "canceled";
    } else if (rc != CURLE_OK) {
        out.error = curl_easy_strerror(rc);
    }
    return out;
}

} // namespace index_ai::a2a
