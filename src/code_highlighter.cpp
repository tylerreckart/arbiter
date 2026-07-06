#include "code_highlighter.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace arbiter {

namespace {

struct Region {
    std::size_t begin = 0;
    std::size_t end = 0;
    StyleId id = StyleId::Code;
    int priority = 0;
};

bool is_ident_start(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) || c == '_';
}

bool is_ident_char(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_';
}

void add_region(std::vector<Region>& regions, Region r) {
    if (r.end <= r.begin) return;
    regions.push_back(r);
}

void mark_comments(std::string_view text, std::string_view lang, std::vector<Region>& out) {
    const bool hash_comment = (lang == "python" || lang == "shell" || lang == "yaml"
                                 || lang == "toml" || lang == "ruby" || lang == "perl"
                                 || lang == "r" || lang == "generic");
    if (hash_comment) {
        const auto pos = text.find('#');
        if (pos != std::string_view::npos) {
            add_region(out, {pos, text.size(), StyleId::CodeComment, 100});
        }
        if (lang != "generic") return;
    }

    if (lang == "sql") {
        const auto pos = text.find("--");
        if (pos != std::string_view::npos) {
            add_region(out, {pos, text.size(), StyleId::CodeComment, 100});
        }
        return;
    }

    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '/' && text[i + 1] == '/') {
            add_region(out, {i, text.size(), StyleId::CodeComment, 100});
            return;
        }
    }
}

void mark_strings(std::string_view text, std::vector<Region>& out) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char q = text[i];
        if (q != '"' && q != '\'') continue;
        std::size_t j = i + 1;
        while (j < text.size()) {
            if (text[j] == '\\' && j + 1 < text.size()) {
                j += 2;
                continue;
            }
            if (text[j] == q) {
                j += 1;
                break;
            }
            ++j;
        }
        add_region(out, {i, j, StyleId::CodeString, 90});
        i = j > i ? j - 1 : j;
    }
}

bool region_covers(const std::vector<Region>& regions, std::size_t pos) {
    for (const Region& r : regions) {
        if (pos >= r.begin && pos < r.end) return true;
    }
    return false;
}

const std::unordered_set<std::string>& keywords_for(std::string_view lang) {
    static const std::unordered_set<std::string> kCpp = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
        "bool", "break", "case", "catch", "char", "class", "const", "constexpr",
        "consteval", "constinit", "const_cast", "continue", "co_await", "co_return",
        "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
        "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
        "private", "protected", "public", "register", "reinterpret_cast", "requires",
        "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while",
        "xor", "xor_eq",
    };
    static const std::unordered_set<std::string> kPython = {
        "False", "None", "True", "and", "as", "assert", "async", "await", "break",
        "class", "continue", "def", "del", "elif", "else", "except", "finally",
        "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
        "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
    };
    static const std::unordered_set<std::string> kJs = {
        "async", "await", "break", "case", "catch", "class", "const", "continue",
        "debugger", "default", "delete", "do", "else", "export", "extends", "false",
        "finally", "for", "function", "if", "import", "in", "instanceof", "let",
        "new", "null", "of", "return", "super", "switch", "this", "throw", "true",
        "try", "typeof", "undefined", "var", "void", "while", "with", "yield",
    };
    static const std::unordered_set<std::string> kGo = {
        "break", "case", "chan", "const", "continue", "default", "defer", "else",
        "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
        "map", "package", "range", "return", "select", "struct", "switch", "type",
        "var",
    };
    static const std::unordered_set<std::string> kRust = {
        "as", "async", "await", "break", "const", "continue", "crate", "dyn",
        "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let",
        "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self",
        "Self", "static", "struct", "super", "trait", "true", "type", "unsafe",
        "use", "where", "while",
    };
    static const std::unordered_set<std::string> kShell = {
        "if", "then", "else", "elif", "fi", "for", "do", "done", "case", "esac",
        "while", "until", "function", "local", "export", "return", "exit",
    };
    static const std::unordered_set<std::string> kJson = {
        "true", "false", "null",
    };
    static const std::unordered_set<std::string> kEmpty;

    if (lang == "cpp" || lang == "c") return kCpp;
    if (lang == "python") return kPython;
    if (lang == "javascript" || lang == "typescript") return kJs;
    if (lang == "go") return kGo;
    if (lang == "rust") return kRust;
    if (lang == "shell") return kShell;
    if (lang == "json") return kJson;
    return kEmpty;
}

const std::unordered_set<std::string>& types_for(std::string_view lang) {
    static const std::unordered_set<std::string> kCpp = {
        "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t",
        "int32_t", "int64_t", "string", "vector", "map", "set", "optional",
        "string_view", "shared_ptr", "unique_ptr",
    };
    static const std::unordered_set<std::string> kPython = {
        "int", "float", "str", "bool", "list", "dict", "tuple", "set", "bytes",
    };
    static const std::unordered_set<std::string> kJs = {
        "Array", "Object", "String", "Number", "Boolean", "Promise", "Map", "Set",
    };
    static const std::unordered_set<std::string> kGo = {
        "bool", "byte", "complex64", "complex128", "error", "float32", "float64",
        "int", "int8", "int16", "int32", "int64", "rune", "string", "uint", "uint8",
        "uint16", "uint32", "uint64", "uintptr",
    };
    static const std::unordered_set<std::string> kRust = {
        "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128",
        "f32", "f64", "bool", "char", "str", "String", "Vec", "Option", "Result",
    };
    static const std::unordered_set<std::string> kEmpty;

    if (lang == "cpp" || lang == "c") return kCpp;
    if (lang == "python") return kPython;
    if (lang == "javascript" || lang == "typescript") return kJs;
    if (lang == "go") return kGo;
    if (lang == "rust") return kRust;
    return kEmpty;
}

