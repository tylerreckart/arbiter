#include "atomic_file.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <system_error>

namespace arbiter {

bool atomic_write_file(const std::string& path, const std::string& data) {
    const std::string tmp_path = path + ".tmp";

    // Drop a leftover crash file or a planted name. unlink(2) removes a
    // symlink itself, so this cannot clobber the target of
    // `<path>.tmp` → elsewhere. fopen("wb") used to follow that link
    // and truncate the target before rename replaced only the symlink.
    ::unlink(tmp_path.c_str());

    const int fd = ::open(tmp_path.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          0666);
    if (fd < 0) return false;

    size_t off = 0;
    bool ok = true;
    while (ok && off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
        } else if (n == 0) {
            ok = false;
        } else {
            off += static_cast<size_t>(n);
        }
    }
    if (ok) ok = (::fsync(fd) == 0);
    ::close(fd);

    if (!ok) {
        ::unlink(tmp_path.c_str());
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        ::unlink(tmp_path.c_str());
        return false;
    }
    return true;
}

} // namespace arbiter
