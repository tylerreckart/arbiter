// arbiter/src/cli.cpp — see cli.h

#include "cli.h"
#include "cli_helpers.h"
#include "api_server.h"
#include "constitution.h"
#include "logger.h"
#include "orchestrator.h"
#include "sandbox.h"
#include "starters.h"
#include "tenant_store.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/rand.h>

namespace fs = std::filesystem;

namespace arbiter {

// Shared SIGINT/SIGTERM flag used by cmd_api for graceful shutdown.
namespace {
volatile std::sig_atomic_t g_running = 1;
void signal_handler(int) { g_running = 0; }

// ─── Admin-token resolution ─────────────────────────────────────────────────
//
// The admin token authorizes /v1/admin/* endpoints — strictly separate from
// tenant tokens.  Resolution order on every `arbiter --api` startup:
//   1. $ARBITER_ADMIN_TOKEN              (overrides the file)
//   2. ~/.arbiter/admin_token  (file)    (owner-only; re-tightens perms)
//   3. generate a fresh one, write it to #2 at mode 0600, print at startup
//
// A `true` in `freshly_generated` tells the caller to announce the new
// token prominently — this is the only time the operator sees the
// plaintext before it's also stored on disk (just like the existing API
// key file protocol).
std::string resolve_admin_token(const std::string& config_dir,
                                bool& freshly_generated) {
    freshly_generated = false;
    if (const char* env = std::getenv("ARBITER_ADMIN_TOKEN");
        env && env[0] != '\0') {
        return env;
    }

    const std::string path = config_dir + "/admin_token";

    // Same owner-check discipline as load_key() in cli_helpers.cpp: refuse
    // a token file owned by another uid (potential credential swap), and
    // tighten the mode in place if it's loose.
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) {
        if (st.st_uid != ::geteuid()) {
            std::cerr << "ERR: " << path << " is not owned by the current user; "
                         "refusing to load\n";
            std::exit(1);
        }
        if (st.st_mode & (S_IRWXG | S_IRWXO)) {
            ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
        }
        std::ifstream f(path);
        std::string tok;
        std::getline(f, tok);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back())))
            tok.pop_back();
        if (!tok.empty()) return tok;
    }

    // Generate.  32 random bytes → 64 hex chars, prefixed "adm_" so the
    // token's role is obvious in logs (vs "atr_" tenant tokens).
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        std::cerr << "ERR: CSPRNG failure generating admin token\n";
        std::exit(1);
    }
    std::string tok = "adm_";
    tok.reserve(4 + sizeof(buf) * 2);
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : buf) { tok += hex[c >> 4]; tok += hex[c & 0xF]; }

    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        std::cerr << "ERR: could not write " << path << ": "
                  << std::strerror(errno) << "\n";
        std::exit(1);
    }
    const std::string out = tok + "\n";
    ssize_t w = ::write(fd, out.data(), out.size());
    ::close(fd);
    ::chmod(path.c_str(), 0600);
    if (w != static_cast<ssize_t>(out.size())) {
        std::cerr << "ERR: short write to " << path << "\n";
        std::exit(1);
    }
    freshly_generated = true;
    return tok;
}
}

