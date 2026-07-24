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

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <cstring>

#include "acl/acl.h"
#include "common.h"
#include "transfer_engine.h"
#include "transport/transport.h"

using namespace mooncake;

namespace mooncake {

DEFINE_string(local_server_name, getHostname(),
              "Local server name for segment discovery");
DEFINE_string(metadata_server, "127.0.0.1:2379", "etcd server host address");
DEFINE_string(mode, "initiator",
              "Running mode: initiator or target. Initiator node read/write "
              "data blocks from target node");
DEFINE_string(operation, "read", "Operation type: read or write");

DEFINE_string(segment_id, "nvmeof/test_nvmeof", "Segment ID to access data");
DEFINE_int32(nds_device_id, 0, "NPU device ID for NDS");

static void *allocateNpuMemory(size_t size, int32_t device_id) {
    void *buf = nullptr;
    int ret = aclrtMalloc(&buf, size, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != ACL_SUCCESS || buf == nullptr) {
        LOG(ERROR) << "Failed to allocate NPU HBM memory, ret=" << ret << ", size=" << size;
        return nullptr;
    }
    LOG(INFO) << "Allocated NPU HBM memory at " << buf << " size=" << size;
    return buf;
}

static void *allocateHostMemory(size_t size) {
    void *buf = nullptr;
    int ret = aclrtMallocHost(&buf, size);
    if (ret != ACL_SUCCESS || buf == nullptr) {
        LOG(ERROR) << "Failed to allocate host memory, ret=" << ret << ", size=" << size;
        return nullptr;
    }
    LOG(INFO) << "Allocated host memory at " << buf << " size=" << size;
    return buf;
}

static void freeNpuMemory(void *addr) {
    if (addr) {
        aclrtFree(addr);
    }
}

static void freeHostMemory(void *addr) {
    if (addr) {
        aclrtFreeHost(addr);
    }
}

static int copyToNpu(void *dst, const void *src, size_t size) {
    int ret = aclrtMemcpy(dst, size, src, size, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        LOG(ERROR) << "aclrtMemcpy HOST_TO_DEVICE failed, ret=" << ret;
        return -1;
    }
    return 0;
}

static int copyFromNpu(void *dst, const void *src, size_t size) {
    int ret = aclrtMemcpy(dst, size, src, size, ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != ACL_SUCCESS) {
        LOG(ERROR) << "aclrtMemcpy DEVICE_TO_HOST failed, ret=" << ret;
        return -1;
    }
    return 0;
}

class NVMeoFNdsTransportTest : public ::testing::Test {
   public:
    std::shared_ptr<mooncake::TransferMetadata> metadata_client;
    void *npu_addr = nullptr;
    void *host_buffer = nullptr;
    std::pair<std::string, uint16_t> hostname_port;
    std::unique_ptr<mooncake::TransferEngine> engine;
    const size_t ram_buffer_size = 1ull << 30;
    Transport *xport;
    std::shared_ptr<TransferMetadata::SegmentDesc> segment_desc;
    void **args;
    mooncake::Transport::SegmentID segment_id;
    uint64_t remote_base;
    int32_t device_id_;

   protected:
    void SetUp() override {
        static int offset = 0;
        google::InitGoogleLogging("NVMeoFNdsTransportTest");
        FLAGS_logtostderr = 1;

        device_id_ = FLAGS_nds_device_id;

        // Initialize ACL
        int ret = aclInit(nullptr);
        ASSERT_EQ(ret, ACL_SUCCESS) << "aclInit failed, ret=" << ret;

        ret = aclrtSetDevice(device_id_);
        ASSERT_EQ(ret, ACL_SUCCESS) << "Failed to set NPU device " << device_id_ << ", ret=" << ret;

        setenv("MC_NDS_DEVICE_ID", std::to_string(device_id_).c_str(), 1);

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

        // Allocate NPU memory
        npu_addr = allocateNpuMemory(ram_buffer_size, device_id_);
        ASSERT_NE(npu_addr, nullptr);

        // Allocate host buffer for verification
        host_buffer = allocateHostMemory(ram_buffer_size);
        ASSERT_NE(host_buffer, nullptr);

        // Register NPU memory with transfer engine
        std::string npu_location = "npu:" + std::to_string(device_id_);
        LOG(INFO) << "Registering NPU memory at location: " << npu_location;
        int rc = engine->registerLocalMemory(npu_addr, ram_buffer_size, npu_location.c_str());
        ASSERT_EQ(rc, 0);

        segment_id = engine->openSegment(FLAGS_segment_id.c_str());
        bindToSocket(0);
        segment_desc = engine->getMetadata()->getSegmentDescByID(segment_id);
        remote_base = 0;
    }

