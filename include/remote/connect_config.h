#pragma once
// Remote TUI connect options — resolve --connect / --token and env
// fallbacks (ARBITER_API_URL / ARBITER_API_TOKEN).  Pure string helpers;
// no network I/O.

#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

struct RemoteConnectConfig {
    std::string base_url;   // scheme://host[:port], no trailing slash
    std::string token;      // atr_… bearer (required for multi-tenant --api)
    std::string display_host; // host[:port] for chrome (no credentials)
};

// True when argv includes --connect (with or without an inline URL).
bool argv_has_connect(int argc, char* argv[]);

// Parse --connect / --token from argv.  Empty base_url when --connect is
// present without a value and no env fallback — caller must reject.
// Does not read the environment; call resolve_remote_connect afterward.
RemoteConnectConfig parse_connect_argv(int argc, char* argv[]);

// Fill missing URL/token from ARBITER_API_URL / ARBITER_API_TOKEN.
// Normalizes base_url (trim trailing slashes, require http(s) scheme).
// Returns an error message on failure; empty string on success.
std::string resolve_remote_connect(RemoteConnectConfig& cfg);

// Normalize a user-supplied API base URL.  Empty on unrecoverable input.
std::string normalize_api_base_url(std::string_view raw);

// Host[:port] extracted from a normalized base URL for status chrome.
std::string api_display_host(std::string_view base_url);

} // namespace arbiter