void cmd_init(bool force) {
    std::string dir = get_config_dir();
    std::string agents_dir = dir + "/agents";
    fs::create_directories(agents_dir);

    std::cout << "Initialized ~/.arbiter/\n";

    // Write each starter's verbatim source JSON.  Bypassing
    // Constitution::save() preserves the source-tree formatting (pretty-
    // print, field order, raw casing of "advisor") and avoids the IEEE-754
    // double-to-string round-trip that otherwise turns 0.2 into
    // 0.20000000000000001 in to_json output.
    int written = 0, skipped = 0;
    std::vector<std::string> created, kept;
    for (const auto& starter : starter_agents()) {
        std::string path = agents_dir + "/" + starter.id + ".json";
        bool exists = fs::exists(path);
        if (exists && !force) {
            ++skipped;
            kept.push_back(starter.id);
            continue;
        }
        std::string body = starter_json(starter.id);
        if (body.empty()) continue;       // no embedded JSON for this id
        std::ofstream f(path);
        if (!f) {
            std::cerr << "ERR: cannot write " << path << "\n";
            continue;
        }
        f << body;
        if (!body.empty() && body.back() != '\n') f << '\n';
        ++written;
        created.push_back(starter.id);
    }

    if (!created.empty()) {
        std::cout << (force ? "Reset " : "Wrote ") << created.size()
                  << " agent" << (created.size() == 1 ? "" : "s")
                  << " in " << agents_dir << "/\n";
        for (const auto& id : created) {
            // Find the blurb again — small list, linear scan is fine.
            std::string blurb;
            for (const auto& s : starter_agents())
                if (s.id == id) { blurb = s.blurb; break; }
            std::cout << "  " << id << ".json — " << blurb << "\n";
        }
    }
    if (!kept.empty()) {
        std::cout << "\nKept " << kept.size()
                  << " existing file" << (kept.size() == 1 ? "" : "s")
                  << " (re-run with --force to reset):\n";
        for (const auto& id : kept) {
            std::cout << "  " << id << ".json\n";
        }
    }

    if (written == 0 && skipped > 0) {
        std::cout << "\nNo new agents written.  Use `arbiter --init --force` "
                     "to overwrite existing definitions.\n";
    } else if (written > 0) {
        std::cout << "\nEdit these or add your own. Then run: arbiter\n";
    }

    const std::string tui_path = dir + "/tui.json";
    if (!fs::exists(tui_path)) {
        std::ofstream tf(tui_path);
        if (tf) {
            tf << "{\n  \"preset\": \"" << arbiter::kDefaultTuiPreset << "\"\n}\n";
            std::cout << "\nWrote " << tui_path << " (preset: "
                      << arbiter::kDefaultTuiPreset << ")\n";
            std::cout << "  Customize: arbiter --export-theme onedark > "
                      << dir << "/themes/mine.json\n";
            std::cout << "  Then set \"theme_file\": \"themes/mine.json\" in tui.json\n";
        }
    }

    const std::string themes_dir = arbiter::tui_themes_dir(dir);
    fs::create_directories(themes_dir);
    arbiter::tui_install_bundled_themes(dir, force);
    const std::string example_path = themes_dir + "/example.json";
    if (!fs::exists(example_path)) {
        if (arbiter::tui_write_theme_file(
                example_path,
                arbiter::tui_design_for_preset(arbiter::kDefaultTuiPreset),
                "")) {
            std::cout << "Wrote " << example_path << " (editable theme starter)\n";
        }
    }
}

