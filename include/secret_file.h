#pragma once
// arbiter/include/secret_file.h — owner-only dest writes that do not
// follow a planted symlink or other non-regular dest.
//
// Used for ~/.arbiter API-key files and the generated admin token.

#include <string>

namespace arbiter {

// Write `data` to `path` at mode 0600.  Opens with O_NOFOLLOW so a
// dest symlink cannot redirect the bytes into its target, and refuses
// after open unless the fd is a regular file (FIFO / device / dir).
// Returns false on I/O failure or a non-regular dest.
bool write_secret_file(const std::string& path, const std::string& data);

} // namespace arbiter
