#pragma once
// arbiter/include/scheduler.h
//
// Background scheduler subsystem owned by the API server.  Ticks every
// `tick_interval_seconds` (default 30s), polls the DB for due ScheduledTask
// rows (status='active' AND next_fire_at <= now), and fires each one
// through a synchronous orchestrator turn.  A TaskRun row is appended for
// every fire; the row reaches a terminal status before the tick returns.
//
// Recurring tasks have their `next_fire_at` advanced via the schedule_parser
// helper; one-shot tasks transition to status='completed' after a successful
// fire (or 'failed' if the orchestrator throws — operators can DELETE the
// task to restart, or PATCH status back to 'active' after fixing the cause).
//
// Notifications: every terminal run (succeeded | failed) is published on
// the NotificationBus passed at construction.  Subscribers (the SSE
// /v1/notifications/stream handler) wake and emit to their clients.
//
// Lifecycle: start() spawns the tick thread; stop() signals shutdown and
// joins.  Destructor calls stop().  Safe to construct without start() —
// CRUD methods still work (the writ uses them at request time).

#include "notification_bus.h"
#include "tenant_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace arbiter {

struct ApiServerOptions;

class Scheduler {
public:
    Scheduler(ApiServerOptions* opts,
              TenantStore* tenants,
              NotificationBus* bus,
              int tick_interval_seconds = 30);
    ~Scheduler();

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    void stop();

    bool running() const { return running_.load(); }

    // Test hook: run one tick synchronously, ignoring the interval timer.
    // Returns the number of tasks fired.
    int tick_once_for_test();

private:
    void tick_loop();
    void process_due_tasks();
    void fire_task(const TenantStore::ScheduledTask& task);

    ApiServerOptions*       opts_      = nullptr;
    TenantStore*            tenants_   = nullptr;
    NotificationBus*        bus_       = nullptr;
    int                     interval_s_ = 30;

    std::atomic<bool>       running_{false};
    std::thread             tick_thread_;
    std::mutex              sleep_mu_;
    std::condition_variable sleep_cv_;
};

} // namespace arbiter