    void TearDown() override {
        google::ShutdownGoogleLogging();
        if (engine && npu_addr) {
            engine->unregisterLocalMemory(npu_addr);
        }
        if (npu_addr) {
            freeNpuMemory(npu_addr);
        }
        if (host_buffer) {
            freeHostMemory(host_buffer);
        }
        aclrtResetDevice(device_id_);
        aclFinalize();
    }

    void waitForCompletion(BatchID batch_id, size_t task_id = 0) {
        bool completed = false;
        TransferStatus status;
        while (!completed) {
            Status s = xport->getTransferStatus(batch_id, task_id, status);
            ASSERT_EQ(s, Status::OK());
            if (status.s == TransferStatusEnum::COMPLETED ||
                status.s == TransferStatusEnum::FAILED) {
                completed = true;
            }
        }
    }
};

TEST_F(NVMeoFNdsTransportTest, SingleWrite) {
    const size_t kDataLength = 4096000;

    // Prepare data on host
    std::vector<uint8_t> host_data(kDataLength);
    for (size_t i = 0; i < kDataLength; ++i) {
        host_data[i] = 'a' + (lrand48() % 26);
    }

    // Copy data to NPU memory
    ASSERT_EQ(copyToNpu(npu_addr, host_data.data(), kDataLength), 0);

    // Write from NPU to NVMe via NDS
    auto batch_id = xport->allocateBatchID(1);
    TransferRequest entry;
    entry.opcode = TransferRequest::WRITE;
    entry.length = kDataLength;
    entry.source = (uint8_t *)(npu_addr);
    entry.target_id = segment_id;
    entry.target_offset = remote_base;

    Status s = xport->submitTransfer(batch_id, {entry});
    ASSERT_EQ(s, Status::OK());

    waitForCompletion(batch_id);

    TransferStatus status;
    xport->getTransferStatus(batch_id, 0, status);
    EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);

    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());
}

TEST_F(NVMeoFNdsTransportTest, SingleRead) {
    const size_t kDataLength = 4096000;

    // Prepare data on host
    std::vector<uint8_t> host_data(kDataLength);
    for (size_t i = 0; i < kDataLength; ++i) {
        host_data[i] = 'a' + (lrand48() % 26);
    }

    // Copy data to NPU memory
    ASSERT_EQ(copyToNpu(npu_addr, host_data.data(), kDataLength), 0);

    // Write from NPU to NVMe via NDS
    auto batch_id = xport->allocateBatchID(1);
    TransferRequest write_entry;
    write_entry.opcode = TransferRequest::WRITE;
    write_entry.length = kDataLength;
    write_entry.source = (uint8_t *)(npu_addr);
    write_entry.target_id = segment_id;
    write_entry.target_offset = remote_base;

    Status s = xport->submitTransfer(batch_id, {write_entry});
    ASSERT_EQ(s, Status::OK());
    waitForCompletion(batch_id);
    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    // Read from NVMe to NPU via NDS
    batch_id = xport->allocateBatchID(1);
    void *npu_read_addr = (uint8_t *)npu_addr + kDataLength;

    // Print address information
    LOG(INFO) << "=== SingleRead Address Information ===";
    LOG(INFO) << "Original npu_addr:           " << npu_addr << " (0x" << std::hex << npu_addr << std::dec << ")";
    LOG(INFO) << "Calculated npu_read_addr:     " << npu_read_addr << " (0x" << std::hex << npu_read_addr << std::dec << ")";
    LOG(INFO) << "Data length (kDataLength):   " << kDataLength << " bytes (0x" << std::hex << kDataLength << std::dec << ")";
    LOG(INFO) << "Offset from npu_addr:        " << kDataLength << " bytes";

    LOG(INFO) << "Allocated NPU memory region: " << npu_addr << " (size: " << ram_buffer_size << " bytes)";
    LOG(INFO) << "Write data location:         " << npu_addr << " (first " << kDataLength << " bytes)";
    LOG(INFO) << "Read data location:          " << npu_read_addr << " (next " << kDataLength << " bytes)";

    TransferRequest read_entry;
    read_entry.opcode = TransferRequest::READ;
    read_entry.length = kDataLength;
    read_entry.source = (uint8_t *)npu_read_addr;
    read_entry.target_id = segment_id;
    read_entry.target_offset = remote_base;

    s = xport->submitTransfer(batch_id, {read_entry});
    ASSERT_EQ(s, Status::OK());
    waitForCompletion(batch_id);

    TransferStatus status;
    xport->getTransferStatus(batch_id, 0, status);
    EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);

    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    // Copy read data back to host for verification
