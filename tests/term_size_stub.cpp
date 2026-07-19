// Minimal stubs so TUI unit tests can link without cli_helpers.cpp
// (which pulls OpenTUI Engine, starters, commands, …).
#include "cli_helpers.h"

namespace arbiter {

int term_cols() { return 80; }
int term_rows() { return 24; }

} // namespace arbiter
