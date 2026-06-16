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

#include <cuda_runtime.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sys/time.h>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>

#include "transfer_engine.h"
#include "transport/transport.h"
#include "common.h"

using namespace mooncake;

namespace mooncake {
using TransferRequest = Transport::TransferRequest;
using TransferStatus = Transport::TransferStatus;

DEFINE_string(local_server_name, getHostname(),
              "Local server name for segment discovery");
DEFINE_string(metadata_server, "127.0.0.1:2379",
              "Metadata backend: P2PHANDSHAKE, etcd, redis, or http (default: 127.0.0.1:2379)");
DEFINE_string(mode, "initiator",
              "Running mode: initiator or target. Initiator node read/write "
              "data blocks from target node");
DEFINE_string(operation, "read", "Operation type: read or write");
DEFINE_string(device_name, "erdma_1", "Device name to use");
DEFINE_string(nic_priority_matrix, "",
              "Path to RDMA NIC priority matrix file (Advanced)");
DEFINE_string(segment_id, "nvmeof/test_nvmeof", "Segment ID to access data");
DEFINE_int32(gpu_device_id, 0, "GPU device ID to use for GDS testing");

static void *allocateGPUMemory(size_t size, int gpu_device_id) {
    void *ptr = nullptr;
    cudaError_t err = cudaSetDevice(gpu_device_id);
    if (err != cudaSuccess) {
        LOG(ERROR) << "cudaSetDevice failed: " << cudaGetErrorString(err);
        return nullptr;
    }
    err = cudaMalloc(&ptr, size);
    if (err != cudaSuccess) {
        LOG(ERROR) << "cudaMalloc failed: " << cudaGetErrorString(err);
        return nullptr;
    }
    return ptr;
}

static void freeGPUMemory(void *addr) {
    if (addr) {
        cudaFree(addr);
    }
}

class NVMeoFGDSTransportTest : public ::testing::Test {
   public:
    std::shared_ptr<mooncake::TransferMetadata> metadata_client;
    void *gpu_addr = nullptr;
    void *host_buffer = nullptr;  // Host buffer for verification
    std::pair<std::string, uint16_t> hostname_port;
    std::unique_ptr<mooncake::TransferEngine> engine;
    const size_t gpu_buffer_size = 1ull << 30;
    Transport *xport;
    std::string nic_priority_matrix;
    void **args;
    mooncake::Transport::SegmentID segment_id;
    std::shared_ptr<TransferMetadata::SegmentDesc> segment_desc;
    uint64_t remote_base;
    int gpu_device_id;

   protected:
    void SetUp() override {
        static int offset = 0;
        google::InitGoogleLogging("NVMeoFGDSTransportTest");
        FLAGS_logtostderr = 1;

        gpu_device_id = FLAGS_gpu_device_id;

        // Check GPU availability
        int device_count = 0;
        cudaError_t err = cudaGetDeviceCount(&device_count);
        if (err != cudaSuccess || device_count == 0) {
            LOG(ERROR) << "No CUDA devices available";
            FAIL() << "No CUDA devices available";
        }
        if (gpu_device_id >= device_count) {
            LOG(ERROR) << "Invalid GPU device ID " << gpu_device_id;
            FAIL() << "Invalid GPU device ID";
        }

        LOG(INFO) << "Using GPU device " << gpu_device_id << " for GDS testing";

        // disable topology auto discovery for testing.
        engine = std::make_unique<TransferEngine>(false);
        hostname_port = parseHostNameWithPort(FLAGS_local_server_name);
        engine->init(FLAGS_metadata_server, FLAGS_local_server_name.c_str(),
                     hostname_port.first.c_str(),
                     hostname_port.second + offset++);
        xport = nullptr;
        args = (void **)malloc(2 * sizeof(void *));
        args[0] = nullptr;
        xport = engine->installTransport("nvmeof", args);
        ASSERT_NE(xport, nullptr);

        // Allocate GPU memory
        gpu_addr = allocateGPUMemory(gpu_buffer_size, gpu_device_id);
        ASSERT_NE(gpu_addr, (void*)0);

        // Allocate host buffer for verification
        ASSERT_EQ(cudaMallocHost(&host_buffer, gpu_buffer_size), cudaSuccess);

        // Register GPU memory with transfer engine
        std::string gpu_location = "gpu:" + std::to_string(gpu_device_id);
        LOG(INFO) << "Registering GPU memory at location: " << gpu_location;
        int rc = engine->registerLocalMemory(gpu_addr, gpu_buffer_size,
                                             gpu_location.c_str());
        ASSERT_EQ(rc, 0);

        segment_id = engine->openSegment(FLAGS_segment_id.c_str());
        segment_desc = engine->getMetadata()->getSegmentDescByID(segment_id);
        remote_base = 0;
    }

