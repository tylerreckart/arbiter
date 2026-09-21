#pragma once
// arbiter/include/atomic_file.h — crash-safe file writes.

#include <string>

namespace arbiter {

// Write `data` to `path` atomically: write to `<path>.tmp` in the same
// directory, fsync it, then rename over the target. A crash mid-write
// leaves the previous file (or no file) intact instead of a truncated or
// corrupt one. The staging name is opened with O_NOFOLLOW / O_EXCL so a
// planted `<path>.tmp` symlink cannot redirect the write. Returns false
// if the write or rename failed.
bool atomic_write_file(const std::string& path, const std::string& data);

} // namespace arbiter
