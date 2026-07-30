#include "tui/clipboard.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace arbiter {
namespace {

std::string base64_encode(std::string_view bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    if (bytes.empty()) return out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();

    while (i + 3 <= n) {
        const std::uint32_t v = (std::uint32_t(p[i]) << 16)
            | (std::uint32_t(p[i + 1]) << 8)
            | std::uint32_t(p[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
        i += 3;
    }
    if (i < n) {
        std::uint32_t v = std::uint32_t(p[i]) << 16;
        if (i + 1 < n) v |= std::uint32_t(p[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        if (i + 1 < n) {
            out.push_back(kAlphabet[(v >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

}  // namespace

bool clipboard_write_osc52(std::string_view text) {
    const std::string b64 = base64_encode(text);
    const std::string seq = "\033]52;c;" + b64 + "\a";

    // Prefer /dev/tty so we bypass OpenTUI's buffered renderer destination.
    int fd = ::open("/dev/tty", O_WRONLY | O_CLOEXEC);
    const bool owns = fd >= 0;
    if (!owns) fd = STDOUT_FILENO;
    const bool ok = write_all(fd, seq);
    if (owns) ::close(fd);
    return ok;
}

}  // namespace arbiter