StyleId classify_word(std::string_view lang, std::string_view word) {
    const std::string key(word);
    if (keywords_for(lang).count(key)) return StyleId::CodeKeyword;
    if (types_for(lang).count(key)) return StyleId::CodeType;
    return StyleId::Code;
}

void mark_numbers(std::string_view text, std::vector<Region>& regions,
                  std::vector<Region>& out) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (region_covers(regions, i)) continue;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!std::isdigit(c) && !(c == '.' && i + 1 < text.size()
                                  && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            continue;
        }
        if (i > 0 && (is_ident_char(text[i - 1]) || text[i - 1] == '.')) continue;
        std::size_t j = i;
        while (j < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[j]);
            if (std::isdigit(ch) || text[j] == '.') {
                ++j;
                continue;
            }
            break;
        }
        if (j > i) add_region(out, {i, j, StyleId::CodeNumber, 50});
        i = j > i ? j - 1 : j;
    }
}

void mark_words(std::string_view text, std::string_view lang,
                const std::vector<Region>& covered, std::vector<Region>& out) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (region_covers(covered, i)) continue;
        if (!is_ident_start(text[i])) continue;
        std::size_t j = i + 1;
        while (j < text.size() && is_ident_char(text[j])) ++j;
        const auto word = text.substr(i, j - i);
        const StyleId id = classify_word(lang, word);
        if (id != StyleId::Code) {
            add_region(out, {i, j, id, 40});
        } else if (j < text.size() && text[j] == '(') {
            add_region(out, {i, j, StyleId::CodeFunction, 35});
        }
        i = j - 1;
    }
}

StyledLine paint_regions(std::string_view text, const std::vector<Region>& regions) {
    StyledLine line;
    if (text.empty()) return line;

    if (regions.empty()) {
        styled_append(line, StyleId::Code, text);
        return line;
    }

    std::vector<int> priority(text.size(), -1);
    std::vector<StyleId> styles(text.size(), StyleId::Code);
    for (const Region& r : regions) {
        const std::size_t end = std::min(r.end, text.size());
        for (std::size_t i = r.begin; i < end; ++i) {
            if (r.priority >= priority[i]) {
                priority[i] = r.priority;
                styles[i] = r.id;
            }
        }
    }

    std::size_t i = 0;
    while (i < text.size()) {
        const StyleId id = styles[i];
        std::size_t j = i + 1;
        while (j < text.size() && styles[j] == id) ++j;
        styled_append(line, id, text.substr(i, j - i));
        i = j;
    }
    return line;
}

} // namespace

std::string normalize_code_lang(std::string_view lang) {
    while (!lang.empty() && std::isspace(static_cast<unsigned char>(lang.front()))) {
        lang.remove_prefix(1);
    }
    while (!lang.empty() && std::isspace(static_cast<unsigned char>(lang.back()))) {
        lang.remove_suffix(1);
    }
    const auto stop = lang.find_first_of(" \t\r");
    if (stop != std::string_view::npos) lang = lang.substr(0, stop);

    std::string norm;
    norm.reserve(lang.size());
    for (char c : lang) {
        norm.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (norm == "c++" || norm == "cxx" || norm == "cc") return "cpp";
    if (norm == "c") return "cpp";
    if (norm == "py") return "python";
    if (norm == "js" || norm == "jsx") return "javascript";
    if (norm == "ts" || norm == "tsx") return "typescript";
    if (norm == "sh" || norm == "bash" || norm == "zsh" || norm == "shell") return "shell";
    if (norm == "yml") return "yaml";
    if (norm == "rs") return "rust";
    if (norm == "golang") return "go";
    if (norm == "md" || norm == "markdown") return "markdown";
    if (norm == "txt" || norm == "text") return "text";
    return norm;
}

StyledLine highlight_code_line(std::string_view lang, std::string_view line) {
    const std::string norm = normalize_code_lang(lang);
    if (norm == "text" || norm == "markdown") {
        StyledLine plain;
        styled_append(plain, StyleId::Code, line);
        return plain;
    }

    const std::string_view hl_lang = norm.empty() ? std::string_view("generic") : norm;

    std::vector<Region> regions;
    mark_comments(line, hl_lang, regions);
    mark_strings(line, regions);

    std::vector<Region> secondary;
    mark_numbers(line, regions, secondary);
    mark_words(line, hl_lang, regions, secondary);
    regions.insert(regions.end(), secondary.begin(), secondary.end());
    return paint_regions(line, regions);
}

std::vector<StyledLine> highlight_code_block(std::string_view lang,
                                             const std::vector<std::string>& lines) {
    std::vector<StyledLine> out;
    out.reserve(lines.size());
    for (const std::string& line : lines) {
        out.push_back(highlight_code_line(lang, line));
    }
    return out;
}

} // namespace arbiter