    void TearDown() override {
        google::ShutdownGoogleLogging();
        if (engine && gpu_addr) {
            engine->unregisterLocalMemory(gpu_addr);
        }
        if (gpu_addr) {
            freeGPUMemory(gpu_addr);
        }
        if (host_buffer) {
            cudaFreeHost(host_buffer);
        }
    }
};

TEST_F(NVMeoFGDSTransportTest, GDSWriteTest) {
    const size_t kDataLength = 4096000;
    int times = 10;

    while (times--) {
        // Prepare data on host
        std::vector<uint8_t> host_data(kDataLength);
        for (size_t i = 0; i < kDataLength; ++i) {
            host_data[i] = 'a' + (lrand48() % 26);
        }

        // Copy data to GPU memory
        cudaError_t err =
            cudaMemcpy(gpu_addr, host_data.data(), kDataLength,
                       cudaMemcpyHostToDevice);
        ASSERT_EQ(err, cudaSuccess);

        // Write from GPU to NVMe via GDS
        auto batch_id = xport->allocateBatchID(1);
        Status s;
        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)(gpu_addr);
        entry.target_id = segment_id;
        entry.target_offset = remote_base;
        s = xport->submitTransfer(batch_id, {entry});
        ASSERT_TRUE(s.ok());

        // Wait for completion
        bool completed = false;
        TransferStatus status;
        while (!completed) {
            s = xport->getTransferStatus(batch_id, 0, status);
            ASSERT_TRUE(s.ok());
            if (status.s == TransferStatusEnum::COMPLETED) {
                completed = true;
                LOG(INFO) << "GDS write completed, transferred bytes: "
                          << status.transferred_bytes;
            } else if (status.s == TransferStatusEnum::FAILED) {
                LOG(ERROR) << "GDS write FAILED";
                break;
            }
        }
        ASSERT_TRUE(completed);

        s = xport->freeBatchID(batch_id);
        ASSERT_TRUE(s.ok());
    }

    LOG(INFO) << "GDS write test completed successfully";
}

