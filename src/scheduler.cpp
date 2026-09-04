// arbiter/src/scheduler.cpp
//
// Tick thread + fire path for ScheduledTask rows.

#include "scheduler.h"

#include "api/in_flight_scope.h"
#include "api_server.h"
#include "orchestrator.h"
#include "schedule_parser.h"
#include "tenant_limiter.h"
#include "tenant_store.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>

namespace arbiter {

namespace {

int64_t now_epoch() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

constexpr size_t kResultSummaryMax = 4096;   // bytes; longer replies are tail-truncated
constexpr char kTenantFacingRunError[] =
    "the scheduled task failed";

std::string truncate_summary(const std::string& s) {
    if (s.size() <= kResultSummaryMax) return s;
    std::string out = s.substr(0, kResultSummaryMax);
    out += "\n…[truncated]";
    return out;
}

std::string short_request_id() {
    // 16 hex chars, modest entropy.  Used to correlate runs with logs;
    // not a security boundary.
    static const char kHex[] = "0123456789abcdef";
    char buf[17];
    int64_t t = now_epoch();
    for (int i = 15; i >= 0; --i) {
        buf[i] = kHex[t & 0xF];
        t >>= 4;
    }
    buf[16] = '\0';
    // Mix in a per-call counter so two ids issued the same second still differ.
    static std::atomic<uint64_t> ctr{0};
    uint64_t k = ctr.fetch_add(1);
    for (int i = 0; i < 8; ++i) {
        buf[i] ^= kHex[(k >> (i * 4)) & 0xF];
        // ASCII XOR can land outside hex; remap into [0-9a-f]
        buf[i] = kHex[buf[i] & 0xF];
    }
    return std::string(buf);
}

} // namespace

Scheduler::Scheduler(ApiServerOptions* opts,
                      TenantStore* tenants,
                      NotificationBus* bus,
                      InFlightRegistry* in_flight,
                      TenantLimiter* limiter,
                      int tick_interval_seconds)
    : opts_(opts), tenants_(tenants), bus_(bus), in_flight_(in_flight),
      limiter_(limiter), interval_s_(tick_interval_seconds) {
    if (interval_s_ < 1) interval_s_ = 1;
}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    bool was = running_.exchange(true);
    if (was) return;
    tick_thread_ = std::thread([this]{ tick_loop(); });
}

void Scheduler::stop() {
    bool was = running_.exchange(false);
    if (!was) return;
    {
        std::lock_guard<std::mutex> lk(sleep_mu_);
        sleep_cv_.notify_all();
    }
    if (tick_thread_.joinable()) tick_thread_.join();
}

int Scheduler::tick_once_for_test() {
    int before = 0; (void)before;
    int fired = 0;
    if (!tenants_) return 0;
    auto due = tenants_->list_due_scheduled_tasks(now_epoch(), /*limit=*/200);
    for (const auto& t : due) {
        if (fire_task(t)) ++fired;
    }
    return fired;
}

void Scheduler::tick_loop() {
    while (running_.load()) {
        try {
            process_due_tasks();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[scheduler] tick error: %s\n", e.what());
        }
        std::unique_lock<std::mutex> lk(sleep_mu_);
        sleep_cv_.wait_for(lk, std::chrono::seconds(interval_s_),
                            [this]{ return !running_.load(); });
    }
}

void Scheduler::process_due_tasks() {
    if (!tenants_) return;
    auto due = tenants_->list_due_scheduled_tasks(now_epoch(), /*limit=*/200);
    for (const auto& t : due) {
        if (!running_.load()) return;
        try {
            fire_task(t);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[scheduler] fire failed for task %lld: %s\n",
                (long long)t.id, e.what());
        }
    }
}

