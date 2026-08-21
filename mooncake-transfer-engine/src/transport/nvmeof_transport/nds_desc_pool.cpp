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

#include "transport/nvmeof_transport/nds_desc_pool.h"

#ifdef USE_NDS

#include <glog/logging.h>

#include <chrono>
#include <cstddef>
#include <mutex>

namespace mooncake {

NdsDescPool::NdsDescPool(size_t max_batch_size) : max_batch_size_(max_batch_size) {
    // Initialize descriptor array
    for (size_t i = 0; i < MAX_NR_DESC; ++i) {
        descs_[i] = nullptr;
    }
}

NdsDescPool::~NdsDescPool() {
    // First, collect and destroy batch_handles from allocated descriptors
    for (size_t i = 0; i < MAX_NR_DESC; ++i) {
        if (descs_[i] != nullptr) {
            nds_batch_io_destroy(descs_[i]->batch_handle->handle);
            delete descs_[i]->batch_handle;
            delete descs_[i];
            descs_[i] = nullptr;
        }
    }

    // Then clean up any remaining handles in the pool
    std::lock_guard<std::mutex> lock(handle_pool_lock_);
    for (auto *batch_handle : handle_pool_) {
        nds_batch_io_destroy(batch_handle->handle);
        delete batch_handle;
    }
    handle_pool_.clear();

    // Dump accumulated latency statistics.
    if (transfer_latency_[0].count.load() > 0 ||
        transfer_latency_[1].count.load() > 0 ||
        submit_call_time_[0].count.load() > 0 ||
        submit_call_time_[1].count.load() > 0) {
        printTransferStats();
    }
}

int NdsDescPool::allocNdsDesc(size_t batch_size) {
    if (batch_size > max_batch_size_) {
        LOG(ERROR) << "Batch Size " << batch_size
                   << " Exceeds Max NDS Batch Size " << max_batch_size_;
        return -1;
    }

    RWSpinlock::WriteGuard guard(mutex_);

    // Find a free slot (nullptr = free)
    int idx = -1;
    for (size_t i = 0; i < MAX_NR_DESC; ++i) {
        if (descs_[i] == nullptr) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        LOG(ERROR) << "No Batch Descriptor Available";
        return -1;
    }

    // Create new descriptor with independent params/events/slices
    auto *desc = new NdsBatchDesc();

    // Get or create NdsBatchHandle from pool (lazy loading)
    NdsBatchHandle *batch_handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle_pool_lock_);
        if (!handle_pool_.empty()) {
            batch_handle = handle_pool_.back();
            handle_pool_.pop_back();
        }
    }

    try {
        // If pool is empty or handle size mismatch, create new handle
        // (expensive operation)
        if (!batch_handle ||
            batch_handle->max_nr != static_cast<unsigned>(max_batch_size_)) {
            // Destroy mismatched handle if exists
            if (batch_handle) {
                nds_batch_io_destroy(batch_handle->handle);
                delete batch_handle;
                batch_handle = nullptr;
            }

            auto new_batch_handle = std::make_unique<NdsBatchHandle>();
            new_batch_handle->max_nr =
                static_cast<unsigned>(max_batch_size_);
            // nds_batch_io_setup is time-costly, so we reuse handles
            if (nds_batch_io_setup(&new_batch_handle->handle,
                                new_batch_handle->max_nr) != 0) {
                LOG(ERROR) << "NdsDescPool: nds_batch_io_setup failed for max_nr="
                           << new_batch_handle->max_nr;
                delete desc;
                return -1;
            }
            batch_handle = new_batch_handle.release();
        }

        desc->batch_handle = batch_handle;
        desc->params.clear();
        desc->params.reserve(max_batch_size_);
        desc->events.resize(max_batch_size_);
        desc->slices.clear();
        desc->slices.reserve(max_batch_size_);
        desc->batch_submitted.store(false);
        desc->submit_failed.store(false);
        for (size_t i = 0; i < max_batch_size_; ++i) {
            desc->events[i].status = NDS_BATCH_IO_WAITING;
            desc->events[i].ret = 0;
        }
        descs_[idx] = desc;
        return idx;
    } catch (...) {
        // Clean up on exception to avoid memory leaks
        delete desc;
        if (batch_handle) {
            nds_batch_io_destroy(batch_handle->handle);
            delete batch_handle;
        }
        throw;  // Re-throw to caller
    }
}

