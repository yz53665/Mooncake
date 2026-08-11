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

#include "transport/nvmeof_transport/nvmeof_transport.h"

#include <bits/stdint-uintn.h>
#include <glog/logging.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <tuple>

#include "common.h"
#include "transfer_engine.h"
#include "transfer_metadata.h"
#ifndef USE_NDS
#include "transport/nvmeof_transport/cufile_context.h"
#include "transport/nvmeof_transport/cufile_desc_pool.h"
#endif
#include "transport/transport.h"

#ifdef USE_NDS
#include "transport/nvmeof_transport/nds.h"
#include "transport/nvmeof_transport/nds_desc_pool.h"
#include "acl/acl.h"
#endif

namespace mooncake {
NVMeoFTransport::NVMeoFTransport() {
#ifdef USE_NDS
    int32_t dev_id = -1;
    if (aclrtGetDevice(&dev_id) == ACL_SUCCESS && dev_id >= 0) {
        nds_device_id_ = dev_id;
        LOG(INFO) << "NVMeoFTransport: NDS device_id=" << nds_device_id_
                  << " obtained from aclrtGetDevice";
    } else {
        const char *nds_device_env = getenv("MC_NDS_DEVICE_ID");
        if (nds_device_env) {
            auto opt = parseFromString<int32_t>(nds_device_env);
            if (opt.has_value() && opt.value() >= 0) {
                nds_device_id_ = opt.value();
                LOG(INFO) << "NVMeoFTransport: NDS device_id set to "
                          << nds_device_id_
                          << " from MC_NDS_DEVICE_ID (fallback)";
            }
        }
    }
    nds_desc_pool_ = std::make_shared<NdsDescPool>();
#else
    CUFILE_CHECK(cuFileDriverOpen());
    desc_pool_ = std::make_shared<CUFileDescPool>();
#endif
}

NVMeoFTransport::~NVMeoFTransport() {
#ifdef USE_NDS
    stopNdsThreadPool();
    if (nds_initialized_ && nds_device_id_ >= 0) {
        nds_deinit(nds_device_id_);
        nds_initialized_ = false;
    }
#endif
}

#ifndef USE_NDS
Transport::TransferStatusEnum from_cufile_transfer_status(
    CUfileStatus_t status) {
    switch (status) {
        case CUFILE_WAITING:
            return Transport::WAITING;
        case CUFILE_PENDING:
            return Transport::PENDING;
        case CUFILE_INVALID:
            return Transport::INVALID;
        case CUFILE_CANCELED:
            return Transport::CANCELED;
        case CUFILE_COMPLETE:
            return Transport::COMPLETED;
        case CUFILE_TIMEOUT:
            return Transport::TIMEOUT;
        case CUFILE_FAILED:
            return Transport::FAILED;
        default:
            return Transport::FAILED;
    }
}
#endif

NVMeoFTransport::BatchID NVMeoFTransport::allocateBatchID(size_t batch_size) {
    auto nvmeof_desc = new NVMeoFBatchDesc();
    auto batch_id = Transport::allocateBatchID(batch_size);
    auto &batch_desc = *((BatchDesc *)(batch_id));
#ifdef USE_NDS
    nvmeof_desc->desc_idx_ = nds_desc_pool_->allocNdsDesc(batch_size);
    nvmeof_desc->task_to_slices.reserve(batch_size);
#else
    nvmeof_desc->desc_idx_ = desc_pool_->allocCUfileDesc(batch_size);
    nvmeof_desc->transfer_status.reserve(batch_size);
    nvmeof_desc->task_to_slices.reserve(batch_size);
#endif
    batch_desc.context = nvmeof_desc;
    return batch_id;
}

Status NVMeoFTransport::getTransferStatus(BatchID batch_id, size_t task_id,
                                          TransferStatus &status) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    auto &task = batch_desc.task_list[task_id];

#ifdef USE_NDS
    auto &nvmeof_desc = *((NVMeoFBatchDesc *)(batch_desc.context));

