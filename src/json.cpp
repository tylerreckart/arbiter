// index_ai/src/json.cpp — Minimal JSON parser + serializer
#include "json.h"
#include <cctype>
#include <charconv>
#include <cstring>

namespace index_ai {

// ============================================================
// Serializer
// ============================================================

static void escape_string(std::ostringstream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    os << '"';
}

static void serialize_impl(std::ostringstream& os, const JsonValue& val) {
    if (val.is_null()) {
        os << "null";
    } else if (val.is_bool()) {
        os << (val.as_bool() ? "true" : "false");
    } else if (val.is_number()) {
        double d = val.as_number();
        if (d == static_cast<int64_t>(d) && std::abs(d) < 1e15) {
            os << static_cast<int64_t>(d);
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            os << buf;
        }
    } else if (val.is_string()) {
        escape_string(os, val.as_string());
    } else if (val.is_array()) {
        os << '[';
        bool first = true;
        for (auto& elem : val.as_array()) {
            if (!first) os << ',';
            first = false;
            if (elem) serialize_impl(os, *elem);
            else os << "null";
        }
        os << ']';
    } else if (val.is_object()) {
        os << '{';
        bool first = true;
        for (auto& [k, v] : val.as_object()) {
            if (!first) os << ',';
            first = false;
            escape_string(os, k);
            os << ':';
            if (v) serialize_impl(os, *v);
            else os << "null";
        }
        os << '}';
    }
}

std::string json_serialize(const JsonValue& val) {
    std::ostringstream os;
    serialize_impl(os, val);
    return os.str();
}

// ============================================================
// Parser
// ============================================================

struct Parser {
    std::string_view src;
    size_t pos = 0;

    char peek() const {
        return pos < src.size() ? src[pos] : '\0';
    }
    char advance() {
        return pos < src.size() ? src[pos++] : '\0';
    }
    void skip_ws() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos])))
            ++pos;
    }
    bool match(std::string_view s) {
        if (src.substr(pos, s.size()) == s) {
            pos += s.size();
            return true;
        }
        return false;
    }

    [[noreturn]] void error(const char* msg) {
        throw std::runtime_error(
            std::string("JSON parse error at pos ") + std::to_string(pos) + ": " + msg);
    }

    std::shared_ptr<JsonValue> parse_value() {
        skip_ws();
        char c = peek();
        if (c == '"')      return parse_string_val();
        if (c == '{')      return parse_object();
        if (c == '[')      return parse_array();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n')      return parse_null();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parse_number();
        error("unexpected character");
    }

    std::string parse_string() {
        if (advance() != '"') error("expected '\"'");
        std::string result;
        result.reserve(64);
        while (pos < src.size()) {
            char c = advance();
            if (c == '"') return result;
            if (c == '\\') {
                char e = advance();
                switch (e) {
                    case '"':  result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/'; break;
                    case 'b':  result += '\b'; break;
                    case 'f':  result += '\f'; break;
                    case 'n':  result += '\n'; break;
                    case 'r':  result += '\r'; break;
                    case 't':  result += '\t'; break;
                    case 'u': {
                        // Basic BMP only
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = advance();
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else error("invalid hex in \\u escape");
                        }
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: error("invalid escape");
                }
            } else {
                result += c;
            }
        }
        error("unterminated string");
    }

    std::shared_ptr<JsonValue> parse_string_val() {
        return jstr(parse_string());
    }

    std::shared_ptr<JsonValue> parse_number() {
        size_t start = pos;
        if (peek() == '-') advance();
        while (pos < src.size() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
        if (peek() == '.') {
            advance();
            while (pos < src.size() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') advance();
            while (pos < src.size() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
        }
        double val = 0;
        auto [ptr, ec] = std::from_chars(src.data() + start, src.data() + pos, val);
        if (ec != std::errc()) error("invalid number");
        return jnum(val);
    }

    std::shared_ptr<JsonValue> parse_bool() {
        if (match("true"))  return jbool(true);
        if (match("false")) return jbool(false);
        error("expected bool");
    }

    std::shared_ptr<JsonValue> parse_null() {
        if (match("null")) return jnull();
        error("expected null");
    }

    std::shared_ptr<JsonValue> parse_array() {
        advance(); // '['
        skip_ws();
        JsonArray arr;
        if (peek() == ']') { advance(); return jarr(std::move(arr)); }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            if (peek() == ']') { advance(); break; }
            if (peek() != ',') error("expected ',' or ']'");
            advance();
        }
        return jarr(std::move(arr));
    }

    std::shared_ptr<JsonValue> parse_object() {
        advance(); // '{'
        skip_ws();
        JsonObject obj;
        if (peek() == '}') { advance(); return jobj(std::move(obj)); }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (advance() != ':') error("expected ':'");
            obj[std::move(key)] = parse_value();
            skip_ws();
            if (peek() == '}') { advance(); break; }
            if (peek() != ',') error("expected ',' or '}'");
            advance();
        }
        return jobj(std::move(obj));
    }
};

std::shared_ptr<JsonValue> json_parse(std::string_view input) {
    Parser p{input, 0};
    auto val = p.parse_value();
    p.skip_ws();
    if (p.pos != input.size()) {
        p.error("trailing characters");
    }
    return val;
}

} // namespace index_ai