#ifndef NDS_USE_STUB
    std::vector<uint8_t> read_back_data(kDataLength);

    // Print host buffer address for verification
    LOG(INFO) << "=== CopyFromNpu Operation ===";
    LOG(INFO) << "Destination (host_buffer):      " << static_cast<void*>(read_back_data.data());
    LOG(INFO) << "Source (npu_read_addr):         " << npu_read_addr << " (0x" << std::hex << npu_read_addr << std::dec << ")";
    LOG(INFO) << "Copy size:                      " << kDataLength << " bytes";
    LOG(INFO) << "Copy direction:                 NPU -> Host (DEVICE_TO_HOST)";

    ASSERT_EQ(copyFromNpu(read_back_data.data(), npu_read_addr, kDataLength), 0);
    LOG(INFO) << "CopyFromNpu completed successfully";

    // Print first few bytes of original data and read back data for comparison
    LOG(INFO) << "=== SingleRead Data Comparison ===";
    LOG(INFO) << "Data length: " << kDataLength << " bytes";

    // Print first 32 bytes
    LOG(INFO) << "Original host_data (first 32 bytes):";
    std::string host_str, read_str;
    for (int i = 0; i < 32 && i < static_cast<int>(kDataLength); ++i) {
        host_str += std::to_string(static_cast<int>(host_data[i])) + " ";
    }
    LOG(INFO) << host_str;

    LOG(INFO) << "Read back read_back_data (first 32 bytes):";
    for (int i = 0; i < 32 && i < static_cast<int>(kDataLength); ++i) {
        read_str += std::to_string(static_cast<int>(read_back_data[i])) + " ";
    }
    LOG(INFO) << read_str;

    // Find first mismatch and print context
    size_t first_mismatch = 0;
    bool has_mismatch = false;
    for (size_t i = 0; i < kDataLength; ++i) {
        if (host_data[i] != read_back_data[i]) {
            first_mismatch = i;
            has_mismatch = true;
            break;
        }
    }

    if (has_mismatch) {
        LOG(ERROR) << "First mismatch at offset " << first_mismatch;

        // Print context around mismatch (10 bytes before and after)
        size_t start = std::max(static_cast<size_t>(0), first_mismatch - 10);
        size_t end = std::min(kDataLength, first_mismatch + 11);

        LOG(INFO) << "Context around mismatch (host_data):";
        std::string context_str;
        for (size_t i = start; i < end; ++i) {
            std::string prefix = (i == first_mismatch) ? "[MISMATCH]" : "";
            context_str += prefix + std::to_string(static_cast<int>(host_data[i])) + " ";
        }
        LOG(INFO) << context_str;

        LOG(INFO) << "Context around mismatch (read_back_data):";
        context_str.clear();
        for (size_t i = start; i < end; ++i) {
            std::string prefix = (i == first_mismatch) ? "[MISMATCH]" : "";
            context_str += prefix + std::to_string(static_cast<int>(read_back_data[i])) + " ";
        }
        LOG(INFO) << context_str;

        // Count total mismatches
        int mismatch_count = 0;
        for (size_t i = 0; i < kDataLength; ++i) {
            if (host_data[i] != read_back_data[i]) {
                mismatch_count++;
            }
        }
        LOG(ERROR) << "Total mismatches: " << mismatch_count << " out of " << kDataLength << " bytes";
    } else {
        LOG(INFO) << "No mismatches found! Data matches perfectly.";
    }

    int ret = memcmp(host_data.data(), read_back_data.data(), kDataLength);
    EXPECT_EQ(ret, 0);
#endif
    LOG(INFO) << "SingleRead test completed (data verification completed)";
}


