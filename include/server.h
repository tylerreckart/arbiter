#pragma once
// index/include/server.h — TCP server for remote CLI access
// Simple line-protocol: AUTH <token>\n then AGENT <id> MSG <text>\n

#include "orchestrator.h"
#include "auth.h"
#include <string>
#include <atomic>
#include <memory>
#include <mutex>
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
    // Each worker carries a shared `done` flag set by the worker right
    // before it returns.  accept_loop scans the vector and reaps any whose
    // flag is set — lets us bound the vector without resorting to detach,
    // which would leak orphaned threads if stop() races with a new client.
    struct ClientWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<ClientWorker> client_threads_;
    std::mutex threads_mutex_;

    void accept_loop();
    void handle_client(int client_fd);
    std::string process_command(const std::string& line, bool& authenticated);
};

} // namespace index_ai
