#include "remote/api_client.h"

#include "json.h"

#include <curl/curl.h>

#include <algorithm>
#include <sstream>

namespace arbiter {

namespace {

std::string json_error_message(const std::string& body) {
    if (body.empty()) return {};
    try {
        auto v = json_parse(body);
        if (v && v->is_object()) {
            auto err = v->get_string("error");
            if (!err.empty()) return err;
        }
    } catch (...) {}
    // Truncate raw body for stderr-friendly messages.
    if (body.size() > 200) return body.substr(0, 197) + "...";
    return body;
}

std::string http_fail(const a2a::HttpResponse& resp, const char* what) {
    if (!resp.error.empty() && resp.status_code == 0) {
        return std::string(what) + ": " + resp.error;
    }
    if (resp.status_code == 401) {
        std::string detail = json_error_message(resp.body);
        return detail.empty()
            ? "authentication failed (401) — check --token / ARBITER_API_TOKEN"
            : "authentication failed (401): " + detail;
    }
    if (resp.status_code == 403) {
        std::string detail = json_error_message(resp.body);
        return detail.empty()
            ? "forbidden (403) — tenant may be disabled"
            : "forbidden (403): " + detail;
    }
    if (resp.status_code == 0) {
        return std::string(what) + ": unreachable";
    }
    std::string detail = json_error_message(resp.body);
    std::ostringstream os;
    os << what << " (HTTP " << resp.status_code << ")";
    if (!detail.empty()) os << ": " << detail;
    return os.str();
}

} // namespace

RemoteApiClient::RemoteApiClient(RemoteConnectConfig cfg)
    : cfg_(std::move(cfg)) {}

std::vector<a2a::HttpHeader> RemoteApiClient::auth_headers() const {
    std::vector<a2a::HttpHeader> h;
    if (!cfg_.token.empty()) {
        h.push_back({"Authorization", "Bearer " + cfg_.token});
    }
    return h;
}

std::string RemoteApiClient::url(std::string_view path) const {
    std::string out = cfg_.base_url;
    if (!path.empty() && path[0] != '/') out.push_back('/');
    out.append(path);
    return out;
}

ConversationEntry
RemoteApiClient::conversation_from_json(const JsonValue& v) {
    ConversationEntry e;
    if (!v.is_object()) return e;
    const int64_t id = static_cast<int64_t>(v.get_number("id", 0));
    if (id > 0) e.id = std::to_string(id);
    e.title = v.get_string("title", "Untitled");
    if (e.title.empty()) e.title = "Untitled";
    e.created_at = static_cast<int64_t>(v.get_number("created_at", 0));
    e.updated_at = static_cast<int64_t>(v.get_number("updated_at", 0));
    e.total_tokens = static_cast<int>(
        v.get_number("input_tokens", 0) + v.get_number("output_tokens", 0));
    // Prefer message_count-derived signal; API may not ship token totals
    // on list rows — leave 0 if absent.
    if (auto folder = v.get("folder_id"); folder && folder->is_number()) {
        const auto fid = static_cast<int64_t>(folder->as_number());
        if (fid > 0) e.folder_id = std::to_string(fid);
    }
    if (v.get_bool("archived", false)) {
        // Soft-hide archived from the default sidebar by marking deleted.
        e.deleted_at = e.updated_at > 0 ? e.updated_at : 1;
    }
    e.titled = (e.title != "Untitled");
    return e;
}

a2a::HttpResponse RemoteApiClient::health(long timeout_secs) const {
    a2a::HttpCallOpts opts;
    opts.timeout_secs = timeout_secs;
    opts.ssrf_guard = false;
    return a2a::http_get(url("/v1/health"), /*extra=*/{}, opts);
}

RemoteBootstrapResult RemoteApiClient::bootstrap() const {
    RemoteBootstrapResult out;

    auto health_resp = health();
    if (health_resp.status_code != 200 || !health_resp.error.empty()) {
        out.error = http_fail(health_resp, "health check failed");
        return out;
    }

    // Auth + conversation probe.
    a2a::HttpCallOpts opts;
    opts.timeout_secs = 15;
    opts.ssrf_guard = false;
    auto conv_resp = a2a::http_get(url("/v1/conversations?limit=50"),
                                    auth_headers(), opts);
    if (conv_resp.status_code != 200 || !conv_resp.error.empty()) {
        out.error = http_fail(conv_resp, "conversation probe failed");
        return out;
    }

    try {
        auto body = json_parse(conv_resp.body);
        if (!body || !body->is_object()) {
            out.error = "conversation probe: invalid JSON";
            return out;
        }
        auto arr = body->get("conversations");
        if (arr && arr->is_array()) {
            for (auto& item : arr->as_array()) {
                if (!item) continue;
                auto e = conversation_from_json(*item);
                if (e.id.empty() || e.deleted_at != 0) continue;
                out.conversations.push_back(std::move(e));
            }
        }
    } catch (const std::exception& e) {
        out.error = std::string("conversation probe: ") + e.what();
        return out;
    }

    // Agents catalogue (best-effort — not required to enter the TUI).
    {
        std::string err;
        out.agents = list_agents(&err);
        (void)err;
    }

    if (out.conversations.empty()) {
        std::string err;
        const std::string id = create_conversation(&err, "Untitled");
        if (id.empty()) {
            out.error = err.empty() ? "could not create conversation" : err;
            return out;
        }
        ConversationEntry e;
        e.id = id;
        e.title = "Untitled";
        out.conversations.push_back(e);
        out.active_conversation_id = id;
    } else {
        // Newest-updated first from the API.
        out.active_conversation_id = out.conversations.front().id;
    }

    out.ok = true;
    return out;
}

std::vector<ConversationEntry>
RemoteApiClient::list_conversations(int limit) const {
    std::vector<ConversationEntry> out;
    a2a::HttpCallOpts opts;
    opts.timeout_secs = 15;
    opts.ssrf_guard = false;
    const std::string path =
        "/v1/conversations?limit=" + std::to_string(std::max(1, limit));
    auto resp = a2a::http_get(url(path), auth_headers(), opts);
    if (resp.status_code != 200 || !resp.error.empty()) return out;
    try {
        auto body = json_parse(resp.body);
        if (!body || !body->is_object()) return out;
        auto arr = body->get("conversations");
        if (!arr || !arr->is_array()) return out;
        for (auto& item : arr->as_array()) {
            if (!item) continue;
            auto e = conversation_from_json(*item);
            if (e.id.empty() || e.deleted_at != 0) continue;
            out.push_back(std::move(e));
        }
    } catch (...) {}
    return out;
}

std::string RemoteApiClient::create_conversation(std::string* error_out,
                                                  const std::string& title,
                                                  const std::string& agent_id) const {
    auto body = jobj();
    auto& m = body->as_object_mut();
    if (!title.empty()) m["title"] = jstr(title);
    if (!agent_id.empty()) m["agent_id"] = jstr(agent_id);
    a2a::HttpCallOpts opts;
    opts.timeout_secs = 15;
    opts.ssrf_guard = false;
    auto resp = a2a::rpc_call(url("/v1/conversations"), auth_headers(),
                               json_serialize(*body), opts);
    if (resp.status_code != 201 && resp.status_code != 200) {
        if (error_out) *error_out = http_fail(resp, "create conversation failed");
        return {};
    }
    try {
        auto parsed = json_parse(resp.body);
        if (!parsed || !parsed->is_object()) {
            if (error_out) *error_out = "create conversation: invalid JSON";
            return {};
        }
        const int64_t id = static_cast<int64_t>(parsed->get_number("id", 0));
        if (id <= 0) {
            if (error_out) *error_out = "create conversation: missing id";
            return {};
        }
        return std::to_string(id);
    } catch (const std::exception& e) {
        if (error_out) *error_out = std::string("create conversation: ") + e.what();
        return {};
    }
}

bool RemoteApiClient::delete_conversation(const std::string& id,
                                           std::string* error_out) const {
    // a2a::http has no DELETE helper — use rpc_call with a custom method
    // via curl is awkward; use GET-style workaround: libcurl DELETE.
    // Implement with a one-shot curl here.
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error_out) *error_out = "curl_easy_init failed";
        return false;
    }
    const std::string u = url("/v1/conversations/" + id);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!cfg_.token.empty()) {
        const std::string auth = "Authorization: Bearer " + cfg_.token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "arbiter-remote/1.0");
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        if (error_out) *error_out = curl_easy_strerror(rc);
        return false;
    }
    if (status != 200 && status != 204) {
        if (error_out) {
            a2a::HttpResponse fake;
            fake.status_code = status;
            fake.body = body;
            *error_out = http_fail(fake, "delete conversation failed");
        }
        return false;
    }
    return true;
}

