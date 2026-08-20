// Benchmark for the hash containers used in the KVSegment hash_key design:
//   1. used_hash_keys_  : per-segment dedup set of uint64_t hash_key
//      (std::unordered_set vs. open-addressing linear-probe set vs.
//       128-bucket sharded_set)
//   2. key->hash_key map: ObjectMetadata-style index
//      (std::unordered_map<string, uint64_t>)
//
// Target extreme scenario: 1e9 entries per segment, up to 16 segments.
// NOTE: std::unordered_set<uint64_t> with 1e9 entries needs ~45-50 GB RAM;
// the open-addressing set needs ~16 GB. Run with --num_elements scaled to
// your machine and extrapolate, or run one segment per process.
//
// Usage examples:
//   ./hash_container_bench --container=open_set --operation=insert --num_elements=100000000
//   ./hash_container_bench --container=sharded_set --operation=insert --num_elements=100000000
//   ./hash_container_bench --container=unordered_map --operation=lookup --num_elements=10000000 --key_len=32
//
// Memory is reported from mallinfo2 (glibc) + explicit container sizing.

#include <gflags/gflags.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

DEFINE_uint64(num_elements, 1000000, "Number of elements to insert");
DEFINE_uint64(num_threads, 1, "Number of worker threads");
DEFINE_string(operation, "insert",
              "insert / lookup / mixed (insert then 50% hit lookup)");
DEFINE_string(container, "open_set",
              "unordered_set | open_set | sharded_set | unordered_map");
DEFINE_uint64(key_len, 32, "String key length for unordered_map mode");
DEFINE_uint64(lookups_per_element, 1,
              "Lookup probes per element for lookup/mixed ops");

namespace {

constexpr uint64_t kEmptySlot = UINT64_MAX;  // sentinel for open set

// ---------------------------------------------------------------------------
// Open-addressing set (linear probing, power-of-2 capacity, load factor 0.5)
// This approximates a memory-lean hash_key dedup set: 8 bytes * 2 * N.
// ---------------------------------------------------------------------------
class OpenSet {
   public:
    explicit OpenSet(size_t capacity) {
        size_t cap = 1;
        while (cap < capacity * 2) cap <<= 1;  // load factor <= 0.5
        slots_.assign(cap, kEmptySlot);
        mask_ = cap - 1;
    }

    bool insert(uint64_t key) {
        size_t idx = key & mask_;
        while (slots_[idx] != kEmptySlot) {
            if (slots_[idx] == key) return false;  // already present
            idx = (idx + 1) & mask_;
        }
        slots_[idx] = key;
        return true;
    }

    bool contains(uint64_t key) const {
        size_t idx = key & mask_;
        while (slots_[idx] != kEmptySlot) {
            if (slots_[idx] == key) return true;
            idx = (idx + 1) & mask_;
        }
        return false;
    }

    size_t byte_size() const { return slots_.size() * sizeof(uint64_t); }

   private:
    std::vector<uint64_t> slots_;
    size_t mask_;
};

// ---------------------------------------------------------------------------
// Sharded set: split a per-segment dedup set into 128 buckets, mirroring the
// way ObjectMetadata is sharded. Random hash_key values are uniformly
// distributed, so taking the low 7 bits is sufficient to pick a bucket.
// Each bucket is a std::unordered_set; this keeps each rehash local to one
// bucket and avoids a single huge hash table per segment.
// ---------------------------------------------------------------------------
class ShardedSet {
   public:
    static constexpr size_t kShardBits = 7;
    static constexpr size_t kShardCount = 1 << kShardBits;

    explicit ShardedSet(size_t capacity) {
        const size_t per_shard = std::max<size_t>(1, capacity / kShardCount);
        for (auto& shard : shards_) {
            shard.reserve(static_cast<size_t>(per_shard * 1.2));
        }
    }

    bool insert(uint64_t key) {
        return shards_[key & (kShardCount - 1)].insert(key).second;
    }

    bool contains(uint64_t key) const {
        return shards_[key & (kShardCount - 1)].count(key) > 0;
    }