TEST_F(NVMeoFGDSTransportTest, GDSReadWriteTest) {
    const size_t kDataLength = 4194304;  // 4MB, aligned to 1MB
    const int kWriteIterations = 5;
    const int kReadIterations = 5;

    // Write phase
    for (int iter = 0; iter < kWriteIterations; ++iter) {
        // Prepare data on host
        std::vector<uint8_t> host_data(kDataLength);
        for (size_t i = 0; i < kDataLength; ++i) {
            host_data[i] = 'a' + (iter % 26);
        }

        // Copy data to GPU memory
        cudaError_t err =
            cudaMemcpy(gpu_addr, host_data.data(), kDataLength,
                       cudaMemcpyHostToDevice);
        ASSERT_EQ(err, cudaSuccess);

        // Write from GPU to NVMe via GDS
        auto batch_id = xport->allocateBatchID(1);
        Status s;
        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)(gpu_addr);
        entry.target_id = segment_id;
        entry.target_offset = remote_base + iter * kDataLength;
        s = xport->submitTransfer(batch_id, {entry});
        ASSERT_TRUE(s.ok());

        // Wait for completion
        bool completed = false;
        TransferStatus status;
        while (!completed) {
            s = xport->getTransferStatus(batch_id, 0, status);
            ASSERT_TRUE(s.ok());
            if (status.s == TransferStatusEnum::COMPLETED) {
                completed = true;
            } else if (status.s == TransferStatusEnum::FAILED) {
                LOG(ERROR) << "Write FAILED";
                break;
            }
        }
        ASSERT_TRUE(completed);

        s = xport->freeBatchID(batch_id);
        ASSERT_TRUE(s.ok());

        LOG(INFO) << "Write iteration " << iter << " completed at offset "
                  << (remote_base + iter * kDataLength);
    }

    // Read phase
    for (int iter = 0; iter < kReadIterations; ++iter) {
        // Clear GPU buffer
        cudaMemset(gpu_addr, 0, kDataLength);

        // Read from NVMe to GPU via GDS
        auto batch_id = xport->allocateBatchID(1);
        Status s;
        TransferRequest entry;
        entry.opcode = TransferRequest::READ;
        entry.length = kDataLength;
        entry.source = (uint8_t *)(gpu_addr);
        entry.target_id = segment_id;
        entry.target_offset = remote_base + iter * kDataLength;
        s = xport->submitTransfer(batch_id, {entry});
        ASSERT_TRUE(s.ok());

        // Wait for completion
        bool completed = false;
        TransferStatus status;
        while (!completed) {
            s = xport->getTransferStatus(batch_id, 0, status);
            ASSERT_TRUE(s.ok());
            if (status.s == TransferStatusEnum::COMPLETED) {
                completed = true;
                LOG(INFO) << "GDS read completed, transferred bytes: "
                          << status.transferred_bytes;
            } else if (status.s == TransferStatusEnum::FAILED) {
                LOG(ERROR) << "Read FAILED";
                break;
            }
        }
        ASSERT_TRUE(completed);

        s = xport->freeBatchID(batch_id);
        ASSERT_TRUE(s.ok());

        // Copy data from GPU to host for verification
        std::vector<uint8_t> read_back_data(kDataLength);
        cudaError_t err =
            cudaMemcpy(read_back_data.data(), gpu_addr, kDataLength,
                       cudaMemcpyDeviceToHost);
        ASSERT_EQ(err, cudaSuccess);

        // Verify all bytes are the expected characters
        char expected_char = 'a' + (iter % 26);
        for (size_t i = 0; i < kDataLength; ++i) {
            char c = read_back_data[i];
            ASSERT_EQ(c, expected_char) << "Mismatch at offset " << i
                                        << ", expected: " << expected_char
                                        << ", actual: " << c;
        }

        LOG(INFO) << "Read iteration " << iter << " completed, data verified (all "
                  << expected_char << ")";
    }

    LOG(INFO) << "GDS read/write test completed successfully";
}

