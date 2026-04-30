// index/src/quartermaster_client.cpp — see quartermaster_client.h
#include "quartermaster_client.h"

#include "json.h"

#include <curl/curl.h>
#include <openssl/sha.h>

#include <cstdio>
#include <iomanip>
#include <sstream>
#include <thread>

namespace index_ai {

namespace {

std::string sha256_hex(const std::string& s) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), d);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char c : d) ss << std::setw(2) << static_cast<int>(c);
    return ss.str();
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    // Hard-cap at 64 KiB — Quartermaster responses are tiny JSON; anything
    // larger is almost certainly a misconfigured proxy serving HTML.
    constexpr size_t kMax = 64 * 1024;
    if (out->size() + bytes > kMax) return 0;
    out->append(ptr, bytes);
    return bytes;
}

// Pull `int64_t` from a JSON field that may be a number or null.  Returns
// `null_sentinel` when the field is missing/null so the unlimited-tenant
// case (Quartermaster sends literal JSON null) round-trips faithfully.
int64_t get_i64_or(const std::shared_ptr<JsonValue>& obj,
                    const char* key,
                    int64_t null_sentinel = 0) {
    if (!obj || !obj->is_object()) return null_sentinel;
    auto v = obj->get(key);
    if (!v || v->is_null()) return null_sentinel;
    if (v->is_number()) return static_cast<int64_t>(v->as_number());
    return null_sentinel;
}

}  // namespace

