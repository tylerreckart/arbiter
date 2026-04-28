// index_ai/src/cli.cpp — see cli.h

#include "cli.h"
#include "cli_helpers.h"
#include "api_server.h"
#include "auth.h"
#include "constitution.h"
#include "orchestrator.h"
#include "server.h"
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

// Shared SIGINT/SIGTERM flag used by cmd_serve.
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

void cmd_init() {
    std::string dir = get_config_dir();
    std::string agents_dir = dir + "/agents";
    fs::create_directories(agents_dir);

    Auth auth;
    std::string token_path = dir + "/auth_tokens";
    auth.load(token_path);

    std::string token = Auth::generate_token();
    auth.add_token(token);
    auth.save(token_path);

    std::cout << "Initialized ~/.arbiter/\n";
    std::cout << "Auth token (save this): " << token << "\n";
    std::cout << "Tokens stored (hashed) in: " << token_path << "\n\n";

    for (auto& starter : starter_agents()) {
        starter.config.save(agents_dir + "/" + starter.id + ".json");
    }
    std::cout << "Example agents created in " << agents_dir << "/\n";
    for (auto& starter : starter_agents()) {
        std::cout << "  " << starter.id << ".json — " << starter.blurb << "\n";
    }
    std::cout << "\nEdit these or add your own. Then run: arbiter\n";
}

void cmd_gen_token() {
    std::string dir = get_config_dir();
    std::string token_path = dir + "/auth_tokens";

    Auth auth;
    auth.load(token_path);

    std::string token = Auth::generate_token();
    auth.add_token(token);
    auth.save(token_path);

    std::cout << "New token: " << token << "\n";
    std::cout << "Total active tokens: " << auth.token_count() << "\n";
}