    TransferStatus transfer_status = {.s = Transport::PENDING,
                                      .transferred_bytes = 0};
    auto [slice_id, slice_num] = nvmeof_desc.task_to_slices[task_id];
    for (size_t i = slice_id; i < slice_id + slice_num; ++i) {
        auto event = nds_desc_pool_->getTransferStatus(
            nvmeof_desc.desc_idx_, static_cast<int>(i));
        auto *slice =
            nds_desc_pool_->getDesc(nvmeof_desc.desc_idx_)->slices[i];
        if (slice && slice->status != Slice::SUCCESS &&
            slice->status != Slice::FAILED) {
            if (event.status == NDS_BATCH_IO_COMPLETED) {
                slice->markSuccess();
            } else if (event.status == NDS_BATCH_IO_FAILED) {
                LOG(ERROR) << "NVMeoFTransport: NDS slice " << i
                           << " failed, task_id=" << task_id
                           << ", error=" << event.error
                           << ", ret=" << event.ret;
                slice->markFailed();
            }
        }

        if (event.status == NDS_BATCH_IO_COMPLETED) {
            transfer_status.s = Transport::COMPLETED;
            transfer_status.transferred_bytes += event.ret;
        } else if (event.status == NDS_BATCH_IO_FAILED) {
            transfer_status.s = Transport::FAILED;
            break;
        } else {
            transfer_status.s = Transport::WAITING;
            break;
        }
    }
    if (transfer_status.s == COMPLETED || transfer_status.s == FAILED) {
        task.is_finished = true;
    }
    status = transfer_status;
    return Status::OK();
#else
    auto &nvmeof_desc = *((NVMeoFBatchDesc *)(batch_desc.context));

    TransferStatus transfer_status = {.s = Transport::PENDING,
                                      .transferred_bytes = 0};
    auto [slice_id, slice_num] = nvmeof_desc.task_to_slices[task_id];
    for (size_t i = slice_id; i < slice_id + slice_num; ++i) {
        auto event =
            desc_pool_->getTransferStatus(nvmeof_desc.desc_idx_, i);
        transfer_status.s = from_cufile_transfer_status(event.status);
        if (transfer_status.s == COMPLETED) {
            transfer_status.transferred_bytes += event.ret;
        } else {
            break;
        }
    }
    if (transfer_status.s == COMPLETED) {
        task.is_finished = true;
    }
    status = transfer_status;
    return Status::OK();
#endif
}