int NdsDescPool::pushParams(int idx, const nds_batch_io_params_t &io_params,
                            Transport::Slice *slice) {
    RWSpinlock::WriteGuard guard(mutex_);
    if (idx < 0 || idx >= (int)MAX_NR_DESC || descs_[idx] == nullptr) {
        LOG(ERROR) << "Invalid descriptor index: " << idx;
        return -1;
    }

    auto *desc = descs_[idx];
    if (desc->params.size() >= desc->params.capacity()) {
        LOG(ERROR) << "Descriptor " << idx << " is full";
        return -1;
    }

    // Take the batch opcode from its first slice; used to split the latency
    // statistics by READ/WRITE.
    if (desc->params.empty()) {
        desc->opcode = io_params.opcode;
    }

    desc->params.push_back(io_params);
    desc->slices.push_back(slice);
    return 0;
}

int NdsDescPool::submitBatch(int idx) {
    RWSpinlock::WriteGuard guard(mutex_);
    if (idx < 0 || idx >= (int)MAX_NR_DESC || descs_[idx] == nullptr) {
        LOG(ERROR) << "Invalid descriptor index: " << idx;
        return -1;
    }

    auto *desc = descs_[idx];
    if (desc->params.empty()) {
        LOG(WARNING) << "Submitting empty batch for descriptor " << idx;
        desc->batch_submitted.store(true);
        return 0;
    }

    // Measure the nds_batch_io_submit() call itself: take a timestamp right
    // before and right after the call (recorded on both success and failure).
    unsigned nr = static_cast<unsigned>(desc->params.size());
    auto submit_start = std::chrono::steady_clock::now();
    int submit_ret = nds_batch_io_submit(desc->batch_handle->handle, nr,
                                         desc->params.data(), 0);
    auto submit_end = std::chrono::steady_clock::now();
    auto submit_elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(submit_end -
                                                              submit_start)
            .count();
    submit_call_time_[static_cast<size_t>(desc->opcode)].record(
        submit_elapsed_us > 0 ? static_cast<uint64_t>(submit_elapsed_us) : 0);
    submit_end = std::chrono::steady_clock::now();
    if (submit_ret != 0) {
        LOG(ERROR) << "NdsDescPool: nds_batch_io_submit failed for " << nr
                   << " slices";
        desc->submit_failed.store(true);
        desc->batch_submitted.store(true);
        return -1;
    }
    desc->batch_submitted.store(true);

    // The transfer duration is measured from right after the submit call
    // returns, so its start point is submit_end. The end point is determined
    // by the caller (NVMeoFTransport::getTransferStatus) when the whole batch
    // is finished, via recordTransferCompleted().
    desc->transfer_start_ns_.store(
        static_cast<uint64_t>(submit_end.time_since_epoch().count()),
        std::memory_order_release);
    return 0;
}

nds_batch_io_events_t NdsDescPool::getTransferStatus(int idx, int slice_id) {
    RWSpinlock::WriteGuard guard(mutex_);
    if (idx < 0 || idx >= (int)MAX_NR_DESC || descs_[idx] == nullptr) {
        LOG(ERROR) << "Invalid descriptor index: " << idx;
        nds_batch_io_events_t event;
        event.status = NDS_BATCH_IO_FAILED;
        event.ret = -1;
        return event;
    }

    auto *desc = descs_[idx];
    if (slice_id < 0 || slice_id >= (int)desc->params.size()) {
        LOG(ERROR) << "Invalid slice_id " << slice_id << " for descriptor "
                   << idx << " (size: " << desc->params.size() << ")";
        nds_batch_io_events_t event;
        event.status = NDS_BATCH_IO_FAILED;
        event.ret = -1;
        return event;
    }

    // If batch has not been submitted yet (async submit in progress),
    // report WAITING to caller.
    if (!desc->batch_submitted.load()) {
        nds_batch_io_events_t event;
        event.status = NDS_BATCH_IO_WAITING;
        event.ret = 0;
        return event;
    }

    // If submit itself failed, report FAILED for all slices.
    if (desc->submit_failed.load()) {
        nds_batch_io_events_t event;
        event.status = NDS_BATCH_IO_FAILED;
        event.ret = -1;
        return event;
    }

    unsigned nr = static_cast<unsigned>(desc->params.size());
    if (nds_batch_io_get_status(desc->batch_handle->handle, 0, &nr,
                            desc->events.data(), nullptr) != 0) {
        LOG(ERROR) << "NdsDescPool: nds_batch_io_get_status failed for desc " << idx;
        nds_batch_io_events_t event;
        event.status = NDS_BATCH_IO_FAILED;
        event.ret = -1;
        return event;
    }

    return desc->events[slice_id];
}

int NdsDescPool::getSliceNum(int idx) {
    RWSpinlock::ReadGuard guard(mutex_);
    if (idx < 0 || idx >= (int)MAX_NR_DESC || descs_[idx] == nullptr) {
        LOG(ERROR) << "Invalid descriptor index: " << idx;
        return -1;
    }

    return descs_[idx]->params.size();
}