TEST_F(NVMeoFNdsTransportTest, MultiWrite) {
    const size_t kDataLength = 4096000;
    int times = 10;
    while (times--) {
        // Prepare data on host
        std::vector<uint8_t> host_data(kDataLength);
        for (size_t i = 0; i < kDataLength; ++i) {
            host_data[i] = 'a' + (lrand48() % 26);
        }

        // Copy data to NPU memory
        ASSERT_EQ(copyToNpu(npu_addr, host_data.data(), kDataLength), 0);

        // Write from NPU to NVMe via NDS
        auto batch_id = xport->allocateBatchID(1);
        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)(npu_addr);
        entry.target_id = segment_id;
        entry.target_offset = remote_base;

        Status s = xport->submitTransfer(batch_id, {entry});
        ASSERT_EQ(s, Status::OK());
        waitForCompletion(batch_id);

        TransferStatus status;
        xport->getTransferStatus(batch_id, 0, status);
        EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);

        s = xport->freeBatchID(batch_id);
        ASSERT_EQ(s, Status::OK());
    }
}

TEST_F(NVMeoFNdsTransportTest, MultipleRead) {
    const size_t kDataLength = 4096000;
    int times = 10;
    while (times--) {
        // Prepare data on host
        std::vector<uint8_t> host_data(kDataLength);
        for (size_t i = 0; i < kDataLength; ++i) {
            host_data[i] = 'a' + (lrand48() % 26);
        }

        // Copy data to NPU memory
        ASSERT_EQ(copyToNpu(npu_addr, host_data.data(), kDataLength), 0);

        // Write from NPU to NVMe via NDS
        auto batch_id = xport->allocateBatchID(1);
        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)(npu_addr);
        entry.target_id = segment_id;
        entry.target_offset = remote_base;

        Status s = xport->submitTransfer(batch_id, {entry});
        ASSERT_EQ(s, Status::OK());
        waitForCompletion(batch_id);
        s = xport->freeBatchID(batch_id);
        ASSERT_EQ(s, Status::OK());
    }

    times = 10;
    void *npu_read_addr = (uint8_t *)npu_addr + kDataLength;
    while (times--) {
        // Read from NVMe to NPU via NDS
        auto batch_id = xport->allocateBatchID(1);
        TransferRequest entry;
        entry.opcode = TransferRequest::READ;
        entry.length = kDataLength;
        entry.source = (uint8_t *)npu_read_addr;
        entry.target_id = segment_id;
        entry.target_offset = remote_base;

        Status s = xport->submitTransfer(batch_id, {entry});
        ASSERT_EQ(s, Status::OK());
        waitForCompletion(batch_id);

        TransferStatus status;
        xport->getTransferStatus(batch_id, 0, status);
        EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);

        s = xport->freeBatchID(batch_id);
        ASSERT_EQ(s, Status::OK());
    }
    LOG(INFO) << "MultipleRead test completed (data verification skipped)";
}

// Test with non-zero remote_base offset
TEST_F(NVMeoFNdsTransportTest, SingleReadWithOffset) {
    const size_t kDataLength = 4096000;
    const uint64_t kRemoteBaseOffset = 2;

    // Prepare data on host
    std::vector<uint8_t> host_data(kDataLength);
    for (size_t i = 0; i < kDataLength; ++i) {
        host_data[i] = 'a' + (lrand48() % 26);
    }

    // Copy data to NPU memory
    ASSERT_EQ(copyToNpu(npu_addr, host_data.data(), kDataLength), 0);

    // Write from NPU to NVMe via NDS with offset
    auto batch_id = xport->allocateBatchID(1);
    TransferRequest write_entry;
    write_entry.opcode = TransferRequest::WRITE;
    write_entry.length = kDataLength;
    write_entry.source = (uint8_t *)(npu_addr);
    write_entry.target_id = segment_id;
    write_entry.target_offset = kRemoteBaseOffset;

    LOG(INFO) << "=== SingleReadWithOffset - Write Phase ===";
    LOG(INFO) << "Writing with remote_base offset: " << kRemoteBaseOffset << " (0x" << std::hex << kRemoteBaseOffset << std::dec << ")";

    Status s = xport->submitTransfer(batch_id, {write_entry});
    ASSERT_EQ(s, Status::OK());
    waitForCompletion(batch_id);
    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    // Read from NVMe to NPU via NDS with same offset
    batch_id = xport->allocateBatchID(1);
    void *npu_read_addr = (uint8_t *)npu_addr + kDataLength;

    LOG(INFO) << "=== SingleReadWithOffset - Read Phase ===";
    LOG(INFO) << "Reading with remote_base offset: " << remote_base << " (0x" << std::hex << remote_base << std::dec << ")";
    LOG(INFO) << "Original npu_addr:        " << npu_addr << " (0x" << std::hex << npu_addr << std::dec << ")";
    LOG(INFO) << "Read dest npu_read_addr:  " << npu_read_addr << " (0x" << std::hex << npu_read_addr << std::dec << ")";

    TransferRequest read_entry;
    read_entry.opcode = TransferRequest::READ;
    read_entry.length = kDataLength;
    read_entry.source = (uint8_t *)npu_read_addr;
    read_entry.target_id = segment_id;
    read_entry.target_offset = kRemoteBaseOffset;

    s = xport->submitTransfer(batch_id, {read_entry});
    ASSERT_EQ(s, Status::OK());
    waitForCompletion(batch_id);

    TransferStatus status;
    xport->getTransferStatus(batch_id, 0, status);
    EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);

    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    // Copy read data back to host for verification
