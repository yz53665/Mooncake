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

#ifndef NVMEOF_TRANSPORT_H_
#define NVMEOF_TRANSPORT_H_

#include <bits/stdint-uintn.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "transfer_metadata.h"
#include "transport/transport.h"

#ifndef USE_NDS
#include "cufile_context.h"
#include "cufile_desc_pool.h"
#endif

#ifdef USE_NDS
#include "nds_context.h"
#include "nds_desc_pool.h"
#endif

namespace mooncake {

struct NVMeoFBatchDesc {
    int desc_idx_;
#ifndef USE_NDS
    std::vector<TransferStatus> transfer_status;
    std::vector<std::tuple<size_t, uint64_t>> task_to_slices;
#else
    std::vector<std::tuple<size_t, uint64_t>> task_to_slices;
#endif
};

class NVMeoFTransport : public Transport {
   public:
    NVMeoFTransport();

    ~NVMeoFTransport();

    BatchID allocateBatchID(size_t batch_size) override;

    Status submitTransferTask(
        const std::vector<TransferTask *> &task_list) override;

    Status submitTransfer(BatchID batch_id,
                          const std::vector<TransferRequest> &entries) override;

    Status getTransferStatus(BatchID batch_id, size_t task_id,
                             TransferStatus &status) override;

    Status freeBatchID(BatchID batch_id) override;

    void addSliceToTask(void *source_addr, uint64_t slice_len,
                        uint64_t target_start, TransferRequest::OpCode op,
                        TransferTask &task, const char *file_path);

#ifndef USE_NDS
    void addSliceToCUFileBatch(void *source_addr, uint64_t file_offset,
                               uint64_t slice_len, uint64_t desc_id,
                               TransferRequest::OpCode op, CUfileHandle_t fh);
#else
    void addSliceToNdsBatch(void *source_addr, uint64_t file_offset,
                            uint64_t slice_len, int desc_id,
                            TransferRequest::OpCode op, nds_Handle nds_handle,
                            Slice *slice);
#endif

   private:
    void startTransfer(Slice *slice);

   private:
    struct pair_hash {
        template <class T1, class T2>
        std::size_t operator()(const std::pair<T1, T2> &pair) const {
            auto hash1 = std::hash<T1>{}(pair.first);
            auto hash2 = std::hash<T2>{}(pair.second);
            return hash1 ^ hash2;
        }
    };

    int install(std::string &local_server_name,
                std::shared_ptr<TransferMetadata> meta,
                std::shared_ptr<Topology> topo) override;

    int registerLocalMemory(void *addr, size_t length,
                            const std::string &location, bool remote_accessible,
                            bool update_metadata) override;

    int unregisterLocalMemory(void *addr,
                              bool update_metadata = false) override;

    int registerLocalMemoryBatch(
        const std::vector<Transport::BufferEntry> &buffer_list,
        const std::string &location) override {
        return 0;
    }

    int unregisterLocalMemoryBatch(
        const std::vector<void *> &addr_list) override {
        return 0;
    }

    const char *getName() const override { return "nvmeof"; }

#ifndef USE_NDS
    std::unordered_map<BatchID, int> batch_to_cufile_desc_;
    std::unordered_map<std::pair<SegmentHandle, uint64_t>,
                       std::shared_ptr<CuFileContext>, pair_hash>
        segment_to_context_;
    std::vector<std::thread> workers_;

    std::shared_ptr<CUFileDescPool> desc_pool_;
    RWSpinlock context_lock_;
#endif

#ifdef USE_NDS
    std::unordered_map<std::pair<SegmentHandle, uint64_t>,
                       std::shared_ptr<NdsFileContext>, pair_hash>
        nds_segment_to_context_;
    RWSpinlock nds_context_lock_;

    std::shared_ptr<NdsDescPool> nds_desc_pool_;

    int32_t nds_device_id_ = -1;
    bool nds_initialized_ = false;

    // NDS submit thread pool: worker threads execute nds_batch_io_submit
    // asynchronously. Each worker submits the batch and moves on to the next
    // without waiting for getTransferStatus.
    void initializeNdsThreadPool();
    void stopNdsThreadPool();
    void ndsWorkerThread();

    std::vector<std::thread> nds_workers_;
    std::queue<std::function<void()>> nds_task_queue_;
    std::mutex nds_queue_mutex_;
    std::condition_variable nds_queue_cv_;
    std::atomic<bool> nds_running_{false};
    size_t nds_thread_pool_size_ = kDefaultNdsThreadPoolSize;
    static constexpr size_t kDefaultNdsThreadPoolSize = 8;
#endif
};
}  // namespace mooncake

#endif