void cmd_api(int port, const std::string& bind, bool verbose,
             bool allow_host_exec) {
    Logger::global().init_from_env();

    std::string dir = get_config_dir();
    auto api_keys = get_api_keys();

    TenantStore tenants;
    tenants.open(dir + "/tenants.db");
    // Single-tenant mode: ensure one primary tenant row exists so the rest
    // of the API surface can keep using tenant-scoped tables unchanged.
    // Existing multi-tenant DBs are tolerated; we pin to the smallest id.
    auto all = tenants.list_tenants();
    if (all.empty()) {
        auto created = tenants.create_tenant("default");
        all.push_back(created.tenant);
    }
    int64_t primary_id = all.front().id;
    for (const auto& t : all) {
        if (t.id < primary_id) primary_id = t.id;
    }
    auto primary = tenants.get_tenant(primary_id);
    if (primary && primary->disabled) {
        tenants.set_disabled(std::to_string(primary->id), false);
    }

    bool fresh_admin = false;
    std::string admin_token = resolve_admin_token(dir, fresh_admin);

    bool log_verbose = verbose;
    if (!log_verbose) {
        if (const char* e = std::getenv("ARBITER_API_VERBOSE"))
            log_verbose = (e[0] != '\0' && e[0] != '0');
    }

    ApiServerOptions opts;
    opts.port          = port;
    opts.bind          = bind;
    opts.agents_dir    = dir + "/agents";
    opts.memory_root   = dir + "/memory";    // single primary tenant uses one subdir
    opts.api_keys      = std::move(api_keys);
    opts.exec_disabled = true;               // SaaS default: no shell
    // Host exec opt-in: CLI flag or ARBITER_ALLOW_HOST_EXEC=1.  Sandbox
    // takes precedence when both are set (make_exec_invoker_callback checks
    // sandbox first).
    {
        bool host_exec = allow_host_exec;
        if (!host_exec) {
            const char* env = std::getenv("ARBITER_ALLOW_HOST_EXEC");
            host_exec = (env && env[0] == '1' && env[1] == '\0');
        }
        if (host_exec) {
            opts.host_exec_enabled = true;
            opts.exec_disabled     = false;
            ::fprintf(stderr,
                "WARN: host exec enabled — agents can run shell commands "
                "as this process (uid %d)\n",
                (int)::getuid());
        }
    }
    opts.admin_token   = admin_token;
    opts.log_verbose   = log_verbose;
    // MCP registry — file is optional.  If present, every /v1/orchestrate
    // request gets a per-request MCP manager loaded from this file.  See
    // docs/api/concepts/mcp.md for the registry schema.
    opts.mcp_servers_path = dir + "/mcp_servers.json";
    // A2A registry — same shape as MCP: optional file, per-request
    // remote-agent manager loaded from it.  See docs/cli/a2a-agents.md.
    opts.a2a_agents_path  = dir + "/a2a_agents.json";
    // Web search — provider + key from env vars.  Either ARBITER-prefixed
    // (preferred, scoped) or the bare BRAVE_SEARCH_API_KEY (convenience).
    // Without a key, /search returns ERR — agents drop the step and fall
    // back to /fetch on URLs they already know.
    if (const char* p = std::getenv("ARBITER_SEARCH_PROVIDER"); p && *p) {
        opts.search_provider = p;
    }
    opts.search_api_key = get_search_api_key();
    // ── Per-tenant sandbox ───────────────────────────────────────────
    if (const char* img = std::getenv("ARBITER_SANDBOX_IMAGE"); img && *img) {
        opts.sandbox_enabled         = true;
        opts.sandbox_image           = img;
        opts.sandbox_workspaces_root = dir + "/workspaces";
        if (const char* r = std::getenv("ARBITER_SANDBOX_RUNTIME"); r && *r)
            opts.sandbox_runtime = r;
        if (const char* n = std::getenv("ARBITER_SANDBOX_NETWORK"); n && *n)
            opts.sandbox_network = n;
        if (const char* m = std::getenv("ARBITER_SANDBOX_MEMORY_MB"); m && *m) {
            try { opts.sandbox_memory_mb = std::stoi(m); } catch (...) {}
        }
        if (const char* c = std::getenv("ARBITER_SANDBOX_CPUS"); c && *c) {
            try { opts.sandbox_cpus = std::stod(c); } catch (...) {}
        }
        if (const char* p = std::getenv("ARBITER_SANDBOX_PIDS_LIMIT"); p && *p) {
            try { opts.sandbox_pids_limit = std::stoi(p); } catch (...) {}
        }
        if (const char* t = std::getenv("ARBITER_SANDBOX_EXEC_TIMEOUT"); t && *t) {
            try { opts.sandbox_exec_timeout_seconds = std::stoi(t); } catch (...) {}
        }
        if (const char* q = std::getenv("ARBITER_SANDBOX_WORKSPACE_MAX_BYTES");
                q && *q) {
            try { opts.sandbox_workspace_max_bytes = std::stoll(q); } catch (...) {}
        }
        if (const char* i = std::getenv("ARBITER_SANDBOX_IDLE_SECONDS");
                i && *i) {
            try { opts.sandbox_idle_seconds = std::stoi(i); } catch (...) {}
        }
    }

    ApiServer server(std::move(opts), tenants);

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        std::exit(1);
    }

    std::cout << "\033[2J\033[H";

    std::cout << "API listening on " << bind << ":" << server.port() << "\n";
    std::cout << "  POST  /v1/orchestrate          (single-tenant; no bearer required)\n";
    std::cout << "  GET   /v1/health\n";
    std::cout << "  *     /v1/admin/*              (Bearer <admin-token>)\n";
    std::cout << "Logging: " << (log_verbose ? "verbose (per-event mirror to stderr)"
                                              : "request-level only "
                                                "(use --verbose for streamed deltas)")
              << "\n";

    if (auto* sb = server.sandbox_manager()) {
        if (sb->usable()) {
            const auto& sc = sb->config();
            std::cout << "Sandbox: " << sc.image
                      << " (network=" << sc.network
                      << ", " << sc.memory_mb << "m / "
                      << sc.cpus << " CPU / "
                      << sc.pids_limit << " pids, "
                      << sc.exec_timeout_seconds << "s exec timeout)\n";
        } else {
            std::cout << "Sandbox: DISABLED — " << sb->unusable_reason()
                      << "\n         /exec returns ERR; fix and restart.\n";
        }
    }

    if (fresh_admin) {
        std::cout << "\n  Admin token (save this — not shown again):\n"
                  << "    " << admin_token << "\n"
                  << "  Stored at: " << dir << "/admin_token (0600)\n"
                  << "  Override with $ARBITER_ADMIN_TOKEN.\n";
    } else {
        std::cout << "Admin token loaded from "
                  << (std::getenv("ARBITER_ADMIN_TOKEN")
                      ? "$ARBITER_ADMIN_TOKEN"
                      : (dir + "/admin_token").c_str())
                  << "\n";
    }

    if (all.size() > 1) {
        std::cerr << "WARN: legacy multi-tenant DB detected (" << all.size()
                  << " tenant rows). Running in single-tenant mode with tenant #"
                  << primary_id << ".\n";
    }
    std::cout << "\n";

    while (g_running && server.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutting down...\n";
    server.stop();
}