Status NVMeoFTransport::submitTransferTask(
    const std::vector<TransferTask *> &task_list) {
    if (task_list.empty()) {
        return Status::OK();
    }

    auto &batch_desc = toBatchDesc(task_list[0]->batch_id);

#ifdef USE_NDS
    auto &nvmeof_desc = *((NVMeoFBatchDesc *)(batch_desc.context));

    size_t slice_id = nds_desc_pool_->getSliceNum(nvmeof_desc.desc_idx_);

    std::unordered_map<SegmentID, std::shared_ptr<SegmentDesc>>
        segment_desc_map;

    for (size_t index = 0; index < task_list.size(); ++index) {
        auto &task = *task_list[index];
        auto &request = *task.request;
        auto target_id = request.target_id;

        if (!segment_desc_map.count(target_id)) {
            segment_desc_map[target_id] =
                metadata_->getSegmentDescByID(target_id);
            if (!segment_desc_map[target_id]) {
                LOG(ERROR)
                    << "NVMeoFTransport: Cannot find segment desc for "
                       "target_id="
                    << target_id;
                return Status::InvalidArgument(
                    "NVMeoFTransport: Cannot find segment desc for "
                    "target_id: " +
                    std::to_string(target_id));
            }
        }

        auto &desc = segment_desc_map.at(target_id);
        if (desc->protocol != "nvmeof") {
            LOG(ERROR) << "NVMeoFTransport: Segment protocol mismatch, "
                          "expected nvmeof, got "
                       << desc->protocol;
            return Status::InvalidArgument(
                "NVMeoFTransport: Segment protocol mismatch");
        }

        uint32_t buffer_id = 0;
        uint64_t segment_start = request.target_offset;
        uint64_t segment_end = request.target_offset + request.length;
        uint64_t current_offset = 0;
        for (auto &buffer_desc : desc->nvmeof_buffers) {
            bool is_overlap = overlap(
                (void *)segment_start, request.length,
                (void *)current_offset, buffer_desc.length);
            if (is_overlap) {
                uint64_t slice_start =
                    std::max(segment_start, current_offset);
                uint64_t slice_end =
                    std::min(segment_end,
                             current_offset + buffer_desc.length);
                const char *file_path =
                    buffer_desc.local_path_map[local_server_name_].c_str();
                void *source_addr =
                    (char *)request.source + slice_start - segment_start;
                uint64_t file_offset = slice_start - current_offset;
                uint64_t slice_len = slice_end - slice_start;

                addSliceToTask(source_addr, slice_len, file_offset,
                               request.opcode, task, file_path);

                auto buf_key = std::make_pair(target_id, buffer_id);
                nds_Handle nds_handle = nullptr;
                {
                    RWSpinlock::WriteGuard guard(nds_context_lock_);
                    if (!nds_segment_to_context_.count(buf_key)) {
                        nds_segment_to_context_[buf_key] =
                            std::make_shared<NdsFileContext>(
                                file_path, nds_device_id_);
                    }
                    nds_handle = nds_segment_to_context_.at(buf_key)->getHandle();
                }

                Slice *slice = task.slice_list.back();
                if (!nds_handle) {
                    LOG(ERROR)
                        << "NVMeoFTransport: Invalid nds_handle for buf_key";
                    slice->markFailed();
                    continue;
                }

                addSliceToNdsBatch(source_addr, file_offset, slice_len,
                                   nvmeof_desc.desc_idx_, request.opcode,
                                   nds_handle, slice);
            }
            ++buffer_id;
            current_offset += buffer_desc.length;
        }

        nvmeof_desc.task_to_slices.push_back({slice_id, task.slice_count});
        slice_id += task.slice_count;
    }

    // Asynchronously submit the batch via the NDS worker thread pool.
    // Each worker calls nds_batch_io_submit and moves on to the next batch
    // without waiting for getTransferStatus.
    {
        int desc_idx = nvmeof_desc.desc_idx_;
        std::unique_lock<std::mutex> lock(nds_queue_mutex_);
        if (!nds_running_) {
            // Fallback: submit synchronously if thread pool is not running
            nds_desc_pool_->submitBatch(desc_idx);
        } else {
            nds_task_queue_.emplace([this, desc_idx]() {
                nds_desc_pool_->submitBatch(desc_idx);
            });
            nds_queue_cv_.notify_one();
        }
    }
    return Status::OK();
#else
    auto &nvmeof_desc = *((NVMeoFBatchDesc *)(batch_desc.context));

    size_t slice_id = desc_pool_->getSliceNum(nvmeof_desc.desc_idx_);

    std::unordered_map<SegmentID, std::shared_ptr<SegmentDesc>>
        segment_desc_map;

    for (size_t index = 0; index < task_list.size(); ++index) {
        assert(task_list[index]);
        auto &task = *task_list[index];
        assert(task.request);
        auto &request = *task.request;
        auto target_id = request.target_id;

        if (!segment_desc_map.count(target_id)) {
            segment_desc_map[target_id] =
                metadata_->getSegmentDescByID(target_id);
            if (!segment_desc_map[target_id]) {
                LOG(ERROR) << "NVMeoFTransport: Cannot find segment desc for "
                              "target_id="
                           << target_id;
                return Status::InvalidArgument(
                    "NVMeoFTransport: Cannot find segment desc for target_id: "
                    + std::to_string(target_id));
            }
        }

        auto &desc = segment_desc_map.at(target_id);
        if (desc->protocol != "nvmeof") {
            LOG(ERROR) << "NVMeoFTransport: Segment protocol mismatch, "
                          "expected nvmeof, got "
                       << desc->protocol;
            return Status::InvalidArgument(
                "NVMeoFTransport: Segment protocol mismatch");
        }

        uint32_t buffer_id = 0;
        uint64_t segment_start = request.target_offset;
        uint64_t segment_end = request.target_offset + request.length;
        uint64_t current_offset = 0;
        for (auto &buffer_desc : desc->nvmeof_buffers) {
            bool is_overlap = overlap(
                (void *)segment_start, request.length, (void *)current_offset,
                buffer_desc.length);
            if (is_overlap) {
                uint64_t slice_start = std::max(segment_start, current_offset);
                uint64_t slice_end =
                    std::min(segment_end, current_offset + buffer_desc.length);
                const char *file_path =
                    buffer_desc.local_path_map[local_server_name_].c_str();
                void *source_addr =
                    (char *)request.source + slice_start - segment_start;
                uint64_t file_offset = slice_start - current_offset;
                uint64_t slice_len = slice_end - slice_start;
                addSliceToTask(source_addr, slice_len, file_offset,
                               request.opcode, task, file_path);
                auto buf_key = std::make_pair(target_id, buffer_id);
                CUfileHandle_t fh;
                {
                    RWSpinlock::WriteGuard guard(context_lock_);
                    if (!segment_to_context_.count(buf_key)) {
                        segment_to_context_[buf_key] =
                            std::make_shared<CuFileContext>(file_path);
                    }
                    fh = segment_to_context_.at(buf_key)->getHandle();
                }
                addSliceToCUFileBatch(source_addr, file_offset, slice_len,
                                      nvmeof_desc.desc_idx_, request.opcode,
                                      fh);
            }
            ++buffer_id;
            current_offset += buffer_desc.length;
        }

        nvmeof_desc.transfer_status.push_back(
            TransferStatus{.s = PENDING, .transferred_bytes = 0});
        nvmeof_desc.task_to_slices.push_back({slice_id, task.slice_count});
        slice_id += task.slice_count;
    }

    desc_pool_->submitBatch(nvmeof_desc.desc_idx_);
    return Status::OK();
#endif
}

