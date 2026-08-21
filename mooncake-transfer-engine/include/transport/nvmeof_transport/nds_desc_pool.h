// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NDS_DESC_POOL_H_
#define NDS_DESC_POOL_H_

#ifdef USE_NDS

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "transfer_engine.h"
#include "transport/nvmeof_transport/nds.h"

namespace mooncake {

// Wrapper for reusable nds_batch_handle_t.
// nds_batch_io_setup is expensive, so we reuse handles (similar to GDS transport)
struct NdsBatchHandle {
    nds_batch_handle_t handle;
    unsigned max_nr;  // max number of batch entries
};

// Per-batch descriptor with independent params, events and slices.
// Each allocation gets a fresh descriptor to avoid parameter confusion.
struct NdsBatchDesc {
    NdsBatchHandle *batch_handle;  // Pointer to reusable handle from pool
    std::vector<nds_batch_io_params_t> params;
    std::vector<nds_batch_io_events_t> events;
    std::vector<Transport::Slice *> slices;
    // Submit status (atomic for lock-free polling in getTransferStatus)
    std::atomic<bool> batch_submitted{false};
    std::atomic<bool> submit_failed{false};
    // Start of the transfer duration measurement (steady_clock time in ns,
    // set right after nds_batch_io_submit returns successfully). 0 means the
    // batch was never submitted successfully. Atomic because it is written by
    // the submit worker thread and read by getTransferStatus polling threads.
    std::atomic<uint64_t> transfer_start_ns_{0};
    // Set to true once the whole batch is observed completed, so the transfer
    // latency is recorded exactly once per batch.
    std::atomic<bool> completion_recorded{false};
};

class NdsDescPool {
   public:
    explicit NdsDescPool(size_t max_batch_size = 256);
    ~NdsDescPool();

    NdsDescPool(const NdsDescPool &) = delete;
    NdsDescPool &operator=(const NdsDescPool &) = delete;
    NdsDescPool(NdsDescPool &&) = delete;

    // Allocate a new batch descriptor with independent params/events/slices.
    // Returns descriptor index, or -1 on failure
    int allocNdsDesc(size_t batch_size);

    // Add params and associated slice to the descriptor
    int pushParams(int idx, const nds_batch_io_params_t &io_params,
                   Transport::Slice *slice);

    // Submit the batch
    int submitBatch(int idx);

    // Get transfer status for a specific slice.
    // Triggers a full nds_batch_io_get_status poll and returns the event for
    // slice_id. Updates the corresponding Slice's status via markSuccess /
    // markFailed.
    nds_batch_io_events_t getTransferStatus(int idx, int slice_id);

    // Get current number of slices in the descriptor
    int getSliceNum(int idx);

    // Free the descriptor and return handle to pool
    int freeNdsDesc(int idx);

    // Get descriptor by index
    NdsBatchDesc *getDesc(int idx);

    // Record the transfer duration (submit-to-completion) of the batch once,
    // after the caller has determined that the whole batch is finished. The
    // caller (NVMeoFTransport::getTransferStatus) performs the completion
    // judgment using its existing per-task logic; this method only computes
    // and accumulates the latency and is safe to call concurrently.
    void recordTransferCompleted(int idx);

    // Print accumulated submit-to-completion latency statistics (completed
    // batch count, average and max duration) to the log.
    void printTransferStats() const;

   private:
    static const size_t MAX_NR_DESC = 512;  // Max number of descriptors
    size_t max_batch_size_;

    // Object pool for NdsBatchHandle to avoid frequent
    // nds_batch_io_setup/destroy
    std::vector<NdsBatchHandle *> handle_pool_;
    std::mutex handle_pool_lock_;

    // Array of descriptors (nullptr = free slot)
    NdsBatchDesc *descs_[MAX_NR_DESC];
    RWSpinlock mutex_;

    // Duration statistics (count, total and max in microseconds), updated
    // atomically and thus safe to record from multiple threads.
    struct LatencyStats {
        std::atomic<uint64_t> count{0};      // number of samples
        std::atomic<uint64_t> total_us{0};   // sum of all durations in us
        std::atomic<uint64_t> max_us{0};     // max duration in us
        void record(uint64_t us) {
            count.fetch_add(1, std::memory_order_relaxed);
            total_us.fetch_add(us, std::memory_order_relaxed);
            uint64_t cur_max = max_us.load(std::memory_order_relaxed);
            while (cur_max < us &&
                   !max_us.compare_exchange_weak(cur_max, us,
                                                 std::memory_order_relaxed)) {
            }
        }
    };

    // Submit-to-completion latency of a whole batch: recorded exactly once per
    // batch when getTransferStatus observes all slices in a terminal state.
    LatencyStats transfer_latency_;
    // Execution time of the nds_batch_io_submit() call itself.
    LatencyStats submit_call_time_;
};

}  // namespace mooncake

#endif  // USE_NDS

#endif  // NDS_DESC_POOL_H_