bool RemoteApiClient::patch_conversation_title(const std::string& id,
                                                const std::string& title,
                                                std::string* error_out) const {
    auto body = jobj();
    body->as_object_mut()["title"] = jstr(title);
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error_out) *error_out = "curl_easy_init failed";
        return false;
    }
    const std::string u = url("/v1/conversations/" + id);
    const std::string payload = json_serialize(*body);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!cfg_.token.empty()) {
        const std::string auth = "Authorization: Bearer " + cfg_.token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    std::string resp_body;
    curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "arbiter-remote/1.0");
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        if (error_out) *error_out = curl_easy_strerror(rc);
        return false;
    }
    if (status != 200) {
        if (error_out) {
            a2a::HttpResponse fake;
            fake.status_code = status;
            fake.body = resp_body;
            *error_out = http_fail(fake, "rename conversation failed");
        }
        return false;
    }
    return true;
}

std::vector<RemoteMessage>
RemoteApiClient::list_messages(const std::string& conversation_id,
                                std::string* error_out,
                                int limit) const {
    std::vector<RemoteMessage> out;
    a2a::HttpCallOpts opts;
    opts.timeout_secs = 30;
    opts.ssrf_guard = false;
    const std::string path = "/v1/conversations/" + conversation_id +
        "/messages?limit=" + std::to_string(std::max(1, limit));
    auto resp = a2a::http_get(url(path), auth_headers(), opts);
    if (resp.status_code != 200 || !resp.error.empty()) {
        if (error_out) *error_out = http_fail(resp, "list messages failed");
        return out;
    }
    try {
        auto body = json_parse(resp.body);
        if (!body || !body->is_object()) {
            if (error_out) *error_out = "list messages: invalid JSON";
            return out;
        }
        auto arr = body->get("messages");
        if (!arr || !arr->is_array()) return out;
        for (auto& item : arr->as_array()) {
            if (!item || !item->is_object()) continue;
            RemoteMessage m;
            m.id = static_cast<int64_t>(item->get_number("id", 0));
            m.role = item->get_string("role");
            m.content = item->get_string("content");
            m.input_tokens = static_cast<int>(item->get_number("input_tokens", 0));
            m.output_tokens = static_cast<int>(item->get_number("output_tokens", 0));
            m.created_at = static_cast<int64_t>(item->get_number("created_at", 0));
            m.request_id = item->get_string("request_id");
            out.push_back(std::move(m));
        }
    } catch (const std::exception& e) {
        if (error_out) *error_out = std::string("list messages: ") + e.what();
    }
    return out;
}