ApiServerOptions make_cli_api_options(const std::string& config_dir,
                                      const std::map<std::string, std::string>& api_keys,
                                      bool exec_allowed) {
    ApiServerOptions opts;
    opts.agents_dir    = config_dir + "/agents";
    opts.memory_root   = config_dir + "/memory";
    opts.api_keys      = api_keys;
    opts.exec_disabled = !exec_allowed;
    {
        bool host_exec = exec_allowed;
        if (!host_exec) {
            const char* env = std::getenv("ARBITER_ALLOW_HOST_EXEC");
            host_exec = (env && env[0] == '1' && env[1] == '\0');
        }
        if (host_exec) {
            opts.host_exec_enabled = true;
            opts.exec_disabled     = false;
        }
    }
    opts.mcp_servers_path = config_dir + "/mcp_servers.json";
    opts.a2a_agents_path  = config_dir + "/a2a_agents.json";
    if (const char* p = std::getenv("ARBITER_SEARCH_PROVIDER"); p && *p) {
        opts.search_provider = p;
    }
    opts.search_api_key = get_search_api_key();
    return opts;
}

Tenant ensure_primary_tenant(TenantStore& store) {
    auto all = store.list_tenants();
    if (all.empty()) {
        return store.create_tenant("default").tenant;
    }
    size_t best = 0;
    for (size_t i = 1; i < all.size(); ++i) {
        if (all[i].id < all[best].id) best = i;
    }
    if (all[best].disabled) {
        store.set_disabled(std::to_string(all[best].id), false);
        all[best].disabled = false;
    }
    return all[best];
}

void cmd_oneshot(const std::string& agent_id, const std::string& msg) {
    std::string dir = get_config_dir();
    auto api_keys = get_api_keys();

    Orchestrator orch(std::move(api_keys));
    orch.load_agents(dir + "/agents");

    auto resp = orch.send(agent_id, msg);
    if (resp.ok) {
        std::cout << resp.content << "\n";
    } else {
        std::cerr << "ERR: " << resp.error << "\n";
        std::exit(1);
    }
}

// ─── Tenant admin ───────────────────────────────────────────────────────────
// All open the same SQLite file (~/.arbiter/tenants.db), do one operation,
// print a human-readable report, and exit.

namespace {

std::string tenants_db_path() { return get_config_dir() + "/tenants.db"; }

std::string fmt_ts(int64_t epoch_seconds) {
    if (epoch_seconds == 0) return "never";
    std::time_t t = static_cast<std::time_t>(epoch_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tm);
    return buf;
}

} // namespace

void cmd_add_tenant(const std::string& name) {
    if (name.empty()) {
        std::cerr << "Usage: arbiter --add-tenant <name>\n";
        std::exit(1);
    }
    TenantStore store;
    store.open(tenants_db_path());
    auto created = store.create_tenant(name);

    std::cout << "Created tenant #" << created.tenant.id
              << " (" << created.tenant.name << ")\n";

    // The plaintext token is only visible here.  After this, the DB only
    // stores its SHA-256 digest — a misplaced token means issuing a new one.
    std::cout << "\n  API key (save this — it will not be shown again):\n"
              << "    " << created.token << "\n\n"
              << "  Clients call:\n"
              << "    curl -N -H \"Authorization: Bearer " << created.token << "\" \\\n"
              << "         -H \"Content-Type: application/json\" \\\n"
              << "         -d '{\"agent\":\"index\",\"message\":\"...\"}' \\\n"
              << "         http://<host>:<port>/v1/orchestrate\n";
}

void cmd_list_tenants() {
    TenantStore store;
    store.open(tenants_db_path());
    auto tenants = store.list_tenants();
    if (tenants.empty()) {
        std::cout << "No tenants configured.  "
                     "Add one with `arbiter --add-tenant <name>`.\n";
        return;
    }

    std::cout << std::left
              << std::setw(5)  << "ID"
              << std::setw(20) << "Name"
              << std::setw(12) << "Status"
              << "Last used\n";
    std::cout << std::string(60, '-') << "\n";
    for (auto& t : tenants) {
        std::cout << std::left
                  << std::setw(5)  << t.id
                  << std::setw(20) << (t.name.size() > 19 ? t.name.substr(0, 16) + "..." : t.name)
                  << std::setw(12) << (t.disabled ? "disabled" : "active")
                  << fmt_ts(t.last_used_at) << "\n";
    }
}

void cmd_disable_tenant(const std::string& key) {
    if (key.empty()) {
        std::cerr << "Usage: arbiter --disable-tenant <id|name>\n";
        std::exit(1);
    }
    TenantStore store;
    store.open(tenants_db_path());
    if (store.set_disabled(key, true))
        std::cout << "Disabled tenant '" << key << "'.\n";
    else
        std::cerr << "No tenant matched '" << key << "'.\n", std::exit(1);
}

void cmd_enable_tenant(const std::string& key) {
    if (key.empty()) {
        std::cerr << "Usage: arbiter --enable-tenant <id|name>\n";
        std::exit(1);
    }
    TenantStore store;
    store.open(tenants_db_path());
    if (store.set_disabled(key, false))
        std::cout << "Enabled tenant '" << key << "'.\n";
    else
        std::cerr << "No tenant matched '" << key << "'.\n", std::exit(1);
}

} // namespace arbiter