TEST_F(NVMeoFGDSTransportTest, GDSBatchWrites) {
    // This test validates batch writes using cuFile batch API
    // Due to GDS driver limitations on concurrent batch queries, this test
    // uses sequential waiting instead of parallel status polling
    const size_t kDataLength = 1048576;  // 1MB
    const int kBatchSize = 4;            // Number of writes in one batch
    const int64_t kWaitTimeoutMs = 15000;  // 15 second timeout

    LOG(INFO) << "Starting GDS batch writes test with " << kBatchSize << " operations";

    // Check segment descriptor for buffer sizes
    if (segment_desc && !segment_desc->nvmeof_buffers.empty()) {
        auto &nvmeof_buffers = segment_desc->nvmeof_buffers;
        LOG(INFO) << "Segment has " << nvmeof_buffers.size() << " NVMe-oF buffers";
        for (size_t i = 0; i < nvmeof_buffers.size(); ++i) {
            LOG(INFO) << "  Buffer " << i << ": length=" << nvmeof_buffers[i].length;
            for (auto &path_map : nvmeof_buffers[i].local_path_map) {
                LOG(INFO) << "    Server: " << path_map.first << " -> " << path_map.second;
            }
        }
        uint64_t total_size = 0;
        for (auto &buf : nvmeof_buffers) {
            total_size += buf.length;
        }
        LOG(INFO) << "Total segment size: " << total_size << " bytes";
        LOG(INFO) << "Test requires: " << (kBatchSize * kDataLength) << " bytes";
        if (total_size < (kBatchSize * kDataLength)) {
            LOG(WARNING) << "Segment size is smaller than required test size!";
        }
    }

    // Allocate batch ID for multiple operations
    auto batch_id = xport->allocateBatchID(kBatchSize);

    // Prepare data for each write
    std::vector<std::vector<uint8_t>> host_data(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        host_data[i].resize(kDataLength);
        for (size_t j = 0; j < kDataLength; ++j) {
            host_data[i][j] = 'a' + ((i + j) % 26);
        }
    }

    // Prepare all transfer requests with different GPU and file offsets
    std::vector<TransferRequest> entries;
    for (int i = 0; i < kBatchSize; ++i) {
        size_t gpu_offset = i * kDataLength;
        void *gpu_src = (uint8_t *)gpu_addr + gpu_offset;

        cudaError_t err = cudaMemcpy(gpu_src, host_data[i].data(), kDataLength,
                                     cudaMemcpyHostToDevice);
        ASSERT_EQ(err, cudaSuccess) << "cudaMemcpy failed for request " << i;

        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)gpu_src;
        entry.target_id = segment_id;
        entry.target_offset = remote_base + gpu_offset;

        entries.push_back(entry);

        LOG(INFO) << "Prepared request " << i << ": GPU offset=" << gpu_offset
                  << ", file offset=" << entry.target_offset;
    }

    // Submit all writes in a single batch
    Status s = xport->submitTransfer(batch_id, entries);
    ASSERT_TRUE(s.ok()) << "Batch submit failed";
    LOG(INFO) << "Submitted batch of " << kBatchSize << " writes";

    // Wait for each task individually (workaround for GDS status polling limitation)
    std::vector<Transport::TransferStatus> statuses(kBatchSize);
    bool all_completed = false;
    auto start_time = std::chrono::steady_clock::now();
    int iteration = 0;

    while (!all_completed) {
        all_completed = true;
        iteration++;

        // Check status in serial order (avoids batch query issues)
        for (int i = 0; i < kBatchSize; ++i) {
            if (statuses[i].s != Transport::TransferStatusEnum::COMPLETED) {
                s = xport->getTransferStatus(batch_id, i, statuses[i]);
                ASSERT_TRUE(s.ok()) << "getTransferStatus failed for task " << i;

                // Log status every ~1 second (every 100 iterations)
                if (iteration % 100 == 0) {
                    LOG(INFO) << "Iteration " << iteration << " Task " << i
                              << " status=" << static_cast<int>(statuses[i].s)
                              << ", transferred=" << statuses[i].transferred_bytes;
                }

                if (statuses[i].s == Transport::TransferStatusEnum::FAILED) {
                    FAIL() << "Task " << i << " FAILED, transferred=" << statuses[i].transferred_bytes;
                } else if (statuses[i].s != Transport::TransferStatusEnum::COMPLETED) {
                    all_completed = false;
                } else {
                    LOG(INFO) << "Task " << i << " completed: transferred=" << statuses[i].transferred_bytes;
                }
            }
        }

        // Check timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        if (elapsed > kWaitTimeoutMs) {
            LOG(ERROR) << "Timeout after " << elapsed << " ms";
            for (int i = 0; i < kBatchSize; ++i) {
                LOG(ERROR) << "Task " << i << ": status=" << static_cast<int>(statuses[i].s)
                           << ", transferred=" << statuses[i].transferred_bytes;
            }
            FAIL() << "Batch writes timeout";
        }

        if (!all_completed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Verify all tasks completed successfully
    for (int i = 0; i < kBatchSize; ++i) {
        ASSERT_EQ(statuses[i].s, Transport::TransferStatusEnum::COMPLETED)
            << "Task " << i << " did not complete";
        ASSERT_EQ(statuses[i].transferred_bytes, kDataLength)
            << "Task " << i << " incomplete transfer";
    }

    LOG(INFO) << "GDS batch writes test completed successfully";

    s = xport->freeBatchID(batch_id);
    ASSERT_TRUE(s.ok());
}

}  // namespace mooncake

int main(int argc, char **argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, false);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
// Trigger rebuild