std::vector<RemoteAgentInfo>
RemoteApiClient::list_agents(std::string* error_out) const {
    std::vector<RemoteAgentInfo> out;
    a2a::HttpCallOpts opts;
    opts.timeout_secs = 15;
    opts.ssrf_guard = false;
    auto resp = a2a::http_get(url("/v1/agents"), auth_headers(), opts);
    if (resp.status_code != 200 || !resp.error.empty()) {
        if (error_out) *error_out = http_fail(resp, "list agents failed");
        return out;
    }
    try {
        auto body = json_parse(resp.body);
        if (!body || !body->is_object()) return out;
        auto arr = body->get("agents");
        if (!arr || !arr->is_array()) return out;
        for (auto& item : arr->as_array()) {
            if (!item || !item->is_object()) continue;
            RemoteAgentInfo a;
            a.id = item->get_string("id");
            if (a.id.empty()) a.id = item->get_string("agent_id");
            a.name = item->get_string("name");
            a.role = item->get_string("role");
            a.model = item->get_string("model");
            if (!a.id.empty()) out.push_back(std::move(a));
        }
    } catch (const std::exception& e) {
        if (error_out) *error_out = std::string("list agents: ") + e.what();
    }
    return out;
}

a2a::HttpResponse RemoteApiClient::post_message_stream(
    const std::string& conversation_id,
    const std::string& message,
    a2a::SseReader::EventCallback on_event,
    std::atomic<bool>& cancel,
    long timeout_secs) const {
    auto body = jobj();
    body->as_object_mut()["message"] = jstr(message);
    a2a::HttpCallOpts opts;
    opts.timeout_secs = timeout_secs;
    opts.ssrf_guard = false;
    return a2a::rpc_stream(
        url("/v1/conversations/" + conversation_id + "/messages"),
        auth_headers(),
        json_serialize(*body),
        std::move(on_event),
        cancel,
        opts);
}

a2a::HttpResponse
RemoteApiClient::cancel_request(const std::string& request_id) const {
    a2a::HttpCallOpts opts;
    opts.timeout_secs = 10;
    opts.ssrf_guard = false;
    return a2a::rpc_call(
        url("/v1/requests/" + request_id + "/cancel"),
        auth_headers(),
        "{}",
        opts);
}

} // namespace arbiter
