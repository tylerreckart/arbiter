// index_ai/src/server.cpp — TCP server for remote CLI
#include "server.h"

#include <cstring>
#include <sstream>
#include <algorithm>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

namespace index_ai {

Server::Server(Orchestrator& orch, Auth& auth, int port)
    : orch_(orch), auth_(auth), port_(port)
{}

Server::~Server() {
    stop();
}

void Server::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket() failed");

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(listen_fd_);
        throw std::runtime_error("bind() failed on port " + std::to_string(port_));
    }

    if (listen(listen_fd_, 16) < 0) {
        close(listen_fd_);
        throw std::runtime_error("listen() failed");
    }

    running_ = true;
    accept_thread_ = std::thread(&Server::accept_loop, this);
}

void Server::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (auto& w : client_threads_) {
        if (w.thread.joinable()) w.thread.join();
    }
    client_threads_.clear();
}

void Server::accept_loop() {
    while (running_) {
        struct sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &len);
        if (client_fd < 0) {
            if (!running_) break;
            continue;
        }

        std::lock_guard<std::mutex> lock(threads_mutex_);
        // Reap workers that have already signalled completion.  Each worker
        // sets its `done` flag as the last thing it does before returning,
        // so we can safely join here without blocking.
        for (auto it = client_threads_.begin(); it != client_threads_.end(); ) {
            if (it->done && it->done->load()) {
                if (it->thread.joinable()) it->thread.join();
                it = client_threads_.erase(it);
            } else {
                ++it;
            }
        }
        auto done_flag = std::make_shared<std::atomic<bool>>(false);
        client_threads_.push_back(ClientWorker{
            std::thread([this, client_fd, done_flag]() {
                handle_client(client_fd);
                done_flag->store(true);
            }),
            done_flag,
        });
    }
}

static std::string read_line(int fd) {
    std::string line;
    char c;
    while (true) {
        int n = static_cast<int>(recv(fd, &c, 1, 0));
        if (n <= 0) return "";
        if (c == '\n') break;
        if (c != '\r') line += c;
    }
    return line;
}

static void send_line(int fd, const std::string& msg) {
    std::string out = msg + "\n";
    send(fd, out.data(), out.size(), 0);
}

void Server::handle_client(int client_fd) {
    send_line(client_fd, "index_ai v0.1 — AUTH required");
    bool authenticated = false;

    while (running_) {
        std::string line = read_line(client_fd);
        if (line.empty()) break;

        std::string response = process_command(line, authenticated);
        send_line(client_fd, response);

        if (line == "QUIT" || line == "quit") break;
    }

    close(client_fd);
}

std::string Server::process_command(const std::string& line, bool& authenticated) {
    // Tokenize
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    // Convert to uppercase for command matching
    std::string CMD = cmd;
    std::transform(CMD.begin(), CMD.end(), CMD.begin(), ::toupper);

    // --- AUTH ---
    if (CMD == "AUTH") {
        std::string token;
        iss >> token;
        if (auth_.validate(token)) {
            authenticated = true;
            return "OK authenticated";
        }
        return "ERR invalid token";
    }

    if (!authenticated) {
        return "ERR not authenticated. Send: AUTH <token>";
    }

    // --- Commands requiring auth ---

    if (CMD == "SEND" || CMD == "MSG") {
        // SEND <agent_id> <message...>
        std::string agent_id;
        iss >> agent_id;
        std::string msg;
        std::getline(iss, msg);
        // trim leading space
        if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);

        if (agent_id.empty() || msg.empty())
            return "ERR usage: SEND <agent_id> <message>";

        try {
            auto resp = orch_.send(agent_id, msg);
            if (!resp.ok) return "ERR " + resp.error;
            std::ostringstream os;
            os << "OK [in:" << resp.input_tokens
               << " out:" << resp.output_tokens << "] "
               << resp.content;
            return os.str();
        } catch (const std::exception& e) {
            return std::string("ERR ") + e.what();
        }
    }

    if (CMD == "LIST") {
        auto agents = orch_.list_agents();
        std::ostringstream os;
        os << "OK agents:" << agents.size();
        for (auto& id : agents) os << " " << id;
        return os.str();
    }

    if (CMD == "STATUS") {
        return "OK\n" + orch_.global_status();
    }

    if (CMD == "CREATE") {
        // CREATE <agent_id> <json_config>
        std::string agent_id;
        iss >> agent_id;
        std::string json_rest;
        std::getline(iss, json_rest);
        if (!json_rest.empty() && json_rest[0] == ' ') json_rest.erase(0, 1);

        if (agent_id.empty())
            return "ERR usage: CREATE <agent_id> <json_config>";

        try {
            Constitution config = json_rest.empty()
                ? master_constitution()
                : Constitution::from_json(json_rest);
            if (config.name.empty()) config.name = agent_id;
            orch_.create_agent(agent_id, std::move(config));
            return "OK created " + agent_id;
        } catch (const std::exception& e) {
            return std::string("ERR ") + e.what();
        }
    }

    if (CMD == "REMOVE" || CMD == "DELETE") {
        std::string agent_id;
        iss >> agent_id;
        orch_.remove_agent(agent_id);
        return "OK removed " + agent_id;
    }

    if (CMD == "RESET") {
        std::string agent_id;
        iss >> agent_id;
        try {
            orch_.get_agent(agent_id).reset_history();
            return "OK history cleared for " + agent_id;
        } catch (const std::exception& e) {
            return std::string("ERR ") + e.what();
        }
    }

    if (CMD == "ASK") {
        // ASK <query...> — ask index_ai master
        std::string query;
        std::getline(iss, query);
        if (!query.empty() && query[0] == ' ') query.erase(0, 1);
        try {
            auto resp = orch_.ask_index_ai(query);
            if (!resp.ok) return "ERR " + resp.error;
            std::ostringstream os;
            os << "OK [in:" << resp.input_tokens
               << " out:" << resp.output_tokens << "] "
               << resp.content;
            return os.str();
        } catch (const std::exception& e) {
            return std::string("ERR ") + e.what();
        }
    }

    if (CMD == "TOKENS") {
        std::ostringstream os;
        os << "OK total_in:" << orch_.total_input_tokens()
           << " total_out:" << orch_.total_output_tokens();
        return os.str();
    }

    if (CMD == "QUIT" || CMD == "EXIT") {
        return "OK bye";
    }

    if (CMD == "HELP") {
        return "OK commands: AUTH SEND LIST STATUS CREATE REMOVE RESET ASK TOKENS HELP QUIT";
    }

    return "ERR unknown command. Send HELP";
}

} // namespace index_ai
