// Finalize scheduled_tasks left in status='running' after crash recovery.

#include "scheduled_task_recovery.h"

#include "schedule_parser.h"

namespace arbiter {

namespace {

constexpr const char kTaskRunInterruptedMsg[] =
    "task run was interrupted by a server restart";

bool task_run_was_interrupted(const TenantStore::TaskRun& run) {
    return run.status == "failed" &&
           run.error_message == kTaskRunInterruptedMsg;
}

} // namespace

void finalize_orphaned_scheduled_task_leases(TenantStore& tenants,
                                            int64_t completed_at) {
    while (true) {
        const auto stuck =
            tenants.list_all_scheduled_tasks_by_status("running", 200);
        if (stuck.empty()) break;

        for (const auto& task : stuck) {
            const auto runs = tenants.list_task_runs(task.tenant_id, task.id,
                                                     /*since_epoch=*/0,
                                                     /*limit=*/1);
            if (runs.empty()) {
                tenants.update_scheduled_task(task.tenant_id, task.id,
                    std::optional<std::string>("active"),
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                continue;
            }

            const auto& latest = runs.front();
            if (latest.status == "running" ||
                task_run_was_interrupted(latest)) {
                tenants.update_scheduled_task(task.tenant_id, task.id,
                    std::optional<std::string>("active"),
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                continue;
            }

            const int64_t run_at = latest.completed_at > 0
                ? latest.completed_at : completed_at;

            if (latest.status == "succeeded") {
                if (task.schedule_kind == "recurring") {
                    int64_t next = next_fire_for_recur(task.recur_json, run_at);
                    if (next == 0) {
                        tenants.update_scheduled_task(task.tenant_id, task.id,
                            std::optional<std::string>("paused"),
                            std::nullopt,
                            std::optional<int64_t>(run_at),
                            std::optional<int64_t>(latest.id),
                            std::nullopt);
                    } else {
                        tenants.update_scheduled_task(task.tenant_id, task.id,
                            std::optional<std::string>("active"),
                            std::optional<int64_t>(next),
                            std::optional<int64_t>(run_at),
                            std::optional<int64_t>(latest.id),
                            std::optional<int64_t>(1));
                    }
                } else {
                    tenants.update_scheduled_task(task.tenant_id, task.id,
                        std::optional<std::string>("completed"),
                        std::nullopt,
                        std::optional<int64_t>(run_at),
                        std::optional<int64_t>(latest.id),
                        std::optional<int64_t>(1));
                }
                continue;
            }

            if (task.schedule_kind == "recurring") {
                int64_t next = next_fire_for_recur(task.recur_json, run_at);
                if (next == 0) next = completed_at + 3600;
                tenants.update_scheduled_task(task.tenant_id, task.id,
                    std::optional<std::string>("active"),
                    std::optional<int64_t>(next),
                    std::optional<int64_t>(run_at),
                    std::optional<int64_t>(latest.id),
                    std::optional<int64_t>(1));
            } else {
                tenants.update_scheduled_task(task.tenant_id, task.id,
                    std::optional<std::string>("failed"),
                    std::nullopt,
                    std::optional<int64_t>(run_at),
                    std::optional<int64_t>(latest.id),
                    std::optional<int64_t>(1));
            }
        }

        if (stuck.size() < 200) break;
    }
}

} // namespace arbiter
