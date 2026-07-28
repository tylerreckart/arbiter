// arbiter/src/latex_math.cpp — LaTeX math → terminal-friendly Unicode

#include "latex_math.h"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace arbiter {

namespace {

bool is_name_char(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

// Map ASCII digit / sign / letter to Unicode superscript when possible.
const char* superscript_char(char c) {
    switch (c) {
    case '0': return "\u2070";
    case '1': return "\u00b9";
    case '2': return "\u00b2";
    case '3': return "\u00b3";
    case '4': return "\u2074";
    case '5': return "\u2075";
    case '6': return "\u2076";
    case '7': return "\u2077";
    case '8': return "\u2078";
    case '9': return "\u2079";
    case '+': return "\u207a";
    case '-': return "\u207b";
    case '=': return "\u207c";
    case '(': return "\u207d";
    case ')': return "\u207e";
    case 'n': return "\u207f";
    case 'i': return "\u2071";
    default:  return nullptr;
    }
}

const char* subscript_char(char c) {
    switch (c) {
    case '0': return "\u2080";
    case '1': return "\u2081";
    case '2': return "\u2082";
    case '3': return "\u2083";
    case '4': return "\u2084";
    case '5': return "\u2085";
    case '6': return "\u2086";
    case '7': return "\u2087";
    case '8': return "\u2088";
    case '9': return "\u2089";
    case '+': return "\u208a";
    case '-': return "\u208b";
    case '=': return "\u208c";
    case '(': return "\u208d";
    case ')': return "\u208e";
    case 'a': return "\u2090";
    case 'e': return "\u2091";
    case 'o': return "\u2092";
    case 'x': return "\u2093";
    case 'h': return "\u2095";
    case 'k': return "\u2096";
    case 'l': return "\u2097";
    case 'm': return "\u2098";
    case 'n': return "\u2099";
    case 'p': return "\u209a";
    case 's': return "\u209b";
    case 't': return "\u209c";
    default:  return nullptr;
    }
}

std::string try_script(std::string_view body, bool super) {
    std::string out;
    out.reserve(body.size() * 3);
    for (char c : body) {
        const char* mapped = super ? superscript_char(c) : subscript_char(c);
        if (!mapped) return {};
        out += mapped;
    }
    return out;
}

// Skip spaces (ASCII + common TeX thin spaces already expanded).
size_t skip_ws(std::string_view s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
    return i;
}

// Read a {...} group starting at `{`. Returns content (without braces) and end index
// past the closing `}`. On failure, returns empty content and start.
std::pair<std::string_view, size_t> read_brace_group(std::string_view s, size_t i) {
    if (i >= s.size() || s[i] != '{') return {{}, i};
    int depth = 0;
    size_t start = i + 1;
    for (size_t j = i; j < s.size(); ++j) {
        if (s[j] == '{') ++depth;
        else if (s[j] == '}') {
            --depth;
            if (depth == 0) return {s.substr(start, j - start), j + 1};
        }
    }
    return {{}, i};
}

// Single token after ^ or _: brace group, command, or one character.
std::pair<std::string, size_t> read_script_arg(std::string_view s, size_t i) {
    i = skip_ws(s, i);
    if (i >= s.size()) return {{}, i};
    if (s[i] == '{') {
        auto [body, end] = read_brace_group(s, i);
        return {std::string(body), end};
    }
    if (s[i] == '\\') {
        size_t j = i + 1;
        while (j < s.size() && is_name_char(s[j])) ++j;
        return {std::string(s.substr(i, j - i)), j};
    }
    return {std::string(1, s[i]), i + 1};
}

struct CmdMap {
    const char* name;
    const char* replacement;
};

// Symbols and Greek that become a single Unicode character (no arguments).
constexpr CmdMap kSymbols[] = {
    {"times", "\u00d7"},
    {"cdot", "\u00b7"},
    {"approx", "\u2248"},
    {"neq", "\u2260"},
    {"ne", "\u2260"},
    {"leq", "\u2264"},
    {"le", "\u2264"},
    {"geq", "\u2265"},
    {"ge", "\u2265"},
    {"pm", "\u00b1"},
    {"mp", "\u2213"},
    {"infty", "\u221e"},
    {"partial", "\u2202"},
    {"nabla", "\u2207"},
    {"sum", "\u2211"},
    {"prod", "\u220f"},
    {"int", "\u222b"},
    {"oint", "\u222e"},
    {"rightarrow", "\u2192"},
    {"leftarrow", "\u2190"},
    {"Rightarrow", "\u21d2"},
    {"Leftrightarrow", "\u21d4"},
    {"to", "\u2192"},
    {"mapsto", "\u21a6"},
    {"in", "\u2208"},
    {"notin", "\u2209"},
    {"subset", "\u2282"},
    {"subseteq", "\u2286"},
    {"supset", "\u2283"},
    {"supseteq", "\u2287"},
    {"cup", "\u222a"},
    {"cap", "\u2229"},
    {"emptyset", "\u2205"},
    {"forall", "\u2200"},
    {"exists", "\u2203"},
    {"hbar", "\u210f"},
    {"ell", "\u2113"},
    {"Re", "\u211c"},
    {"Im", "\u2111"},
    {"angle", "\u2220"},
    {"perp", "\u27c2"},
    {"parallel", "\u2225"},
    {"degree", "\u00b0"},
    {"circ", "\u2218"},
    {"ldots", "\u2026"},
    {"cdots", "\u22ef"},
    {"dots", "\u2026"},
    {"alpha", "\u03b1"},
    {"beta", "\u03b2"},
    {"gamma", "\u03b3"},
    {"delta", "\u03b4"},
    {"epsilon", "\u03b5"},
    {"varepsilon", "\u03b5"},
    {"zeta", "\u03b6"},
    {"eta", "\u03b7"},
    {"theta", "\u03b8"},
    {"vartheta", "\u03d1"},
    {"iota", "\u03b9"},
    {"kappa", "\u03ba"},
    {"lambda", "\u03bb"},
    {"mu", "\u03bc"},
    {"nu", "\u03bd"},
    {"xi", "\u03be"},
    {"pi", "\u03c0"},
    {"varpi", "\u03d6"},
    {"rho", "\u03c1"},
    {"sigma", "\u03c3"},
    {"tau", "\u03c4"},
    {"upsilon", "\u03c5"},
    {"phi", "\u03c6"},
    {"varphi", "\u03d5"},
    {"chi", "\u03c7"},
    {"psi", "\u03c8"},
    {"omega", "\u03c9"},
    {"Gamma", "\u0393"},
    {"Delta", "\u0394"},
    {"Theta", "\u0398"},
    {"Lambda", "\u039b"},
    {"Xi", "\u039e"},
    {"Pi", "\u03a0"},
    {"Sigma", "\u03a3"},
    {"Upsilon", "\u03a5"},
    {"Phi", "\u03a6"},
    {"Psi", "\u03a8"},
    {"Omega", "\u03a9"},
};

const char* lookup_symbol(std::string_view name) {
    for (const auto& entry : kSymbols) {
        if (name == entry.name) return entry.replacement;
    }
    return nullptr;
}

bool is_passthrough_text_cmd(std::string_view name) {
    return name == "text" || name == "mathrm" || name == "mathbf" ||
           name == "mathit" || name == "mathsf" || name == "mathtt" ||
           name == "textrm" || name == "textbf" || name == "textit" ||
           name == "operatorname" || name == "mbox" || name == "hbox";
}

bool is_spacing_cmd(std::string_view name) {
    return name == " " || name == "," || name == ";" || name == "!" ||
           name == "quad" || name == "qquad" || name == "hspace" ||
           name == "vspace" || name == "thinspace";
}

bool is_decoration_cmd(std::string_view name) {
    return name == "left" || name == "right" || name == "big" ||
           name == "Big" || name == "bigg" || name == "Bigg" ||
           name == "bigl" || name == "bigr" || name == "Bigl" ||
           name == "Bigr";
}

std::string convert(std::string_view latex);

std::string convert_script(std::string_view raw, bool super) {
    std::string inner = convert(raw);
    if (std::string mapped = try_script(inner, super); !mapped.empty()) {
        return mapped;
    }
    // Fallback: a^b / a_b style when Unicode can't cover the body.
    std::string out;
    out += super ? '^' : '_';
    const bool wrap = inner.size() != 1;
    if (wrap) out += '(';
    out += inner;
    if (wrap) out += ')';
    return out;
}

std::string convert(std::string_view latex) {
    std::string out;
    out.reserve(latex.size());
    size_t i = 0;
    while (i < latex.size()) {
        const char c = latex[i];

        if (c == '{' || c == '}') {
            // Drop grouping braces; content is still converted.
            ++i;
            continue;
        }

        if (c == '^') {
            auto [arg, end] = read_script_arg(latex, i + 1);
            out += convert_script(arg, true);
            i = end;
            continue;
        }

        if (c == '_') {
            auto [arg, end] = read_script_arg(latex, i + 1);
            out += convert_script(arg, false);
            i = end;
            continue;
        }

        if (c == '\\') {
            if (i + 1 >= latex.size()) {
                ++i;
                continue;
            }
            const char next = latex[i + 1];

            // Escaped special chars: \{ \} \_ \% \& \# \$
            if (next == '{' || next == '}' || next == '_' || next == '%' ||
                next == '&' || next == '#' || next == '$' || next == '\\') {
                out += next;
                i += 2;
                continue;
            }

            // Named command
            size_t j = i + 1;
            while (j < latex.size() && is_name_char(latex[j])) ++j;
            std::string_view name = latex.substr(i + 1, j - i - 1);

            // Single non-letter command (\, \; \! etc.)
            if (name.empty()) {
                name = latex.substr(i + 1, 1);
                j = i + 2;
            }

            if (is_spacing_cmd(name)) {
                if (name == "quad" || name == "qquad" || name == "hspace" ||
                    name == "vspace") {
                    out += ' ';
                    if (name == "qquad") out += ' ';
                    // Drop optional brace arg for \hspace{...}
                    if (j < latex.size() && latex[j] == '{') {
                        auto [_, end] = read_brace_group(latex, j);
                        j = end;
                    }
                } else if (name == "," || name == ";" || name == " " ||
                           name == "thinspace") {
                    out += ' ';
                }
                // \! → nothing
                i = j;
                continue;
            }

            if (is_decoration_cmd(name)) {
                // \left( \right) → keep the delimiter if present
                j = skip_ws(latex, j);
                if (j < latex.size()) {
                    if (latex[j] == '\\') {
                        // \left\| or similar — skip another command, emit |
                        size_t k = j + 1;
                        while (k < latex.size() && is_name_char(latex[k])) ++k;
                        std::string_view delim = latex.substr(j + 1, k - j - 1);
                        if (delim == "lVert" || delim == "rVert" || delim == "Vert" ||
                            delim == "|") {
                            out += '|';
                        } else if (delim == "langle") {
                            out += "\u27e8";
                        } else if (delim == "rangle") {
                            out += "\u27e9";
                        }
                        i = k;
                        continue;
                    }
                    if (latex[j] == '.' ) {
                        // \left. invisible
                        i = j + 1;
                        continue;
                    }
                    out += latex[j];
                    i = j + 1;
                    continue;
                }
                i = j;
                continue;
            }

            if (name == "frac" || name == "dfrac" || name == "tfrac") {
                auto [num, after_num] = read_brace_group(latex, skip_ws(latex, j));
                auto [den, after_den] = read_brace_group(latex, skip_ws(latex, after_num));
                const std::string n = convert(num);
                const std::string d = convert(den);
                const bool simple_n = n.size() == 1;
                const bool simple_d = d.size() == 1;
                if (!simple_n) out += '(';
                out += n;
                if (!simple_n) out += ')';
                out += '/';
                if (!simple_d) out += '(';
                out += d;
                if (!simple_d) out += ')';
                i = after_den == after_num ? j : after_den;
                if (num.empty() && den.empty()) i = j;
                continue;
            }

            if (name == "sqrt") {
                size_t at = skip_ws(latex, j);
                // Optional [n] root index — show as ⁿ√
                if (at < latex.size() && latex[at] == '[') {
                    size_t close = latex.find(']', at + 1);
                    if (close != std::string_view::npos) {
                        std::string idx = convert(latex.substr(at + 1, close - at - 1));
                        if (std::string mapped = try_script(idx, true); !mapped.empty()) {
                            out += mapped;
                        } else {
                            out += idx;
                        }
                        at = close + 1;
                    }
                }
                auto [body, end] = read_brace_group(latex, skip_ws(latex, at));
                out += "\u221a";  // √
                const std::string inner = convert(body);
                if (inner.size() == 1) {
                    out += inner;
                } else {
                    out += '(';
                    out += inner;
                    out += ')';
                }
                i = body.empty() ? j : end;
                continue;
            }

            if (is_passthrough_text_cmd(name)) {
                auto [body, end] = read_brace_group(latex, skip_ws(latex, j));
                // Preserve surrounding spaces from \text{ kg} / \mathrm{ m}.
                size_t lead = 0;
                while (lead < body.size() &&
                       (body[lead] == ' ' || body[lead] == '\t')) {
                    ++lead;
                }
                size_t trail = body.size();
                while (trail > lead &&
                       (body[trail - 1] == ' ' || body[trail - 1] == '\t')) {
                    --trail;
                }
                out.append(body.data(), lead);
                if (trail > lead) {
                    out += convert(body.substr(lead, trail - lead));
                }
                out.append(body.data() + trail, body.size() - trail);
                i = (end == skip_ws(latex, j)) ? j : end;
                continue;
            }

            if (name == "overline" || name == "underline" || name == "hat" ||
                name == "bar" || name == "vec" || name == "tilde" ||
                name == "dot" || name == "ddot" || name == "widehat" ||
                name == "widetilde" || name == "mathbf" || name == "boldsymbol") {
                auto [body, end] = read_brace_group(latex, skip_ws(latex, j));
                out += convert(body);
                if (name == "hat" || name == "widehat") out += "\u0302";
                else if (name == "tilde" || name == "widetilde") out += "\u0303";
                else if (name == "bar" || name == "overline") out += "\u0305";
                else if (name == "vec") out += "\u20d7";
                else if (name == "dot") out += "\u0307";
                else if (name == "ddot") out += "\u0308";
                i = body.empty() ? j : end;
                continue;
            }

            if (const char* sym = lookup_symbol(name)) {
                out += sym;
                i = j;
                continue;
            }

            // Unknown command: drop the slash, keep the name (better than raw \cmd).
            out += name;
            i = j;
            continue;
        }

        // Collapse runs of whitespace to a single space.
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            i = skip_ws(latex, i);
            continue;
        }

        out += c;
        ++i;
    }

    // Trim trailing space.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

std::string latex_math_to_plain(std::string_view latex) {
    return convert(latex);
}

} // namespace arbiter
