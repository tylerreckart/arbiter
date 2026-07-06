#include "repl/conversation_titling.h"

#include "styled_text.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace arbiter {

namespace {

std::string trim_to_display_width(std::string s, size_t max_cols) {
    while (!s.empty() && display_width(s) > max_cols) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
            s.pop_back();
        }
    }
    return s;
}

void ltrim(std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    s.erase(0, a);
}

void trim(std::string& s) {
    ltrim(s);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
}

bool is_writ_line(const std::string& trimmed_line) {
    return !trimmed_line.empty() && trimmed_line.front() == '/';
}

// Strips leading markdown structure (headers, list bullets, blockquotes)
// and any stray emphasis/code punctuation. Not a real markdown parser —
// just enough to keep those characters out of a one-line title.
std::string strip_markdown_markers(std::string line) {
    size_t hashes = 0;
    while (hashes < line.size() && line[hashes] == '#') ++hashes;
    if (hashes > 0 && hashes < line.size() && line[hashes] == ' ') {
        line.erase(0, hashes + 1);
    }
    ltrim(line);

    if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+')
        && line[1] == ' ') {
        line.erase(0, 2);
    } else if (!line.empty() && line[0] == '>') {
        line.erase(0, 1);
        ltrim(line);
    } else if (!line.empty() && std::isdigit(static_cast<unsigned char>(line[0]))) {
        size_t d = 0;
        while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
        if (d < line.size() && line[d] == '.' && d + 1 < line.size() && line[d + 1] == ' ') {
            line.erase(0, d + 2);
        }
    }

    std::string out;
    out.reserve(line.size());
    for (char c : line) {
        if (c == '`' || c == '*' || c == '_') continue;
        out += c;
    }
    return out;
}

std::string collapse_whitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool last_space = true; // swallow leading whitespace
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!last_space) out += ' ';
            last_space = true;
        } else {
            out += c;
            last_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

std::string deterministic_conversation_title(const std::string& first_user_message) {
    std::istringstream iss(first_user_message);
    std::string line;
    std::string joined;
    while (std::getline(iss, line)) {
        std::string trimmed = line;
        trim(trimmed);
        if (is_writ_line(trimmed)) continue;
        if (trimmed.rfind("```", 0) == 0) continue;
        std::string stripped = strip_markdown_markers(trimmed);
        if (!joined.empty() && !stripped.empty()) joined += ' ';
        joined += stripped;
    }

    joined = collapse_whitespace(joined);
    if (joined.empty()) return {};

    constexpr size_t kMaxCols = 40;
    if (display_width(joined) <= kMaxCols) return joined;

    // Word-boundary cut, reserving one column for the ellipsis.
    const size_t budget = kMaxCols - 1;
    std::istringstream words_iss(joined);
    std::string word, built;
    while (words_iss >> word) {
        std::string candidate = built.empty() ? word : (built + " " + word);
        if (display_width(candidate) > budget) break;
        built = std::move(candidate);
    }
    if (built.empty()) {
        // Even the first word alone doesn't fit — hard cut it.
        built = trim_to_display_width(joined, budget);
    }
    return built + "…";
}

std::string sanitize_model_title(const std::string& raw_reply) {
    std::istringstream iss(raw_reply);
    std::string s;
    std::getline(iss, s);
    trim(s);

    auto strip_pair = [&](char open, char close) {
        if (s.size() >= 2 && s.front() == open && s.back() == close) {
            s = s.substr(1, s.size() - 2);
        }
    };
    strip_pair('"', '"');
    strip_pair('\'', '\'');
    strip_pair('`', '`');
    strip_pair('*', '*');
    strip_pair('_', '_');
    trim(s);

    if (!s.empty()) {
        const char last = s.back();
        if (last == '.' || last == '!' || last == '?' || last == ':' || last == ';') {
            s.pop_back();
        }
    }
    trim(s);

    if (s.empty()) return {};
    return trim_to_display_width(s, 60);
}

std::string load_title_model_override(const std::string& config_dir) {
    std::ifstream f(config_dir + "/title_model");
    if (!f) return {};
    std::string m;
    std::getline(f, m);
    trim(m);
    return m;
}

} // namespace arbiter
