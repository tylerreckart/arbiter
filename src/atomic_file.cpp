#include "atomic_file.h"

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <unistd.h>

namespace arbiter {

bool atomic_write_file(const std::string& path, const std::string& data) {
    const std::string tmp_path = path + ".tmp";

    FILE* f = std::fopen(tmp_path.c_str(), "wb");
    if (!f) return false;

    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (ok) ok = std::fflush(f) == 0;
    if (ok) ok = ::fsync(fileno(f)) == 0;
    std::fclose(f);

    if (!ok) {
        std::remove(tmp_path.c_str());
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

} // namespace arbiter
