// src/mcp/manager.cpp — Per-request MCP session manager + registry loader

#include "mcp/manager.h"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

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
        // Skip unusable entries instead of failing the whole registry —
        // a single bad hand-edit must not brick --setup-tools or every
        // /v1/orchestrate request.
        if (s.argv[0].empty()) continue;

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

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

} // namespace

std::string serialize_server_registry(const std::vector<ServerSpec>& specs) {
    std::vector<ServerSpec> sorted = specs;
    std::sort(sorted.begin(), sorted.end(),
              [](const ServerSpec& a, const ServerSpec& b) { return a.name < b.name; });

    std::ostringstream os;
    os << "{\n  \"servers\": {";
    if (sorted.empty()) {
        os << "}\n}\n";
        return os.str();
    }
    os << "\n";
    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& s = sorted[i];
        if (s.argv.empty()) {
            throw std::runtime_error("MCP server '" + s.name + "' has empty argv");
        }
        os << "    \"" << json_escape(s.name) << "\": {\n";
        os << "      \"command\": \"" << json_escape(s.argv[0]) << "\"";
        if (s.argv.size() > 1) {
            os << ",\n      \"args\": [";
            for (size_t a = 1; a < s.argv.size(); ++a) {
                if (a > 1) os << ", ";
                os << "\"" << json_escape(s.argv[a]) << "\"";
            }
            os << "]";
        }
        if (!s.env_extra.empty()) {
            os << ",\n      \"env\": {\n";
            for (size_t e = 0; e < s.env_extra.size(); ++e) {
                const auto& kv = s.env_extra[e];
                const auto eq = kv.find('=');
                const std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
                const std::string val = eq == std::string::npos ? "" : kv.substr(eq + 1);
                os << "        \"" << json_escape(key) << "\": \""
                   << json_escape(val) << "\"";
                if (e + 1 < s.env_extra.size()) os << ",";
                os << "\n";
            }
            os << "      }";
        }
        if (s.init_timeout != std::chrono::seconds(60)) {
            os << ",\n      \"init_timeout_ms\": "
               << s.init_timeout.count();
        }
        if (s.call_timeout != std::chrono::seconds(30)) {
            os << ",\n      \"call_timeout_ms\": "
               << s.call_timeout.count();
        }
        os << "\n    }";
        if (i + 1 < sorted.size()) os << ",";
        os << "\n";
    }
    os << "  }\n}\n";
    return os.str();
}

bool save_server_registry(const std::string& path,
                          const std::vector<ServerSpec>& specs) {
    if (path.empty()) return false;
    std::string body;
    try {
        body = serialize_server_registry(specs);
    } catch (const std::exception&) {
        return false;
    }

    // Write tmp with mode 0600 up front — registry env blocks may hold
    // secrets, and umask-default create + later chmod leaves a window.
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    // open(2) mode is masked by umask — force 0600 before any bytes land.
    if (::fchmod(fd, 0600) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    size_t off = 0;
    bool ok = true;
    while (ok && off < body.size()) {
        const ssize_t n = ::write(fd, body.data() + off, body.size() - off);
        if (n <= 0) ok = false;
        else off += static_cast<size_t>(n);
    }
    if (ok) ok = (::fsync(fd) == 0);
    ::close(fd);
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    ::chmod(path.c_str(), 0600);
    return true;
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
