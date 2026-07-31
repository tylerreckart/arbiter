#pragma once
// arbiter/include/file_cap.h
//
// Per-response file byte cap accounting.  Used by /write interceptors
// so parallel sub-agents cannot overshoot file_max_bytes via a racy
// load-then-add sequence.

#include <atomic>
#include <cstddef>

namespace arbiter {

// Atomically reserve `size` bytes against a per-response file cap.
// Returns false when the reservation would exceed `cap`.
inline bool try_reserve_file_bytes(std::atomic<size_t>& captured,
                                   size_t size, size_t cap) {
    if (size > cap) return false;
    size_t prev = captured.load(std::memory_order_relaxed);
    for (;;) {
        // Guard against size_t wrap on prev + size.
        if (prev > cap - size) return false;
        if (captured.compare_exchange_weak(
                prev, prev + size, std::memory_order_relaxed))
            return true;
    }
}

// Undo a successful try_reserve_file_bytes when the write did not land.
inline void release_file_bytes(std::atomic<size_t>& captured, size_t size) {
    captured.fetch_sub(size, std::memory_order_relaxed);
}

} // namespace arbiter
