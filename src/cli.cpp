// index_ai/src/cli.cpp — see cli.h

#include "cli.h"
#include "cli_helpers.h"
#include "api_server.h"
#include "constitution.h"
#include "orchestrator.h"
#include "starters.h"
#include "tenant_store.h"

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

namespace index_ai {

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
}

void cmd_api(int port, const std::string& bind, bool verbose) {
    // HTTP+SSE orchestration endpoint.  Spins up a fresh Orchestrator per
    // request with /exec disabled and /write intercepted so any file the
    // agent produces is streamed to the client rather than landing on the
    // server disk.
    //
    // Tenant identity (tokens, conversations, artifacts, scratchpad)
    // runs through TenantStore (`~/.arbiter/tenants.db`).  Eligibility
    // and per-turn billing are delegated to an external billing service
    // when $ARBITER_BILLING_URL is set; otherwise the runtime routes
    // every authenticated request through to the configured provider
    // keys with no cap enforcement.
    std::string dir = get_config_dir();
    auto api_keys = get_api_keys();

    TenantStore tenants;
    tenants.open(dir + "/tenants.db");

    // Defer the no-tenants warning until after the screen clear, otherwise
    // the message scrolls off when we redraw with the banner.
    const auto all = tenants.list_tenants();
    const bool no_tenants = all.empty();

    bool fresh_admin = false;
    std::string admin_token = resolve_admin_token(dir, fresh_admin);

    // Verbose logging: explicit --verbose flag wins; otherwise honour
    // ARBITER_API_VERBOSE=1 so an operator running under systemd can flip
    // logging on without restarting with new args.
    bool log_verbose = verbose;
    if (!log_verbose) {
        if (const char* e = std::getenv("ARBITER_API_VERBOSE"))
            log_verbose = (e[0] != '\0' && e[0] != '0');
    }

    ApiServerOptions opts;
    opts.port          = port;
    opts.bind          = bind;
    opts.agents_dir    = dir + "/agents";
    opts.memory_root   = dir + "/memory";    // per-tenant subdirs land under here
    opts.api_keys      = std::move(api_keys);
    opts.exec_disabled = true;               // SaaS default: no shell
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
    if (const char* k = std::getenv("ARBITER_SEARCH_API_KEY"); k && *k) {
        opts.search_api_key = k;
    } else if (const char* k = std::getenv("BRAVE_SEARCH_API_KEY"); k && *k) {
        opts.search_api_key = k;
    }
    // External billing host.  Empty ⇒ no billing; requests pass
    // through to the configured provider keys.  See ApiServerOptions.
    if (const char* q = std::getenv("ARBITER_BILLING_URL"); q && *q) {
        opts.billing_url = q;
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

    // Reset the terminal so the banner anchors at row 1.  ANSI sequence:
    //   \033[2J  — erase entire screen
    //   \033[3J  — erase scrollback (xterm/iTerm) so the banner isn't
    //              chasing a half-screen of prior shell history
    //   \033[H   — cursor home (1,1)
    // Stops the operator from squinting past stale output to find the
    // current bind address every time they restart the server.
    std::cout << "\033[2J\033[3J\033[H";

    std::cout << BANNER;
    std::cout << "API listening on " << bind << ":" << server.port() << "\n";
    std::cout << "  POST  /v1/orchestrate          (Bearer <tenant-token>)\n";
    std::cout << "  GET   /v1/health\n";
    std::cout << "  *     /v1/admin/tenants[/:id]  (Bearer <admin-token>)\n";
    std::cout << "Logging: " << (log_verbose ? "verbose (per-event mirror to stderr)"
                                              : "request-level only "
                                                "(use --verbose for streamed deltas)")
              << "\n";

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

    if (no_tenants) {
        std::cerr << "\nWARN: no tenants configured.  Run "
                     "`arbiter --add-tenant <name>` (or POST /v1/admin/tenants) "
                     "and retry.\n"
                     "      The server will start, but every /v1/orchestrate "
                     "call will reject with 401.\n";
    }
    std::cout << "\n";

    while (g_running && server.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutting down...\n";
    server.stop();
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

} // namespace index_ai
