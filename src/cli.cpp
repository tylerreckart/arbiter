// index_ai/src/cli.cpp — see cli.h

#include "cli.h"
#include "cli_helpers.h"
#include "auth.h"
#include "constitution.h"
#include "orchestrator.h"
#include "server.h"
#include "starters.h"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace index_ai {

// Shared SIGINT/SIGTERM flag used by cmd_serve.
namespace {
volatile std::sig_atomic_t g_running = 1;
void signal_handler(int) { g_running = 0; }
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

    std::cout << "Initialized ~/.index/\n";
    std::cout << "Auth token (save this): " << token << "\n";
    std::cout << "Tokens stored (hashed) in: " << token_path << "\n\n";

    for (auto& starter : starter_agents()) {
        starter.config.save(agents_dir + "/" + starter.id + ".json");
    }
    std::cout << "Example agents created in " << agents_dir << "/\n";
    for (auto& starter : starter_agents()) {
        std::cout << "  " << starter.id << ".json — " << starter.blurb << "\n";
    }
    std::cout << "\nEdit these or add your own. Then run: index\n";
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
        std::cerr << "WARN: No auth tokens. Run: index_ai --init\n";
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

} // namespace index_ai