Status NVMeoFTransport::submitTransfer(
    BatchID batch_id, const std::vector<TransferRequest> &entries) {
    auto &batch_desc = *((BatchDesc *)(batch_id));

    if (batch_desc.task_list.size() + entries.size() > batch_desc.batch_size) {
        LOG(ERROR)
            << "NVMeoFTransport: Exceed the limitation of current batch's "
               "capacity";
        return Status::InvalidArgument(
            "NVMeoFTransport: Exceed the limitation of capacity, batch id: " +
            std::to_string(batch_id));
    }

    size_t task_id = batch_desc.task_list.size();
    batch_desc.task_list.resize(task_id + entries.size());
    std::vector<TransferTask *> task_list;
    for (size_t i = 0; i < entries.size(); ++i) {
        auto &task = batch_desc.task_list[task_id + i];
        task.request = &entries[i];
        task.batch_id = batch_id;
        task_list.push_back(&task);
    }
    return submitTransferTask(task_list);
}

Status NVMeoFTransport::freeBatchID(BatchID batch_id) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    auto &nvmeof_desc = *((NVMeoFBatchDesc *)(batch_desc.context));
    int desc_idx = nvmeof_desc.desc_idx_;
    Status rc = Transport::freeBatchID(batch_id);
    if (rc != Status::OK()) {
        return rc;
    }
#ifdef USE_NDS
    nds_desc_pool_->freeNdsDesc(desc_idx);
#else
    desc_pool_->freeCUfileDesc(desc_idx);
#endif
    return Status::OK();
}

int NVMeoFTransport::install(std::string &local_server_name,
                             std::shared_ptr<TransferMetadata> meta,
                             std::shared_ptr<Topology> topo) {
    return Transport::install(local_server_name, meta, topo);
}

int NVMeoFTransport::registerLocalMemory(void *addr, size_t length,
                                         const std::string &location,
                                         bool remote_accessible,
                                         bool update_metadata) {
    (void)remote_accessible;
    (void)update_metadata;
#ifdef USE_NDS
    if (nds_device_id_ >= 0) {
        if (!nds_initialized_) {
            if (nds_init(nds_device_id_) != 0) {
                LOG(ERROR) << "NVMeoFTransport: nds_init failed for device_id="
                           << nds_device_id_;
                return -1;
            }
            int ret = aclrtSetDevice(nds_device_id_);
            if (ret != ACL_SUCCESS) {
                LOG(ERROR) << "NVMeoFTransport: aclrtSetDevice failed for "
                              "device_id="
                           << nds_device_id_ << ", ret=" << ret;
                return -1;
            }
            nds_initialized_ = true;
            initializeNdsThreadPool();
        }
        if (nds_buf_register(nds_device_id_, addr, length) != 0) {
            LOG(ERROR) << "NVMeoFTransport: nds_buf_register failed for addr="
                       << addr << " length=" << length;
            return -1;
        }
        LOG(INFO) << "NVMeoFTransport: nds_buf_register success for addr="
                  << addr << " length=" << length;
    }
#else
    CUFILE_CHECK(cuFileBufRegister(addr, length, 0));
#endif
    return 0;
}

int NVMeoFTransport::unregisterLocalMemory(void *addr, bool update_metadata) {
    (void)update_metadata;
#ifdef USE_NDS
    if (nds_initialized_ && nds_device_id_ >= 0) {
        if (nds_buf_deregister(nds_device_id_, addr) != 0) {
            LOG(ERROR)
                << "NVMeoFTransport: nds_buf_deregister failed for addr="
                << addr;
            return -1;
        }
    }
#else
    CUFILE_CHECK(cuFileBufDeregister(addr));
#endif
    return 0;
}

