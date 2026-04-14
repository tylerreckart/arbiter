#pragma once
// index/include/server.h — TCP server for remote CLI access
// Simple line-protocol: AUTH <token>\n then AGENT <id> MSG <text>\n

#include "orchestrator.h"
#include "auth.h"
#include <string>
#include <atomic>
#include <thread>
#include <vector>

namespace index_ai {

class Server {
public:
    Server(Orchestrator& orch, Auth& auth, int port = 9077);
    ~Server();

    void start();
    void stop();
    bool running() const { return running_.load(); }

private:
    Orchestrator& orch_;
    Auth& auth_;
    int port_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
    std::mutex threads_mutex_;

    void accept_loop();
    void handle_client(int client_fd);
    std::string process_command(const std::string& line, bool& authenticated);
};

} // namespace index_ai
