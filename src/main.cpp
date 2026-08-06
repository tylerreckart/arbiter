// Usage:
//   arbiter                          — interactive REPL
//   arbiter --api [--port 8080]      — HTTP+SSE API server
//   arbiter --send <agent> <msg>     — one-shot message
//   arbiter --init                   — create config dir + example agents
//   arbiter --setup-tools            — MCP / search / browse wizard

#include "cli.h"
#include "cli_helpers.h"
#include "remote/connect_config.h"
#include "repl/repl_argv.h"
#include "tui/tui_design.h"

#include <iostream>
#include <string>
#include <string_view>
#include <cstdlib>

using arbiter::get_config_dir;

int main(int argc, char* argv[]) {
    try {
        if (argc >= 3 && std::string_view(argv[1]) == "--export-theme") {
            const std::string name = argv[2];
            if (!arbiter::tui_preset_is_valid(name)) {
                std::cerr << "Unknown preset: " << name << "\n";
                return 1;
            }
            std::cout << arbiter::tui_design_to_json(
                arbiter::tui_design_for_preset(name), "") << '\n';
            return 0;
        }

        if (arbiter::argv_has_theme_flag(argc, argv) &&
            arbiter::argv_repl_theme(argc, argv).empty()) {
            std::cerr << "Usage: arbiter [--no-exec] [--theme PRESET]\n";
            return 1;
        }

        if (arbiter::argv_launches_interactive(argc, argv)) {
            const std::string theme = arbiter::argv_repl_theme(argc, argv);
            const std::string dir = get_config_dir();
            if (!theme.empty() && !arbiter::tui_theme_name_is_valid(dir, theme)) {
                std::cerr << "Unknown theme: " << theme << "\n";
                std::cerr << "Built-in:";
                for (const auto& p : arbiter::tui_builtin_presets()) {
                    std::cerr << ' ' << p;
                }
                std::cerr << "\nCustom: place JSON in " << arbiter::tui_themes_dir(dir) << "/\n";
                return 1;
            }

            if (arbiter::argv_has_connect(argc, argv)) {
                auto cfg = arbiter::parse_connect_argv(argc, argv);
                if (const std::string err = arbiter::resolve_remote_connect(cfg);
                    !err.empty()) {
                    std::cerr << "ERR: " << err << "\n";
                    std::cerr << "Usage: arbiter --connect [URL] [--token TOKEN] [--theme PRESET]\n"
                                 "  URL/token may also come from ARBITER_API_URL / ARBITER_API_TOKEN\n";
                    return 1;
                }
                arbiter::cmd_interactive_remote(
                    std::move(cfg),
                    !arbiter::argv_has_no_exec(argc, argv),
                    theme);
                return 0;
            }

            arbiter::cmd_interactive(!arbiter::argv_has_no_exec(argc, argv), theme);
            return 0;
        }

        std::string arg1 = argv[1];

        if (arg1 == "--no-exec") {
            const std::string theme = arbiter::argv_repl_theme(argc, argv);
            const std::string dir = get_config_dir();
            if (!theme.empty() && !arbiter::tui_theme_name_is_valid(dir, theme)) {
                std::cerr << "Unknown theme: " << theme << "\n";
                return 1;
            }
            arbiter::cmd_interactive(false, theme);
            return 0;
        }
        if (arg1 == "--init" || arg1 == "init") {
            // arbiter --init [--force]
            // Without --force, --init preserves existing agent JSON files
            // so accidental re-runs don't clobber a user's edits.
            bool force = false;
            for (int i = 2; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--force" || a == "-f") force = true;
                else {
                    std::cerr << "Unknown --init flag: " << a << "\n";
                    return 1;
                }
            }
            arbiter::cmd_init(force);
            return 0;
        }
        if (arg1 == "--setup-tools" || arg1 == "setup-tools") {
            arbiter::cmd_setup_tools();
            return 0;
        }
        if (arg1 == "--api" || arg1 == "api") {
            // arbiter --api [--port N] [--bind ADDR] [--verbose] [--allow-host-exec]
            int port = 8080;
            std::string bind = "127.0.0.1";
            bool verbose = false;
            bool allow_host_exec = false;
            for (int i = 2; i < argc; ) {
                std::string k = argv[i];
                if (k == "--verbose" || k == "-v") {
                    verbose = true;
                    ++i;
                    continue;
                }
                if (k == "--allow-host-exec") {
                    allow_host_exec = true;
                    ++i;
                    continue;
                }
                if (i + 1 >= argc) {
                    std::cerr << "--api flag '" << k << "' requires a value\n";
                    return 1;
                }
                std::string v = argv[i + 1];
                if      (k == "--port") port = std::atoi(v.c_str());
                else if (k == "--bind") bind = v;
                else {
                    std::cerr << "Unknown --api flag: " << k << "\n";
                    return 1;
                }
                i += 2;
            }
            arbiter::cmd_api(port, bind, verbose, allow_host_exec);
            return 0;
        }
        if (arg1 == "--send" || arg1 == "send") {
            if (argc < 4) {
                std::cerr << "Usage: arbiter --send <agent_id> <message>\n";
                return 1;
            }
            std::string agent = argv[2];
            std::string msg;
            for (int i = 3; i < argc; ++i) {
                if (i > 3) msg += " ";
                msg += argv[i];
            }
            arbiter::cmd_oneshot(agent, msg);
            return 0;
        }
        // Tenant identity admin — `arbiter --api` uses the resulting
        // tenants.db for bearer-token auth.
        if (arg1 == "--add-tenant") {
            if (argc < 3) {
                std::cerr << "Usage: arbiter --add-tenant <name>\n";
                return 1;
            }
            arbiter::cmd_add_tenant(argv[2]);
            return 0;
        }
        if (arg1 == "--list-tenants") {
            arbiter::cmd_list_tenants();
            return 0;
        }
        if (arg1 == "--rotate-tenant-token") {
            if (argc < 3) {
                std::cerr << "Usage: arbiter --rotate-tenant-token <id|name>\n";
                return 1;
            }
            arbiter::cmd_rotate_tenant_token(argv[2]);
            return 0;
        }
        if (arg1 == "--disable-tenant") {
            if (argc < 3) {
                std::cerr << "Usage: arbiter --disable-tenant <id|name>\n";
                return 1;
            }
            arbiter::cmd_disable_tenant(argv[2]);
            return 0;
        }
        if (arg1 == "--enable-tenant") {
            if (argc < 3) {
                std::cerr << "Usage: arbiter --enable-tenant <id|name>\n";
                return 1;
            }
            arbiter::cmd_enable_tenant(argv[2]);
            return 0;
        }
        if (arg1 == "--help" || arg1 == "-h" || arg1 == "help") {
            std::cout <<
                "Usage:\n"
                "  arbiter [--theme PRESET]           Interactive REPL\n"
                "  arbiter --no-exec [--theme PRESET] Interactive REPL with /exec disabled\n"
                "                                     (agents cannot run shell commands)\n"
                "                                     --theme: high-contrast (default), onedark,\n"
                "                                     modern, nord, dracula, solarized, light,\n"
                "                                     gruvbox, catppuccin, tokyo-night, …\n"
                "                                     (/theme list for all presets)\n"
                "  arbiter --connect [URL] [--token TOKEN] [--theme PRESET]\n"
                "                                     Thin-client TUI against a remote\n"
                "                                     `arbiter --api` (no local provider keys).\n"
                "                                     URL/token from ARBITER_API_URL /\n"
                "                                     ARBITER_API_TOKEN when omitted. Prefer\n"
                "                                     env for the token (visible in `ps` via\n"
                "                                     --token).\n"
                "  arbiter --api [--port N] [--bind ADDR] [--verbose] [--allow-host-exec]\n"
                "                                     HTTP+SSE orchestration API (default 127.0.0.1:8080).\n"
                "                                     --verbose mirrors every SSE event (text deltas, tool calls,\n"
                "                                     thinking, etc.) to stderr.  Env: ARBITER_API_VERBOSE=1.\n"
                "                                     --allow-host-exec permits agents to run shell commands on\n"
                "                                     the host via popen().  WARNING: agents run as this process's\n"
                "                                     user.  Also: ARBITER_ALLOW_HOST_EXEC=1.\n"
                "  arbiter --send <agent> <msg>       One-shot message\n"
                "  arbiter --export-theme PRESET      Write full theme JSON to stdout\n"
                "  arbiter --init [--force]           Initialize config + example agents\n"
                "                                     --force overwrites existing ~/.arbiter/agents/*.json files;\n"
                "                                     omit it to preserve user-edited agent definitions.\n"
                "  arbiter --setup-tools              Interactive wizard for /search, /browse, and MCP\n"
                "                                     Writes ~/.arbiter/search_api_key and mcp_servers.json.\n"
                "  arbiter --help                     This help\n\n"
                "Tenants (for --api):\n"
                "  arbiter --add-tenant <name>              Provision a tenant + API key\n"
                "  arbiter --list-tenants                   List tenants\n"
                "  arbiter --rotate-tenant-token <id|name>  Issue a new API key (invalidates old)\n"
                "  arbiter --disable-tenant <id|name>       Revoke a tenant's access\n"
                "  arbiter --enable-tenant  <id|name>       Restore a tenant's access\n\n"
                "Environment:\n"
                "  OPENROUTER_API_KEY                 OpenRouter key for hosted models\n"
                "  OLLAMA_HOST                        Ollama server URL (default http://localhost:11434)\n"
                "  ARBITER_API_URL                    Default --connect base URL\n"
                "  ARBITER_API_TOKEN                  Default --connect bearer token\n"
                "Config: ~/.arbiter/\n"
                "  openrouter_api_key                 OpenRouter key file\n"
                "  search_api_key                     Brave Search key for /search\n"
                "  mcp_servers.json                   MCP server registry (/mcp, /browse)\n"
                "  tui.json                           Theme preset, theme_file, or overrides\n"
                "  themes/*.json                      Custom theme documents\n"
                "  tenants.db                         Tenant identity store (--api)\n"
                "  agents/*.json                      Agent constitutions\n";
            return 0;
        }

        std::cerr << "Unknown option: " << arg1 << ". Try --help\n";
        return 1;

    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 1;
    }
}