#ifndef NDS_USE_STUB
    std::vector<uint8_t> read_back_data(kDataLength);
    ASSERT_EQ(copyFromNpu(read_back_data.data(), npu_read_addr, kDataLength), 0);

    // Verify data matches
    int ret = memcmp(host_data.data(), read_back_data.data(), kDataLength);
    EXPECT_EQ(ret, 0);

    if (ret != 0) {
        LOG(ERROR) << "=== SingleReadWithOffset - Data Mismatch ===";
        size_t first_mismatch = 0;
        bool has_mismatch = false;
        for (size_t i = 0; i < kDataLength; ++i) {
            if (host_data[i] != read_back_data[i]) {
                first_mismatch = i;
                has_mismatch = true;
                break;
            }
        }

        if (has_mismatch) {
            LOG(ERROR) << "First mismatch at offset " << first_mismatch;
            LOG(ERROR) << "  host_data[" << first_mismatch << "] = " << static_cast<int>(host_data[first_mismatch]);
            LOG(ERROR) << "  read_back_data[" << first_mismatch << "] = " << static_cast<int>(read_back_data[first_mismatch]);

            // Count total mismatches
            int mismatch_count = 0;
            for (size_t i = 0; i < kDataLength; ++i) {
                if (host_data[i] != read_back_data[i]) {
                    mismatch_count++;
                }
            }
            LOG(ERROR) << "Total mismatches: " << mismatch_count << " out of " << kDataLength;
        }
    } else {
        LOG(INFO) << "=== SingleReadWithOffset - SUCCESS ===";
        LOG(INFO) << "Data verified successfully with remote_base offset: " << kRemoteBaseOffset;
    }
#endif
}

TEST_F(NVMeoFNdsTransportTest, BatchWriteAndRead) {
    const size_t kDataLength = 1024 * 1024;
    const size_t kBatchSize = 4;

    // Prepare data for each write
    std::vector<std::vector<uint8_t>> host_data(kBatchSize);
    for (size_t i = 0; i < kBatchSize; ++i) {
        host_data[i].resize(kDataLength);
        for (size_t j = 0; j < kDataLength; ++j) {
            host_data[i][j] = 'a' + (lrand48() % 26);
        }
    }

    // Prepare all transfer requests
    std::vector<TransferRequest> entries;
    for (size_t i = 0; i < kBatchSize; ++i) {
        void *npu_src = (uint8_t *)npu_addr + i * kDataLength;

        // Copy data to NPU memory
        ASSERT_EQ(copyToNpu(npu_src, host_data[i].data(), kDataLength), 0);

        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)npu_src;
        entry.target_id = segment_id;
        entry.target_offset = remote_base + i * kDataLength;

        entries.push_back(entry);
    }

    // Submit batch writes
    auto batch_id = xport->allocateBatchID(kBatchSize);
    Status s = xport->submitTransfer(batch_id, entries);
    ASSERT_EQ(s, Status::OK());

    std::vector<TransferStatus> statuses(kBatchSize);
    for (size_t i = 0; i < kBatchSize; ++i) {
        waitForCompletion(batch_id, i);
        xport->getTransferStatus(batch_id, i, statuses[i]);
        EXPECT_EQ(statuses[i].s, TransferStatusEnum::COMPLETED);
    }

    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    // Read back and verify
    batch_id = xport->allocateBatchID(kBatchSize);
    entries.clear();

    for (size_t i = 0; i < kBatchSize; ++i) {
        void *npu_src = (uint8_t *)npu_addr + (kBatchSize + i) * kDataLength;

        TransferRequest entry;
        entry.opcode = TransferRequest::READ;
        entry.length = kDataLength;
        entry.source = (uint8_t *)npu_src;
        entry.target_id = segment_id;
        entry.target_offset = remote_base + i * kDataLength;

        entries.push_back(entry);
    }

    s = xport->submitTransfer(batch_id, entries);
    ASSERT_EQ(s, Status::OK());

    for (size_t i = 0; i < kBatchSize; ++i) {
        waitForCompletion(batch_id, i);
        xport->getTransferStatus(batch_id, i, statuses[i]);
        EXPECT_EQ(statuses[i].s, TransferStatusEnum::COMPLETED);
    }

    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    // Verify data