int NdsDescPool::freeNdsDesc(int idx) {
    RWSpinlock::WriteGuard guard(mutex_);
    if (idx < 0 || idx >= (int)MAX_NR_DESC || descs_[idx] == nullptr) {
        LOG(ERROR) << "Invalid descriptor index: " << idx;
        return -1;
    }

    auto *desc = descs_[idx];

    // IMPORTANT: Caller should ensure all IOs are completed (via
    // getTransferStatus) before calling freeNdsDesc, as NDS may still
    // access params otherwise. This is critical for the handle pooling
    // optimization - the handle will be immediately reused and could lead to
    // use-after-free bugs if IOs are in-flight.
    //
    // Reset the batch context before returning to pool so the next user
    // gets a clean handle (clears submitted params and completion state).
    // Return the handle to pool for reuse (avoid expensive
    // nds_batch_io_destroy)
    if (nds_batch_io_reset(desc->batch_handle->handle) != 0) {
        LOG(WARNING) << "NdsDescPool: nds_batch_io_reset failed for desc "
                     << idx << ", destroying handle instead of reusing";
        nds_batch_io_destroy(desc->batch_handle->handle);
        delete desc->batch_handle;
    } else {
        std::lock_guard<std::mutex> lock(handle_pool_lock_);
        handle_pool_.push_back(desc->batch_handle);
    }

    // Delete the descriptor (each allocation gets a fresh one)
    delete desc;
    descs_[idx] = nullptr;

    return 0;
}

NdsBatchDesc *NdsDescPool::getDesc(int idx) {
    RWSpinlock::ReadGuard guard(mutex_);
    if (idx < 0 || idx >= (int)MAX_NR_DESC) {
        return nullptr;
    }
    return descs_[idx];
}

void NdsDescPool::recordTransferCompleted(int idx) {
    if (idx < 0 || idx >= (int)MAX_NR_DESC || descs_[idx] == nullptr) {
        LOG(ERROR) << "Invalid descriptor index: " << idx;
        return;
    }
    auto *desc = descs_[idx];
    uint64_t start_ns =
        desc->transfer_start_ns_.load(std::memory_order_acquire);
    if (start_ns == 0) {
        // The batch was never submitted successfully (e.g. submit failed), so
        // there is no transfer duration to record.
        return;
    }
    // Record exactly once per batch, even if multiple polling threads
    // concurrently observe the batch as finished.
    if (desc->completion_recorded.exchange(true)) {
        return;
    }
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    uint64_t dur_ns = now_ns > start_ns ? now_ns - start_ns : 0;
    transfer_latency_[static_cast<size_t>(desc->opcode)].record(dur_ns /
                                                                1000);  // ns -> us
}

void NdsDescPool::printTransferStats() const {
    static const char *kOpName[2] = {"READ", "WRITE"};

    // Submit-to-completion latency of whole batches, per opcode.
    for (int op = 0; op < 2; ++op) {
        uint64_t count = transfer_latency_[op].count.load();
        if (count > 0) {
            double avg_us =
                static_cast<double>(transfer_latency_[op].total_us.load()) /
                static_cast<double>(count);
            LOG(INFO) << "NdsDescPool batch submit-to-completion stats ["
                      << kOpName[op] << "]: batches=" << count
                      << ", avg=" << avg_us << " us (" << avg_us / 1000.0
                      << " ms), max=" << transfer_latency_[op].max_us.load()
                      << " us (" << transfer_latency_[op].max_us.load() / 1000.0
                      << " ms)";
        } else {
            LOG(INFO) << "NdsDescPool batch submit-to-completion stats ["
                      << kOpName[op] << "]: no completed batch recorded";
        }
    }

    // Execution time of the nds_batch_io_submit() call itself, per opcode.
    for (int op = 0; op < 2; ++op) {
        uint64_t count = submit_call_time_[op].count.load();
        if (count > 0) {
            double avg_us =
                static_cast<double>(submit_call_time_[op].total_us.load()) /
                static_cast<double>(count);
            LOG(INFO) << "NdsDescPool nds_batch_io_submit call stats ["
                      << kOpName[op] << "]: calls=" << count
                      << ", avg=" << avg_us << " us (" << avg_us / 1000.0
                      << " ms), max=" << submit_call_time_[op].max_us.load()
                      << " us (" << submit_call_time_[op].max_us.load() / 1000.0
                      << " ms)";
        } else {
            LOG(INFO) << "NdsDescPool nds_batch_io_submit call stats ["
                      << kOpName[op] << "]: no call recorded";
        }
    }
}

}  // namespace mooncake

#endif  // USE_NDS
