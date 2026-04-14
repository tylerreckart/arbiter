#pragma once
// index/include/json.h — Minimal JSON parser/writer. No deps.
// Supports: string, number, bool, null, object, array.
// Not exhaustive — built for API request/response marshaling.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <variant>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <cmath>

namespace index_ai {

struct JsonValue;
using JsonObject = std::unordered_map<std::string, std::shared_ptr<JsonValue>>;
using JsonArray  = std::vector<std::shared_ptr<JsonValue>>;

struct JsonValue {
    using Value = std::variant<
        std::nullptr_t,
        bool,
        double,
        std::string,
        JsonArray,
        JsonObject
    >;
    Value data;

    JsonValue() : data(nullptr) {}
    explicit JsonValue(Value v) : data(std::move(v)) {}

    bool is_null()   const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_bool()   const { return std::holds_alternative<bool>(data); }
    bool is_number() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array()  const { return std::holds_alternative<JsonArray>(data); }
    bool is_object() const { return std::holds_alternative<JsonObject>(data); }

    const std::string& as_string() const { return std::get<std::string>(data); }
    double             as_number() const { return std::get<double>(data); }
    bool               as_bool()   const { return std::get<bool>(data); }
    const JsonArray&   as_array()  const { return std::get<JsonArray>(data); }
    const JsonObject&  as_object() const { return std::get<JsonObject>(data); }
    JsonObject&        as_object_mut()   { return std::get<JsonObject>(data); }
    JsonArray&         as_array_mut()    { return std::get<JsonArray>(data); }

    // Convenience: obj["key"]
    std::shared_ptr<JsonValue> get(const std::string& key) const {
        if (!is_object()) return nullptr;
        auto& obj = as_object();
        auto it = obj.find(key);
        return (it != obj.end()) ? it->second : nullptr;
    }

    std::string get_string(const std::string& key, const std::string& def = "") const {
        auto v = get(key);
        return (v && v->is_string()) ? v->as_string() : def;
    }

    double get_number(const std::string& key, double def = 0.0) const {
        auto v = get(key);
        return (v && v->is_number()) ? v->as_number() : def;
    }

    int get_int(const std::string& key, int def = 0) const {
        return static_cast<int>(get_number(key, def));
    }

    bool get_bool(const std::string& key, bool def = false) const {
        auto v = get(key);
        return (v && v->is_bool()) ? v->as_bool() : def;
    }
};

// --- Helpers to construct ---
inline std::shared_ptr<JsonValue> jnull() {
    return std::make_shared<JsonValue>();
}
inline std::shared_ptr<JsonValue> jbool(bool b) {
    return std::make_shared<JsonValue>(JsonValue::Value{b});
}
inline std::shared_ptr<JsonValue> jnum(double d) {
    return std::make_shared<JsonValue>(JsonValue::Value{d});
}
inline std::shared_ptr<JsonValue> jstr(std::string s) {
    return std::make_shared<JsonValue>(JsonValue::Value{std::move(s)});
}
inline std::shared_ptr<JsonValue> jarr(JsonArray a = {}) {
    return std::make_shared<JsonValue>(JsonValue::Value{std::move(a)});
}
inline std::shared_ptr<JsonValue> jobj(JsonObject o = {}) {
    return std::make_shared<JsonValue>(JsonValue::Value{std::move(o)});
}

// --- Serializer ---
std::string json_serialize(const JsonValue& val);

// --- Parser ---
std::shared_ptr<JsonValue> json_parse(std::string_view input);

} // namespace index_ai