   private:
    std::array<std::unordered_set<uint64_t>, kShardCount> shards_;
};

// ---------------------------------------------------------------------------
// Workload generators
// ---------------------------------------------------------------------------
std::vector<uint64_t> GenerateKeys(uint64_t n) {
    std::vector<uint64_t> keys(n);
    std::mt19937_64 rng(42);
    for (auto& k : keys) {
        do {
            k = rng();
        } while (k == kEmptySlot);  // avoid sentinel collision
    }
    return keys;
}

std::vector<std::string> GenerateStringKeys(uint64_t n, size_t key_len) {
    std::vector<std::string> keys(n);
    std::mt19937_64 rng(7);
    for (auto& k : keys) {
        k.resize(key_len);
        for (size_t i = 0; i < key_len; ++i) {
            k[i] = static_cast<char>('a' + (rng() % 26));
        }
    }
    return keys;
}

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

size_t PeakRssBytes() {
#if defined(__GLIBC__)
    struct mallinfo2 mi = mallinfo2();
    return mi.uordblks;  // allocated by glibc malloc (incl. container storage)
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Per-thread micro-benchmark helpers
// ---------------------------------------------------------------------------
struct BenchResult {
    double ops_per_sec{0};
    double avg_us_per_op{0};
    double max_us_per_op{0};
};

template <typename Fn>
BenchResult RunTimed(uint64_t total_ops, Fn&& fn) {
    const int64_t t0 = NowUs();
    double max_us = 0;
    for (uint64_t i = 0; i < total_ops; ++i) {
        const int64_t s = NowUs();
        fn(i);
        const int64_t d = NowUs() - s;
        max_us = std::max(max_us, static_cast<double>(d));
    }
    const int64_t elapsed = NowUs() - t0;
    BenchResult r;
    r.ops_per_sec = total_ops * 1e6 / static_cast<double>(elapsed);
    r.avg_us_per_op = static_cast<double>(elapsed) / total_ops;
    r.max_us_per_op = max_us;
    return r;
}

void ReportOps(const char* label, const BenchResult& r) {
    std::cout << "  " << label << ": ops/s=" << std::fixed
              << static_cast<uint64_t>(r.ops_per_sec)
              << " avg_us/op=" << r.avg_us_per_op
              << " max_us/op=" << r.max_us_per_op << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    gflags::SetUsageMessage(
        "Hash container benchmark for KVSegment used_hash_keys_ / key map");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    const uint64_t n = FLAGS_num_elements;
    const bool is_map = (FLAGS_container == "unordered_map");
    const bool is_open = (FLAGS_container == "open_set");
    const bool is_std = (FLAGS_container == "unordered_set");
    const bool is_sharded = (FLAGS_container == "sharded_set");
    if (!is_map && !is_open && !is_std && !is_sharded) {
        std::cerr << "unknown container: " << FLAGS_container << "\n";
        return 1;
    }

    std::cout << "container=" << FLAGS_container << " op=" << FLAGS_operation
              << " elements=" << n << " threads=" << FLAGS_num_threads
              << " key_len=" << FLAGS_key_len << "\n";

    if (is_map) {
        // ---------------- string key -> hash_key map ----------------
        std::unordered_map<std::string, uint64_t> map;
        map.reserve(static_cast<size_t>(n * 1.2));
        std::vector<std::string> keys = GenerateStringKeys(n, FLAGS_key_len);

        auto ins = RunTimed(n, [&](uint64_t i) { map.emplace(keys[i], i); });

        std::vector<std::string> probe = keys;  // 50% hit: first half is present
        for (uint64_t i = n / 2; i < n; ++i) probe[i] = keys[i] + "x";
        auto lkp = RunTimed(n, [&](uint64_t i) {
            volatile uint64_t v = map.count(probe[i]) > 0 ? 1 : 0;
            (void)v;
        });

        ReportOps("insert", ins);
        ReportOps("lookup(50% hit)", lkp);
        std::cout << "  bytes/entry(approx)=" << (PeakRssBytes() / n) << "\n";
        return 0;
    }

    // ---------------- uint64 set ----------------
    std::vector<uint64_t> keys = GenerateKeys(n);
    std::vector<uint64_t> probe = keys;
    for (uint64_t i = n / 2; i < n; ++i) probe[i] = keys[i] ^ 0xDEADBEEFCAFEF00DULL;

    if (is_sharded) {
        ShardedSet set(n);
        auto ins = RunTimed(n, [&](uint64_t i) { set.insert(keys[i]); });
        auto lkp = RunTimed(n, [&](uint64_t i) {
            volatile bool hit = set.contains(probe[i]);
            (void)hit;
        });
        std::cout << "  sharded_set shards=" << ShardedSet::kShardCount << "\n";
        ReportOps("insert", ins);
        ReportOps("lookup(50% hit)", lkp);
        std::cout << "  bytes/entry(approx)=" << (PeakRssBytes() / n) << "\n";
        return 0;
    }

    if (is_open) {
        OpenSet set(n);
        auto ins = RunTimed(n, [&](uint64_t i) { set.insert(keys[i]); });
        auto lkp = RunTimed(n, [&](uint64_t i) {
            volatile bool hit = set.contains(probe[i]);
            (void)hit;
        });
        std::cout << "  open_set byte_size=" << set.byte_size() << " ("
                  << (set.byte_size() / (1024 * 1024 * 1024)) << " GiB)\n";
        ReportOps("insert", ins);
        ReportOps("lookup(50% hit)", lkp);
        return 0;
    }

    // std::unordered_set
    std::unordered_set<uint64_t> set;
    set.reserve(static_cast<size_t>(n * 1.2));
    auto ins = RunTimed(n, [&](uint64_t i) { set.insert(keys[i]); });
    auto lkp = RunTimed(n, [&](uint64_t i) {
        volatile bool hit = set.count(probe[i]) > 0;
        (void)hit;
    });
    ReportOps("insert", ins);
    ReportOps("lookup(50% hit)", lkp);
    std::cout << "  bytes/entry(approx)=" << (PeakRssBytes() / n) << "\n";
    return 0;
}