void cmd_serve(int port) {
    std::string dir = get_config_dir();
    auto api_keys = get_api_keys();

    Orchestrator orch(std::move(api_keys));
    orch.set_memory_dir(get_memory_dir());
    orch.load_agents(dir + "/agents");

    Auth auth;
    auth.load(dir + "/auth_tokens");

    if (auth.token_count() == 0) {
        std::cerr << "WARN: No auth tokens. Run: arbiter --init\n";
    }

    Server server(orch, auth, port);

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << BANNER;
    std::cout << "Server listening on port " << port << "\n";
    std::cout << "Agents loaded: " << orch.list_agents().size() << "\n";
    for (auto& id : orch.list_agents()) {
        std::cout << "  - " << id << "\n";
    }
    std::cout << "\nConnect: nc <host> " << port << "\n";
    std::cout << "Then: AUTH <token>\n\n";

    server.start();

    while (g_running && server.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutting down...\n";
    server.stop();
    std::cout << "Final stats: " << orch.global_status() << "\n";
}

void cmd_api(int port, const std::string& bind, bool verbose) {
    // HTTP+SSE orchestration endpoint.  Unlike --serve (which multiplexes
    // a line-protocol TCP connection and delegates through one shared
    // orchestrator), this spins up a fresh Orchestrator per request with
    // /exec disabled and /write intercepted so any file the agent produces
    // is streamed to the client rather than landing on the server disk.
    //
    // Auth + billing run through TenantStore (`~/.arbiter/tenants.db`).
    // Provision tokens with `arbiter --add-tenant <name> [--cap <usd>]`.
    std::string dir = get_config_dir();
    auto api_keys = get_api_keys();

    TenantStore tenants;
    tenants.open(dir + "/tenants.db");

    const auto all = tenants.list_tenants();
    if (all.empty()) {
        std::cerr << "WARN: no tenants configured.  Run "
                     "`arbiter --add-tenant <name>` (or POST /v1/admin/tenants) "
                     "and retry.\n"
                     "      The server will start, but every /v1/orchestrate "
                     "call will reject with 401.\n";
    }

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

    ApiServer server(std::move(opts), tenants);

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        std::exit(1);
    }

    std::cout << BANNER;
    std::cout << "API listening on " << bind << ":" << server.port() << "\n";
    std::cout << "  POST  /v1/orchestrate          (Bearer <tenant-token>)\n";
    std::cout << "  GET   /v1/health\n";
    std::cout << "  *     /v1/admin/tenants[/:id]  (Bearer <admin-token>)\n";
    std::cout << "  GET   /v1/admin/usage          (Bearer <admin-token>)\n";
    std::cout << "Tenants: " << all.size() << " configured"
              << "  (markup 20% over provider cost)\n";
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

// "1.2345" → "$1.23" for display.  Sub-cent values fall back to 4 digits
// so a fraction-of-a-cent call doesn't look like zero.
std::string fmt_usd(int64_t uc) {
    const double d = uc_to_usd(uc);
    std::ostringstream ss;
    ss << "$" << std::fixed
       << std::setprecision(d >= 0.01 ? 2 : 4)
       << d;
    return ss.str();
}

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

void cmd_add_tenant(const std::string& name, double cap_usd) {
    if (name.empty()) {
        std::cerr << "Usage: arbiter --add-tenant <name> [--cap <usd>]\n";
        std::exit(1);
    }
    TenantStore store;
    store.open(tenants_db_path());
    const int64_t cap_uc = usd_to_uc(cap_usd);
    auto created = store.create_tenant(name, cap_uc);

    std::cout << "Created tenant #" << created.tenant.id
              << " (" << created.tenant.name << ")\n";
    if (cap_uc > 0) std::cout << "  Monthly cap: " << fmt_usd(cap_uc) << "\n";
    else            std::cout << "  Monthly cap: unlimited\n";

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
              << std::setw(12) << "Month"
              << std::setw(12) << "MTD"
              << std::setw(12) << "Cap"
              << "Last used\n";
    std::cout << std::string(90, '-') << "\n";
    for (auto& t : tenants) {
        std::cout << std::left
                  << std::setw(5)  << t.id
                  << std::setw(20) << (t.name.size() > 19 ? t.name.substr(0, 16) + "..." : t.name)
                  << std::setw(12) << (t.disabled ? "disabled" : "active")
                  << std::setw(12) << (t.month_yyyymm.empty() ? "-" : t.month_yyyymm)
                  << std::setw(12) << fmt_usd(t.month_to_date_uc)
                  << std::setw(12) << (t.monthly_cap_uc > 0 ? fmt_usd(t.monthly_cap_uc) : std::string("unlimited"))
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

void cmd_tenant_usage(const std::string& key) {
    TenantStore store;
    store.open(tenants_db_path());
    auto tenants = store.list_tenants();

    // Filter to one tenant if a key was supplied; matching follows the same
    // numeric-id-then-name rule as set_disabled.
    if (!key.empty()) {
        int64_t want_id = 0;
        try { want_id = std::stoll(key); } catch (...) { want_id = 0; }
        tenants.erase(std::remove_if(tenants.begin(), tenants.end(),
            [&](const Tenant& t) {
                if (want_id > 0) return t.id != want_id;
                return t.name != key;
            }), tenants.end());
        if (tenants.empty()) {
            std::cerr << "No tenant matched '" << key << "'.\n";
            std::exit(1);
        }
    }

    for (auto& t : tenants) {
        std::cout << "#" << t.id << "  " << t.name
                  << (t.disabled ? "  [disabled]" : "") << "\n"
                  << "  created:   " << fmt_ts(t.created_at)   << "\n"
                  << "  last used: " << fmt_ts(t.last_used_at) << "\n"
                  << "  month:     " << (t.month_yyyymm.empty() ? "-" : t.month_yyyymm) << "\n"
                  << "  MTD:       " << fmt_usd(t.month_to_date_uc) << "\n"
                  << "  cap:       " << (t.monthly_cap_uc > 0 ? fmt_usd(t.monthly_cap_uc)
                                                               : std::string("unlimited")) << "\n\n";
    }
}

} // namespace index_ai
