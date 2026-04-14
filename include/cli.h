#pragma once
// index/include/cli.h
//
// Non-REPL entry points.  Each function corresponds to one command-line mode:
//
//   index --init              → cmd_init       (generate tokens + example agents)
//   index --gen-token         → cmd_gen_token  (add a new auth token)
//   index --serve [--port N]  → cmd_serve      (TCP server loop)
//   index --send <a> <msg>    → cmd_oneshot    (one-turn request, no TUI)
//
// The interactive REPL (cmd_interactive) is still in main.cpp until we finish
// carving up that function.

#include <string>

namespace index_ai {

void cmd_init();
void cmd_gen_token();
void cmd_serve(int port);
void cmd_oneshot(const std::string& agent_id, const std::string& msg);

} // namespace index_ai
