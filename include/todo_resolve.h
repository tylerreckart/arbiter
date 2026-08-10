#pragma once

#include "tenant_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace arbiter {

// Resolve a `/todo start|done|cancel|delete` target.
//
// Accepts a numeric id (`14`, `#14`) or a subject string that uniquely
// matches one of `candidates` (exact, then case-insensitive exact, then
// unique case-insensitive prefix).  Returns the todo id on success, or 0
// with `err_out` set on failure / ambiguity.
[[nodiscard]] int64_t resolve_todo_target(
    const std::string& args,
    const std::vector<TenantStore::Todo>& candidates,
    std::string& err_out);

// Same matching rules against sidebar todo subjects (id + subject pairs).
struct TodoSubjectRef {
    int id = 0;
    std::string subject;
};
[[nodiscard]] int resolve_todo_subject_ref(
    const std::string& args,
    const std::vector<TodoSubjectRef>& candidates,
    std::string& err_out);

} // namespace arbiter