bool Scheduler::fire_task(const TenantStore::ScheduledTask& task) {
    if (!opts_ || !tenants_) return false;

    // Resolve the tenant — needed for orchestrator construction and
    // notification delivery.  A scheduled task whose tenant has been
    // hard-deleted will fail this lookup; the cascade should have
    // dropped the row, but defend anyway.
    auto tenant_opt = tenants_->get_tenant(task.tenant_id);
    if (!tenant_opt) {
        std::fprintf(stderr,
            "[scheduler] task %lld references unknown tenant %lld; deleting\n",
            (long long)task.id, (long long)task.tenant_id);
        tenants_->delete_scheduled_task(task.tenant_id, task.id);
        return false;
    }
    if (tenant_opt->disabled) {
        // Don't fire; just push the next_fire_at out.  Operators flipping
        // a tenant back online see schedules resume on the next tick.
        int64_t next = task.schedule_kind == "recurring"
            ? next_fire_for_recur(task.recur_json, now_epoch())
            : 0;
        if (next == 0 && task.schedule_kind == "recurring") {
            next = now_epoch() + 3600;   // fallback re-poll in 1h
        }
        if (task.schedule_kind == "recurring") {
            tenants_->update_scheduled_task(task.tenant_id, task.id,
                std::nullopt, next, std::nullopt, std::nullopt, std::nullopt);
        } else {
            // One-shot for a disabled tenant: hold until re-enabled.
            tenants_->update_scheduled_task(task.tenant_id, task.id,
                std::optional<std::string>("paused"),
                std::nullopt, std::nullopt, std::nullopt, std::nullopt);
        }
        return false;
    }

    // Scoped schedules must reference a live HTTP conversation row.
    if (task.conversation_id > 0 &&
        !is_http_scoped_conversation(*tenants_, task.tenant_id,
                                     task.conversation_id)) {
        std::fprintf(stderr,
            "[scheduler] task %lld references missing conversation %lld; pausing\n",
            (long long)task.id, (long long)task.conversation_id);
        tenants_->update_scheduled_task(task.tenant_id, task.id,
            std::optional<std::string>("paused"),
            std::nullopt, std::nullopt, std::nullopt, std::nullopt);
        return false;
    }

    // Claim the task before limiter acquire so a lost claim race does not
    // burn a rate token.  Status='running' is the lease; next_fire_at
    // stays put until the run finishes.
    const int64_t now = now_epoch();
    if (!tenants_->try_claim_scheduled_task(task.tenant_id, task.id, now)) {
        return false;
    }

    std::optional<TenantLimiter::Guard> limit_guard;
    if (limiter_) {
        auto lim = limiter_->acquire(task.tenant_id);
        if (!lim.granted()) {
            std::fprintf(stderr,
                "[scheduler] task %lld deferred: tenant %lld at %s limit\n",
                (long long)task.id, (long long)task.tenant_id,
                lim.kind == TenantLimiter::Result::Kind::ConcurrentExceeded
                    ? "concurrent" : "rate");
            tenants_->update_scheduled_task(task.tenant_id, task.id,
                std::optional<std::string>("active"),
                std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                std::optional<std::string>("running"));
            return false;
        }
        limit_guard = std::move(lim.guard);
    }

    const int64_t  started_at = now;
    const std::string req_id  = short_request_id();
    const std::optional<std::string> k_running_lease("running");
    auto run = tenants_->create_task_run(task.tenant_id, task.id,
                                          /*status=*/"running",
                                          started_at, req_id);

    if (bus_) {
        Notification n;
        n.kind         = Notification::Kind::RunStarted;
        n.tenant_id    = task.tenant_id;
        n.task_id      = task.id;
        n.run_id       = run.id;
        n.status       = "running";
        n.agent_id     = task.agent_id;
        n.started_at   = started_at;
        bus_->publish(n);
    }

    std::string init_err;
    auto orch = build_blocking_orchestrator(*opts_, *tenants_, *tenant_opt,
                                            init_err, task.conversation_id);

    if (!orch) {
        const int64_t completed_at = now_epoch();
        tenants_->update_task_run(task.tenant_id, run.id,
            std::optional<std::string>("failed"),
            completed_at,
            std::nullopt,
            std::optional<std::string>(init_err),
            std::nullopt, std::nullopt,
            std::optional<bool>(true));
        if (bus_) {
            Notification n;
            n.kind          = Notification::Kind::RunFailed;
            n.tenant_id     = task.tenant_id;
            n.task_id       = task.id;
            n.run_id        = run.id;
            n.status        = "failed";
            n.agent_id      = task.agent_id;
            n.started_at    = started_at;
            n.completed_at  = completed_at;
            n.error_message = init_err;
            bus_->publish(n);
        }
        // Keep the schedule active so a transient init failure can retry.
        if (task.schedule_kind == "recurring") {
            int64_t next = next_fire_for_recur(task.recur_json, now_epoch());
            if (next == 0) next = now_epoch() + 3600;
            tenants_->update_scheduled_task(task.tenant_id, task.id,
                std::optional<std::string>("active"), next,
                std::optional<int64_t>(started_at),
                std::optional<int64_t>(run.id),
                std::optional<int64_t>(1),
                k_running_lease);
        } else {
            tenants_->update_scheduled_task(task.tenant_id, task.id,
                std::optional<std::string>("failed"),
                std::nullopt,
                std::optional<int64_t>(started_at),
                std::optional<int64_t>(run.id),
                std::optional<int64_t>(1),
                k_running_lease);
        }
        return true;
    }

    // Disallow targeting an unknown agent for non-`index` ids.  The master
    // orchestrator (`index`) is always available; named agents must be in
    // the tenant catalog.
    if (task.agent_id != "index" && !orch->has_agent(task.agent_id)) {
        const int64_t completed_at = now_epoch();
        std::string err = "agent '" + task.agent_id +
            "' not found in tenant catalog; pause this schedule or restore the agent";
        tenants_->update_task_run(task.tenant_id, run.id,
            std::optional<std::string>("failed"),
            completed_at, std::nullopt, std::optional<std::string>(err),
            std::nullopt, std::nullopt, std::optional<bool>(true));
        tenants_->update_scheduled_task(task.tenant_id, task.id,
            std::optional<std::string>("paused"),
            std::nullopt,
            std::optional<int64_t>(started_at),
            std::optional<int64_t>(run.id),
            std::optional<int64_t>(1),
            k_running_lease);
        if (bus_) {
            Notification n;
            n.kind          = Notification::Kind::RunFailed;
            n.tenant_id     = task.tenant_id;
            n.task_id       = task.id;
            n.run_id        = run.id;
            n.status        = "failed";
            n.agent_id      = task.agent_id;
            n.started_at    = started_at;
            n.completed_at  = completed_at;
            n.error_message = err;
            bus_->publish(n);
        }
        return true;
    }

    auto fail_run = [&](const std::string& err,
                        const std::optional<std::string>& task_status) {
        const int64_t completed_at = now_epoch();
        tenants_->update_task_run(task.tenant_id, run.id,
            std::optional<std::string>("failed"),
            completed_at, std::nullopt, std::optional<std::string>(err),
            std::nullopt, std::nullopt, std::optional<bool>(true));
        if (task_status) {
            tenants_->update_scheduled_task(task.tenant_id, task.id,
                task_status, std::nullopt,
                std::optional<int64_t>(started_at),
                std::optional<int64_t>(run.id),
                std::optional<int64_t>(1),
                k_running_lease);
        } else if (task.schedule_kind == "recurring") {
            int64_t next = next_fire_for_recur(task.recur_json, completed_at);
            if (next == 0) next = completed_at + 3600;
            tenants_->update_scheduled_task(task.tenant_id, task.id,
                std::optional<std::string>("active"), next,
                std::optional<int64_t>(completed_at),
                std::optional<int64_t>(run.id),
                std::optional<int64_t>(1),
                k_running_lease);
        } else {
            tenants_->update_scheduled_task(task.tenant_id, task.id,
                std::optional<std::string>("failed"),
                std::nullopt,
                std::optional<int64_t>(completed_at),
                std::optional<int64_t>(run.id),
                std::optional<int64_t>(1),
                k_running_lease);
        }
        if (bus_) {
            Notification n;
            n.kind          = Notification::Kind::RunFailed;
            n.tenant_id     = task.tenant_id;
            n.task_id       = task.id;
            n.run_id        = run.id;
            n.status        = "failed";
            n.agent_id      = task.agent_id;
            n.started_at    = started_at;
            n.completed_at  = completed_at;
            n.error_message = err;
            bus_->publish(n);
        }
    };

    std::optional<BlockingConversationTurnGuard> prepared_turn;
    if (task.conversation_id > 0) {
        auto log_prepare = [](const std::string& msg) {
            std::fprintf(stderr, "[scheduler] %s\n", msg.c_str());
        };
        int64_t prepared_user_message_id = 0;
        if (!prepare_blocking_conversation_turn(
                *orch, *tenants_, task.tenant_id, task.conversation_id,
                task.agent_id, task.message, req_id, log_prepare,
                &prepared_user_message_id)) {
            // Transient hydration failures should retry — mirror init-failure
            // handling (keep recurring schedules active with next_fire_at).
            const std::optional<std::string> task_status =
                task.schedule_kind == "recurring"
                    ? std::nullopt
                    : std::optional<std::string>("failed");
            fail_run("conversation history could not be loaded", task_status);
            return true;
        }
        prepared_turn.emplace(*tenants_, task.tenant_id, task.conversation_id,
                              prepared_user_message_id);
    }

    // Run one turn synchronously.  Sub-agent delegation, tool calls, and
    // memory bridges all execute inline; the call returns when the agent
    // hits a terminal turn (no more tool calls).
    ApiResponse resp;
    std::string err;
    bool ok = false;
    try {
        if (in_flight_) {
            InFlightScope in_flight_scope(*in_flight_, req_id, orch.get(),
                                          task.tenant_id);
            resp = orch->send(task.agent_id, task.message);
        } else {
            resp = orch->send(task.agent_id, task.message);
        }
        ok = resp.ok;
        if (!ok) {
            std::fprintf(stderr,
                "[scheduler] task %lld failed: %s\n",
                (long long)task.id, resp.error.c_str());
            err = kTenantFacingRunError;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[scheduler] task %lld threw: %s\n",
            (long long)task.id, e.what());
        err = kTenantFacingRunError;
        ok = false;
    }

    persist_blocking_conversation_turn(
        *orch, *tenants_, task.tenant_id, task.conversation_id,
        task.agent_id, resp, req_id, true,
        [](const std::string& msg) {
            std::fprintf(stderr, "[scheduler] %s\n", msg.c_str());
        });
    if (ok && prepared_turn) prepared_turn->commit();
    else prepared_turn.reset();

    const int64_t completed_at = now_epoch();
    const std::string final_text = ok ? truncate_summary(resp.content) : "";

    tenants_->update_task_run(task.tenant_id, run.id,
        std::optional<std::string>(ok ? "succeeded" : "failed"),
        completed_at,
        std::optional<std::string>(final_text),
        std::optional<std::string>(err),
        std::optional<int64_t>(resp.input_tokens),
        std::optional<int64_t>(resp.output_tokens),
        std::optional<bool>(true));

    // Advance the parent task and drop the in-flight lease.
    auto finalize_parent = [&](const std::optional<std::string>& status,
                               const std::optional<int64_t>& next_fire) {
        if (tenants_->update_scheduled_task(
                task.tenant_id, task.id, status, next_fire,
                std::optional<int64_t>(completed_at),
                std::optional<int64_t>(run.id),
                std::optional<int64_t>(1),
                k_running_lease)) {
            return;
        }
        std::fprintf(stderr,
            "[scheduler] task %lld finalize CAS miss (tenant=%lld no longer running)\n",
            static_cast<long long>(task.id),
            static_cast<long long>(task.tenant_id));
        // If the lease is still held, release it so the schedule is not
        // stuck invisible to the tick query until the next restart.
        auto row = tenants_->get_scheduled_task(task.tenant_id, task.id);
        if (!row || row->status != "running") return;
        tenants_->update_scheduled_task(
            task.tenant_id, task.id, status, next_fire,
            std::optional<int64_t>(completed_at),
            std::optional<int64_t>(run.id),
            std::optional<int64_t>(1));
    };

    if (task.schedule_kind == "recurring") {
        int64_t next = next_fire_for_recur(task.recur_json, completed_at);
        if (next == 0) {
            // Unparseable recur — pause to surface to the operator.
            finalize_parent(std::optional<std::string>("paused"), std::nullopt);
        } else {
            finalize_parent(std::optional<std::string>("active"),
                            std::optional<int64_t>(next));
        }
    } else {
        finalize_parent(
            std::optional<std::string>(ok ? "completed" : "failed"),
            std::nullopt);
    }

    if (bus_) {
        Notification n;
        n.kind           = ok ? Notification::Kind::RunCompleted
                               : Notification::Kind::RunFailed;
        n.tenant_id      = task.tenant_id;
        n.task_id        = task.id;
        n.run_id         = run.id;
        n.status         = ok ? "succeeded" : "failed";
        n.agent_id       = task.agent_id;
        n.started_at     = started_at;
        n.completed_at   = completed_at;
        n.result_summary = final_text;
        n.error_message  = err;
        bus_->publish(n);
    }
    return true;
}

} // namespace arbiter