void NVMeoFTransport::addSliceToTask(void *source_addr, uint64_t slice_len,
                                     uint64_t target_start,
                                     TransferRequest::OpCode op,
                                     TransferTask &task,
                                     const char *file_path) {
    if (!source_addr || !file_path) {
        LOG(ERROR) << "Invalid source_addr or file_path";
        return;
    }
    Slice *slice = getSliceCache().allocate();
    slice->source_addr = (char *)source_addr;
    slice->length = slice_len;
    slice->opcode = op;
    slice->nvmeof.file_path = file_path;
    slice->nvmeof.start = target_start;
    slice->task = &task;
    slice->status = Slice::PENDING;
    slice->ts = 0;
    task.slice_list.push_back(slice);
    task.total_bytes += slice->length;
    __sync_fetch_and_add(&task.slice_count, 1);
}

#ifndef USE_NDS
void NVMeoFTransport::addSliceToCUFileBatch(
    void *source_addr, uint64_t file_offset, uint64_t slice_len,
    uint64_t desc_id, TransferRequest::OpCode op, CUfileHandle_t fh) {
    CUfileIOParams_t params;
    params.mode = CUFILE_BATCH;
    params.opcode =
        op == Transport::TransferRequest::READ ? CUFILE_READ : CUFILE_WRITE;
    params.cookie = (void *)0;
    params.u.batch.devPtr_base = source_addr;
    params.u.batch.devPtr_offset = 0;
    params.u.batch.file_offset = file_offset;
    params.u.batch.size = slice_len;
    params.fh = fh;
    desc_pool_->pushParams(desc_id, params);
}
#else
void NVMeoFTransport::addSliceToNdsBatch(
    void *source_addr, uint64_t file_offset, uint64_t slice_len,
    int desc_id, TransferRequest::OpCode op, nds_Handle nds_handle,
    Slice *slice) {
    nds_batch_io_params_t params;
    params.buf = source_addr;
    params.nbyte = slice_len;
    params.offset = file_offset;
    params.nds_handle = nds_handle;
    params.opcode = (op == Transport::TransferRequest::READ)
                        ? NDS_BATCH_IO_READ
                        : NDS_BATCH_IO_WRITE;
    params.cookie = slice;
    params.device_id = nds_device_id_;
    nds_desc_pool_->pushParams(desc_id, params, slice);
}
#endif

#ifdef USE_NDS
void NVMeoFTransport::initializeNdsThreadPool() {
    nds_running_ = true;
    nds_workers_.reserve(nds_thread_pool_size_);
    for (size_t i = 0; i < nds_thread_pool_size_; ++i) {
        nds_workers_.emplace_back(&NVMeoFTransport::ndsWorkerThread, this);
    }
    LOG(INFO) << "NVMeoFTransport: NDS submit thread pool initialized with "
              << nds_thread_pool_size_ << " threads";
}

void NVMeoFTransport::stopNdsThreadPool() {
    bool expected = true;
    if (!nds_running_.compare_exchange_strong(expected, false)) {
        return;
    }
    nds_queue_cv_.notify_all();
    for (auto &worker : nds_workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    nds_workers_.clear();
    while (!nds_task_queue_.empty()) {
        nds_task_queue_.pop();
    }
    LOG(INFO) << "NVMeoFTransport: NDS submit thread pool stopped";
}

void NVMeoFTransport::ndsWorkerThread() {
    // Each worker thread must set the NPU device context for NDS API calls
    int ret = aclrtSetDevice(nds_device_id_);
    if (ret != ACL_SUCCESS) {
        LOG(ERROR) << "NDS worker: aclrtSetDevice failed for device_id="
                   << nds_device_id_ << ", ret=" << ret;
    }
    while (nds_running_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(nds_queue_mutex_);
            nds_queue_cv_.wait(lock, [this] {
                return !nds_running_ || !nds_task_queue_.empty();
            });

            if (!nds_running_ && nds_task_queue_.empty()) {
                return;
            }

            if (!nds_task_queue_.empty()) {
                task = std::move(nds_task_queue_.front());
                nds_task_queue_.pop();
            }
        }

        if (task) {
            try {
                // Execute nds_batch_io_submit for this batch, then immediately
                // loop back to process the next batch without waiting for
                // getTransferStatus.
                task();
            } catch (...) {
                LOG(ERROR) << "NVMeoFTransport: NDS worker thread caught "
                              "unknown exception";
            }
        }
    }
}
#endif

}  // namespace mooncake
