#pragma once
// index/include/cli.h
//
// Non-REPL entry points.  Each function corresponds to one command-line mode:
//
//   arbiter --init                    → cmd_init       (create example agents + config dir)
//   arbiter --api  [--port N]         → cmd_api        (HTTP+SSE orchestration API)
//   arbiter --send <a> <msg>          → cmd_oneshot    (one-turn request, no TUI)
//
//   Tenant management (for `--api` mode).  Billing — eligibility, caps,
//   and the usage ledger — lives in an external billing service;
//   this CLI only manages local tenant identity (name, token, disabled
//   flag).
//     arbiter --add-tenant <name>                → cmd_add_tenant
//     arbiter --list-tenants                     → cmd_list_tenants
//     arbiter --disable-tenant <id|name>         → cmd_disable_tenant
//     arbiter --enable-tenant <id|name>          → cmd_enable_tenant
//
// The interactive REPL (cmd_interactive) is still in main.cpp until we finish
// carving up that function.

#include <cstdint>
#include <string>

namespace index_ai {

void cmd_init();
void cmd_api(int port, const std::string& bind, bool verbose);
void cmd_oneshot(const std::string& agent_id, const std::string& msg);

// Tenant admin.  Each opens ~/.arbiter/tenants.db, runs one operation, and
// prints a human-readable report.
void cmd_add_tenant(const std::string& name);
void cmd_list_tenants();
void cmd_disable_tenant(const std::string& key);
void cmd_enable_tenant(const std::string& key);

} // namespace index_ai
