// arbiter/src/repl/pane.cpp — Pane special members.
//
// CancelToken is incomplete in pane.h (keeps OpenSSL out of light TUI test
// TUs).  shared_ptr destruction requires a complete type, so ctor/dtor live
// here where api_client.h is visible.

#include "repl/pane.h"
#include "api_client.h"

namespace arbiter {

Pane::Pane() = default;
Pane::~Pane() = default;

}  // namespace arbiter
