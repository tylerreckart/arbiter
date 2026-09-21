// arbiter/src/api/http_util.cpp

#include "api/http_util.h"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> out;
    auto q = path.find('?');
    std::string p = (q == std::string::npos) ? path : path.substr(0, q);
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { out += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i+1]), lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Parse "a=1&b=hello%20world" into a flat map.  Last value wins on repeats;
// values that fail url-decode are silently dropped.
std::map<std::string, std::string> parse_query(const std::string& path) {
    std::map<std::string, std::string> out;
    auto q = path.find('?');
    if (q == std::string::npos) return out;
    std::string qs = path.substr(q + 1);
    size_t start = 0;
    while (start < qs.size()) {
        size_t amp = qs.find('&', start);
        size_t end = (amp == std::string::npos) ? qs.size() : amp;
        auto eq = qs.find('=', start);
        if (eq != std::string::npos && eq < end) {
            out[url_decode(qs.substr(start, eq - start))] =
                url_decode(qs.substr(eq + 1, end - eq - 1));
        }
        start = (amp == std::string::npos) ? qs.size() : amp + 1;
    }
    return out;
}

std::string parse_bearer_authorization(const std::string& field_value) {
    // RFC 9110 §11.1: authentication schemes are case-insensitive.
    // RFC 6750 §2.1: credentials = "Bearer" 1*SP token68
    //
    // A literal "Bearer " prefix 401s clients that send `bearer`,
    // `BEARER`, or two spaces after the scheme.  Trailing OWS on
    // the field value is stripped so a proxy-appended space does not
    // become part of the token hash.
    static constexpr char kScheme[] = "bearer";
    static constexpr size_t kSchemeLen = 6;
    if (field_value.size() < kSchemeLen + 2) return {};
    for (size_t i = 0; i < kSchemeLen; ++i) {
        const unsigned char c = static_cast<unsigned char>(field_value[i]);
        if (static_cast<char>(std::tolower(c)) != kScheme[i]) return {};
    }
    size_t i = kSchemeLen;
    if (i >= field_value.size() || field_value[i] != ' ') return {};
    while (i < field_value.size() && field_value[i] == ' ') ++i;
    if (i >= field_value.size()) return {};
    size_t end = field_value.size();
    while (end > i && (field_value[end - 1] == ' ' || field_value[end - 1] == '\t'))
        --end;
    if (end <= i) return {};
    return field_value.substr(i, end - i);
}

} // namespace arbiter
