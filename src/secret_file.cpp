#include "secret_file.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arbiter {

bool write_secret_file(const std::string& path, const std::string& data) {
    // O_NOFOLLOW: a planted dest symlink used to send API keys / adm_
    // tokens into the link target (open(O_CREAT|O_TRUNC) follows).
    // O_NONBLOCK: a planted FIFO must not hang the wizard / --api
    // startup waiting for a reader — and must not deliver the secret
    // if a reader is already attached.
    const int fd = ::open(path.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW |
                              O_CLOEXEC | O_NONBLOCK,
                          0600);
    if (fd < 0) return false;

    auto fail = [fd](int err = 0) {
        if (err != 0) errno = err;
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return false;
    };

    struct stat st{};
    if (::fstat(fd, &st) != 0) return fail();
    if (!S_ISREG(st.st_mode)) return fail(EINVAL);

    // open(2) mode is masked by umask — fchmod so the secret is never
    // briefly group/world-readable.  Path chmod is not used: it follows
    // a dest that is swapped for a symlink after open.
    if (::fchmod(fd, 0600) != 0) return fail();

    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail();
        }
        if (n == 0) return fail(EIO);
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) return fail();
    ::close(fd);
    return true;
}

} // namespace arbiter
