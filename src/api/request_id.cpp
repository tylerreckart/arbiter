// arbiter/src/api/request_id.cpp

#include "api/request_id.h"

#include <atomic>
#include <cstring>
#include <string>

#include <openssl/rand.h>

namespace arbiter {

std::string new_request_id() {
    unsigned char buf[8];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        static std::atomic<uint64_t> ctr{0};
        uint64_t n = ctr.fetch_add(1);
        std::memcpy(buf, &n, sizeof(buf));
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (unsigned char c : buf) {
        out += hex[c >> 4];
        out += hex[c & 0xF];
    }
    return out;
}

} // namespace arbiter