#ifndef NDS_USE_STUB
    for (size_t i = 0; i < kBatchSize; ++i) {
        void *npu_src = (uint8_t *)npu_addr + (kBatchSize + i) * kDataLength;
        std::vector<uint8_t> read_back_data(kDataLength);
        ASSERT_EQ(copyFromNpu(read_back_data.data(), npu_src, kDataLength), 0);

        LOG(INFO) << "=== BatchWriteAndRead - Batch " << i << " ===";
        LOG(INFO) << "host_data (first 32 bytes):";
        std::string host_str;
        for (size_t j = 0; j < 32 && j < kDataLength; ++j) {
            host_str += std::to_string(static_cast<int>(host_data[i][j])) + " ";
        }
        LOG(INFO) << host_str;

        LOG(INFO) << "read_back_data (first 32 bytes):";
        std::string read_str;
        for (size_t j = 0; j < 32 && j < kDataLength; ++j) {
            read_str += std::to_string(static_cast<int>(read_back_data[j])) + " ";
        }
        LOG(INFO) << read_str;

        // Find first mismatch
        size_t first_mismatch = kDataLength;
        for (size_t j = 0; j < kDataLength; ++j) {
            if (host_data[i][j] != read_back_data[j]) {
                first_mismatch = j;
                break;
            }
        }

        if (first_mismatch < kDataLength) {
            LOG(INFO) << "First mismatch at byte " << first_mismatch;
            LOG(INFO) << "  host_data[" << first_mismatch << "] = " << static_cast<int>(host_data[i][first_mismatch]);
            LOG(INFO) << "  read_back_data[" << first_mismatch << "] = " << static_cast<int>(read_back_data[first_mismatch]);

            // Print context around mismatch
            size_t start = std::max(static_cast<size_t>(0), first_mismatch - 10);
            size_t end = std::min(kDataLength, first_mismatch + 11);

            LOG(INFO) << "Context around mismatch (host_data):";
            std::string context_str;
            for (size_t j = start; j < end; ++j) {
                std::string prefix = (j == first_mismatch) ? "[MISMATCH]" : "";
                context_str += prefix + std::to_string(static_cast<int>(host_data[i][j])) + " ";
            }
            LOG(INFO) << context_str;

            LOG(INFO) << "Context around mismatch (read_back_data):";
            context_str.clear();
            for (size_t j = start; j < end; ++j) {
                std::string prefix = (j == first_mismatch) ? "[MISMATCH]" : "";
                context_str += prefix + std::to_string(static_cast<int>(read_back_data[j])) + " ";
            }
            LOG(INFO) << context_str;

            // Count total mismatches
            int mismatch_count = 0;
            for (size_t j = 0; j < kDataLength; ++j) {
                if (host_data[i][j] != read_back_data[j]) {
                    mismatch_count++;
                }
            }
            LOG(ERROR) << "Total mismatches: " << mismatch_count << " out of " << kDataLength << " bytes";
        } else {
            LOG(INFO) << "No mismatches found! Data matches perfectly.";
        }

        int ret = memcmp(host_data[i].data(), read_back_data.data(), kDataLength);
        EXPECT_EQ(ret, 0);
    }
#endif
    LOG(INFO) << "BatchWriteAndRead test completed (data verification completed)";
}

}  // namespace mooncake

int main(int argc, char **argv) {
    // Allow gflags to ignore unrecognized flags (like gtest flags)
    gflags::AllowCommandLineReparsing();
    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, false);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
