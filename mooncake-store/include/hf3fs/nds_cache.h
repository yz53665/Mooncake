#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "hf3fs/nds.h"

#ifdef USE_NDS

namespace mooncake {
namespace nds_cache {

// In-process cache of NDS segment metadata. Entries are populated once per
// registered buffer at nds_init/registration time; the file I/O paths only do
// a cheap lookup here instead of calling nds_get_segment_info on every
// read/write. Segment metadata is constant for the lifetime of a registered
// buffer, so the cache stays valid until the buffer is deregistered.
struct CachedSegment {
    uintptr_t base;
    size_t size;
    nds_segment_infos_t infos;
};

inline std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

inline std::vector<CachedSegment>& Segments() {
    static std::vector<CachedSegment> segments;
    return segments;
}

// Called once per registered buffer, right after nds_buf_register succeeds.
inline void AddSegmentInfo(const void* base, size_t size,
                           const nds_segment_infos_t& infos) {
    std::lock_guard<std::mutex> lk(Mutex());
    Segments().push_back({reinterpret_cast<uintptr_t>(base), size, infos});
}

// Called when a buffer is deregistered so stale entries are not reused.
inline void RemoveSegmentInfo(const void* base) {
    std::lock_guard<std::mutex> lk(Mutex());
    auto& segments = Segments();
    const uintptr_t target = reinterpret_cast<uintptr_t>(base);
    for (auto it = segments.begin(); it != segments.end(); ++it) {
        if (it->base == target) {
            segments.erase(it);
            return;
        }
    }
}

// Cheap in-process lookup. addr may be any address inside a registered buffer.
// Returns false when no registered buffer contains addr.
inline bool GetSegmentInfo(const void* addr, nds_segment_infos_t* out) {
    if (addr == nullptr || out == nullptr) {
        return false;
    }
    const uintptr_t target = reinterpret_cast<uintptr_t>(addr);
    std::lock_guard<std::mutex> lk(Mutex());
    for (const auto& segment : Segments()) {
        if (target >= segment.base && target < segment.base + segment.size) {
            *out = segment.infos;
            return true;
        }
    }
    return false;
}

}  // namespace nds_cache
}  // namespace mooncake

#endif  // USE_NDS