QuartermasterClient::QuartermasterClient(std::string base_url)
    : base_url_(std::move(base_url)) {
    // Strip a trailing slash so callers can append "/v1/runtime/..." without
    // worrying about a double separator.  Empty stays empty.
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

int QuartermasterClient::post_json(const std::string& path,
                                    const std::string& body,
                                    std::string&        body_out,
                                    int                 timeout_seconds) {
    CURL* curl = curl_easy_init();
    if (!curl) return 0;

    const std::string url = base_url_ + path;
    body_out.clear();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Follow redirects sparingly.  Quartermaster is a back-office service
    // we control; redirects mean misconfiguration, not a CDN bounce.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    long status = 0;
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return 0;
    return static_cast<int>(status);
}

QuartermasterClient::AuthResult
QuartermasterClient::validate(const std::string& token) {
    AuthResult r;
    if (!enabled() || token.empty()) {
        r.error_code = "disabled";
        return r;
    }

    const std::string key = sha256_hex(token);
    const auto now = std::chrono::steady_clock::now();

    // Cache lookup — TTL drives the eviction window so a tenant
    // suspension lands within `ttl_seconds` of the back-office change.
    {
        std::lock_guard<std::mutex> lk(auth_mu_);
        auto it = auth_cache_.find(key);
        if (it != auth_cache_.end()) {
            if (it->second.expires_at > now) return it->second.result;
            auth_cache_.erase(it);
        }
    }

    auto reqv = jobj();
    reqv->as_object_mut()["token"] = jstr(token);
    const std::string body = json_serialize(*reqv);

    std::string resp_body;
    const int status = post_json("/v1/runtime/auth/validate", body, resp_body);
    r.http_status = status;

    if (status == 0) {
        r.error_code = "transport_error";
        return r;
    }

    std::shared_ptr<JsonValue> j;
    try { j = json_parse(resp_body); } catch (...) { /* leave j null */ }

    if (status == 200 && j && j->is_object()) {
        r.ok           = true;
        r.workspace_id = j->get_string("workspace_id");
        r.tenant_id    = j->get_string("tenant_id");
        r.ttl_seconds  = j->get_int("ttl_seconds", 60);

        // Cache only successful validations.  Errors should re-hit the
        // server immediately on the next attempt so a freshly-issued
        // token isn't shadowed by a stale 401.
        std::lock_guard<std::mutex> lk(auth_mu_);
        AuthEntry entry;
        entry.result     = r;
        entry.expires_at = now + std::chrono::seconds(std::max(1, r.ttl_seconds));
        auth_cache_[key] = std::move(entry);
        return r;
    }

    if (j && j->is_object()) {
        r.error_code = j->get_string("error");
        r.message    = j->get_string("message");
    }
    return r;
}

QuartermasterClient::QuotaResult
QuartermasterClient::check_quota(const std::string& workspace_id,
                                  const std::string& model,
                                  int est_input_tokens,
                                  int est_output_tokens,
                                  const std::string& request_id) {
    QuotaResult r;
    if (!enabled()) {
        r.ok    = true;
        r.allow = true;   // disabled-mode default-allow — caller proceeds
        return r;
    }

    auto reqv = jobj();
    auto& ro  = reqv->as_object_mut();
    ro["workspace_id"]      = jstr(workspace_id);
    ro["model"]             = jstr(model);
    ro["est_input_tokens"]  = jnum(static_cast<double>(est_input_tokens));
    ro["est_output_tokens"] = jnum(static_cast<double>(est_output_tokens));
    if (!request_id.empty()) ro["request_id"] = jstr(request_id);

    const std::string body = json_serialize(*reqv);
    std::string resp_body;
    const int status = post_json("/v1/runtime/quota/check", body, resp_body);
    r.http_status = status;

    if (status == 0) {
        // Transport failure — fail open so a flaky billing service can't
        // brick the runtime.  An operator alert on Quartermaster
        // unavailability is the right place to act on this; refusing
        // every request would amplify a single-service outage into a
        // total runtime outage.
        r.ok     = false;
        r.allow  = true;
        r.reason = "transport_error";
        return r;
    }

    std::shared_ptr<JsonValue> j;
    try { j = json_parse(resp_body); } catch (...) { /* leave j null */ }

    if (status == 200 && j && j->is_object()) {
        r.ok    = true;
        r.allow = j->get_bool("allow", false);
        if (!r.allow) {
            r.reason  = j->get_string("reason");
            r.message = j->get_string("message");
        }
        r.estimated_cost_uc = get_i64_or(j, "estimated_cost_micro_cents", 0);
        // -1 sentinel ⇒ unlimited (server sent literal null).
        r.plan_remaining_uc = get_i64_or(j, "plan_remaining_micro_cents", -1);
        r.credit_balance_uc = get_i64_or(j, "credit_balance_micro_cents", 0);
        r.total_budget_uc   = get_i64_or(j, "total_budget_micro_cents", -1);
        return r;
    }

    // Non-200 — propagate the error code and DENY by default.  Unlike a
    // transport error, a clean 4xx from Quartermaster is a definitive
    // "this isn't allowed" (unknown_workspace, invalid_input, etc.).
    r.ok    = true;
    r.allow = false;
    if (j && j->is_object()) {
        r.reason  = j->get_string("error");
        r.message = j->get_string("message");
    } else {
        r.reason  = "billing_error";
    }
    return r;
}

void QuartermasterClient::record_usage(const UsageRecord& rec) {
    if (!enabled()) return;

    // Build the payload synchronously so we don't capture-by-reference
    // anything that might outlive the caller's stack.
    auto reqv = jobj();
    auto& ro  = reqv->as_object_mut();
    ro["request_id"]    = jstr(rec.request_id);
    ro["workspace_id"]  = jstr(rec.workspace_id);
    ro["model"]         = jstr(rec.model);
    ro["input_tokens"]  = jnum(static_cast<double>(rec.input_tokens));
    ro["output_tokens"] = jnum(static_cast<double>(rec.output_tokens));
    if (rec.cached_tokens > 0)
        ro["cached_tokens"] = jnum(static_cast<double>(rec.cached_tokens));
    if (rec.duration_ms > 0)
        ro["duration_ms"]   = jnum(static_cast<double>(rec.duration_ms));
    if (!rec.agent_id.empty()) ro["agent_id"] = jstr(rec.agent_id);
    if (rec.depth > 0) ro["depth"] = jnum(static_cast<double>(rec.depth));

    std::string body = json_serialize(*reqv);
    std::string base = base_url_;

    // Detached worker — usage/record is fire-and-forget per the runtime
    // contract.  A 4xx/5xx here doesn't stop the turn; the next retry
    // (or the operator) will reconcile.  Detaching keeps the request
    // thread free for the next event.
    std::thread([base = std::move(base), body = std::move(body)]() {
        CURL* curl = curl_easy_init();
        if (!curl) return;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "content-type: application/json");
        headers = curl_slist_append(headers, "accept: application/json");

        const std::string url = base + "/v1/runtime/usage/record";
        std::string resp;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

        (void)curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }).detach();
}

} // namespace index_ai
