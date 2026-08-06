#include "remote/connect_config.h"

#include <cctype>
#include <cstdlib>

namespace arbiter {

namespace {

std::string trim_copy(std::string_view in) {
    size_t b = 0;
    while (b < in.size() &&
           std::isspace(static_cast<unsigned char>(in[b])))
        ++b;
    size_t e = in.size();
    while (e > b &&
           std::isspace(static_cast<unsigned char>(in[e - 1])))
        --e;
    return std::string(in.substr(b, e - b));
}

bool is_connect_flag(std::string_view arg) {
    return arg == "--connect";
}

bool is_token_flag(std::string_view arg) {
    return arg == "--token";
}

} // namespace

bool argv_has_connect(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (is_connect_flag(argv[i])) return true;
    }
    return false;
}

RemoteConnectConfig parse_connect_argv(int argc, char* argv[]) {
    RemoteConnectConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (is_connect_flag(arg)) {
            if (i + 1 < argc) {
                const std::string_view next = argv[i + 1];
                // Allow `arbiter --connect --token …` (URL from env).
                if (!next.empty() && next[0] != '-') {
                    cfg.base_url = std::string(next);
                    ++i;
                }
            }
            continue;
        }
        if (is_token_flag(arg)) {
            if (i + 1 < argc) {
                cfg.token = argv[i + 1];
                ++i;
            }
            continue;
        }
    }
    return cfg;
}

std::string normalize_api_base_url(std::string_view raw) {
    std::string url = trim_copy(raw);
    if (url.empty()) return {};
    // Strip a single trailing slash repeatedly.
    while (!url.empty() && url.back() == '/') url.pop_back();
    const bool http  = url.rfind("http://", 0) == 0;
    const bool https = url.rfind("https://", 0) == 0;
    if (!http && !https) return {};
    // Reject empty host after scheme.
    const size_t scheme_end = http ? 7 : 8;
    if (url.size() <= scheme_end) return {};
    return url;
}

std::string api_display_host(std::string_view base_url) {
    // Strip scheme.
    std::string_view rest = base_url;
    if (rest.rfind("https://", 0) == 0) rest.remove_prefix(8);
    else if (rest.rfind("http://", 0) == 0) rest.remove_prefix(7);
    // Drop path / query if any slipped through.
    const auto slash = rest.find('/');
    if (slash != std::string_view::npos) rest = rest.substr(0, slash);
    const auto q = rest.find('?');
    if (q != std::string_view::npos) rest = rest.substr(0, q);
    return std::string(rest);
}

std::string resolve_remote_connect(RemoteConnectConfig& cfg) {
    if (cfg.base_url.empty()) {
        if (const char* env = std::getenv("ARBITER_API_URL"); env && *env) {
            cfg.base_url = env;
        }
    }
    if (cfg.token.empty()) {
        if (const char* env = std::getenv("ARBITER_API_TOKEN"); env && *env) {
            cfg.token = env;
        }
    }

    const std::string normalized = normalize_api_base_url(cfg.base_url);
    if (normalized.empty()) {
        return "invalid or missing API URL — pass --connect <https://host> "
               "or set ARBITER_API_URL";
    }
    cfg.base_url = normalized;
    cfg.display_host = api_display_host(cfg.base_url);
    if (cfg.display_host.empty()) {
        return "invalid API URL host";
    }
    if (cfg.token.empty()) {
        return "missing API token — pass --token <atr_…> or set ARBITER_API_TOKEN";
    }
    return {};
}

} // namespace arbiter
