// src/mcp/manager.cpp — Per-request MCP session manager + registry loader

#include "mcp/manager.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

namespace arbiter::mcp {

namespace {

// Pull a string field from a JSON object, defaulting to "".
std::string str_field(const JsonValue& o, const std::string& key,
                       const std::string& def = "") {
    auto v = o.get(key);
    return (v && v->is_string()) ? v->as_string() : def;
}

// Pull an integer ms duration; default falls back to the caller's value.
std::chrono::milliseconds ms_field(const JsonValue& o, const std::string& key,
                                    std::chrono::milliseconds def) {
    auto v = o.get(key);
    if (!v || !v->is_number()) return def;
    return std::chrono::milliseconds(static_cast<int64_t>(v->as_number()));
}

bool path_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

} // namespace

std::vector<ServerSpec> load_server_registry(const std::string& path) {
    std::vector<ServerSpec> out;
    if (path.empty() || !path_exists(path)) return out;

    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("MCP registry exists but cannot be opened: " + path);
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    auto root = json_parse(buf.str());
    if (!root || !root->is_object())
        throw std::runtime_error("MCP registry is not a JSON object: " + path);

    auto servers = root->get("servers");
    if (!servers || !servers->is_object()) {
        // Treat missing/empty as "no servers" rather than an error so a
        // dev with an empty registry gets a clean start.
        return out;
    }

    for (auto& [name, val] : servers->as_object()) {
        if (!val || !val->is_object()) continue;
        ServerSpec s;
        s.name = name;
        s.argv.push_back(str_field(*val, "command"));
        if (s.argv[0].empty())
            throw std::runtime_error("MCP registry entry '" + name + "' missing 'command'");

        if (auto args = val->get("args"); args && args->is_array()) {
            for (auto& a : args->as_array()) {
                if (a && a->is_string()) s.argv.push_back(a->as_string());
            }
        }
        if (auto env = val->get("env"); env && env->is_object()) {
            for (auto& [k, v] : env->as_object()) {
                if (v && v->is_string()) {
                    s.env_extra.push_back(k + "=" + v->as_string());
                }
            }
        }
        s.init_timeout = ms_field(*val, "init_timeout_ms", s.init_timeout);
        s.call_timeout = ms_field(*val, "call_timeout_ms", s.call_timeout);
        out.push_back(std::move(s));
    }
    // Stable order: alphabetical by name so /mcp tools renders
    // deterministically across requests.
    std::sort(out.begin(), out.end(),
              [](const ServerSpec& a, const ServerSpec& b) { return a.name < b.name; });
    return out;
}

Manager::Manager(std::vector<ServerSpec> specs) : specs_(std::move(specs)) {}

Manager::~Manager() {
    // Destruction order: clients first (under the lock so a parallel
    // /mcp call can't race a crash here), then specs.  Each Client's
    // dtor SIGTERMs its subprocess.
    std::lock_guard<std::mutex> lk(mu_);
    clients_.clear();
}

bool Manager::has(const std::string& name) const {
    for (auto& s : specs_) if (s.name == name) return true;
    return false;
}

std::vector<std::string> Manager::server_names() const {
    std::vector<std::string> out;
    out.reserve(specs_.size());
    for (auto& s : specs_) out.push_back(s.name);
    return out;
}

Client& Manager::client(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = clients_.find(name);
    if (it != clients_.end()) return *it->second;

    // Find the spec.
    const ServerSpec* spec = nullptr;
    for (auto& s : specs_) if (s.name == name) { spec = &s; break; }
    if (!spec) throw std::runtime_error("MCP server '" + name + "' is not configured");

    ClientConfig cfg;
    cfg.name         = spec->name;
    cfg.argv         = spec->argv;
    cfg.env_extra    = spec->env_extra;
    cfg.init_timeout = spec->init_timeout;
    cfg.call_timeout = spec->call_timeout;

    auto cli = std::make_unique<Client>(std::move(cfg));
    Client& ref = *cli;
    clients_[name] = std::move(cli);
    return ref;
}

} // namespace arbiter::mcp
