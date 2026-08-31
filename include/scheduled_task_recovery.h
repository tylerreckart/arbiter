#pragma once
// Finalize scheduled_tasks left in status='running' after a crash or kill.
// Called from API startup after recover_running_task_runs() marks orphaned
// task_run rows failed.

#include "tenant_store.h"

#include <cstdint>

namespace arbiter {

void finalize_orphaned_scheduled_task_leases(TenantStore& tenants,
                                            int64_t completed_at);

} // namespace arbiter
