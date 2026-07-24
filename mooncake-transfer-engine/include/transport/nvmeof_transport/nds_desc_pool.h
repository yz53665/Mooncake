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
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "transfer_engine.h"
#include "transport/nvmeof_transport/nds.h"

namespace mooncake {

// Wrapper for reusable ndsBatchHandle_t.
// ndsBatchIOSetUp is expensive, so we reuse handles (similar to GDS transport)
struct NdsBatchHandle {
    ndsBatchHandle_t handle;
    unsigned max_nr;  // max number of batch entries
};

// Per-batch descriptor with independent params, events and slices.
// Each allocation gets a fresh descriptor to avoid parameter confusion.
struct NdsBatchDesc {
    NdsBatchHandle *batch_handle;  // Pointer to reusable handle from pool
    std::vector<ndsBatchIOParams_t> params;
    std::vector<ndsBatchIOEvents_t> events;
    std::vector<Transport::Slice *> slices;
};

class NdsDescPool {
   public:
    explicit NdsDescPool(size_t max_batch_size = 128);
    ~NdsDescPool();

    NdsDescPool(const NdsDescPool &) = delete;
    NdsDescPool &operator=(const NdsDescPool &) = delete;
    NdsDescPool(NdsDescPool &&) = delete;

    // Allocate a new batch descriptor with independent params/events/slices.
    // Returns descriptor index, or -1 on failure
    int allocNdsDesc(size_t batch_size);

    // Add params and associated slice to the descriptor
    int pushParams(int idx, const ndsBatchIOParams_t &io_params,
                   Transport::Slice *slice);

    // Submit the batch
    int submitBatch(int idx);

    // Get transfer status for a specific slice.
    // Triggers a full ndsBatchIOGetStatus poll and returns the event for
    // slice_id. Updates the corresponding Slice's status via markSuccess /
    // markFailed.
    ndsBatchIOEvents_t getTransferStatus(int idx, int slice_id);

    // Get current number of slices in the descriptor
    int getSliceNum(int idx);

    // Free the descriptor and return handle to pool
    int freeNdsDesc(int idx);

    // Get descriptor by index
    NdsBatchDesc *getDesc(int idx);

   private:
    static const size_t MAX_NR_DESC = 256;  // Max number of descriptors
    size_t max_batch_size_;

    // Object pool for NdsBatchHandle to avoid frequent
    // ndsBatchIOSetUp/Destroy
    std::vector<NdsBatchHandle *> handle_pool_;
    std::mutex handle_pool_lock_;

    // Array of descriptors (nullptr = free slot)
    NdsBatchDesc *descs_[MAX_NR_DESC];
    RWSpinlock mutex_;
};

}  // namespace mooncake

#endif  // USE_NDS

#endif  // NDS_DESC_POOL_H_
