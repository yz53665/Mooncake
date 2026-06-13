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

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>

#include "acl_plugin.h"
#include "common.h"
#include "runtime_plugin.h"
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
    if (g_rtMalloc(&buf, size, RT_MEMORY_HBM) != 0) {
        LOG(ERROR) << "Failed to allocate NPU HBM memory, size=" << size;
        return nullptr;
    }
    LOG(INFO) << "Allocated NPU HBM memory at " << buf << " size=" << size;
    return buf;
}

static void freeNpuMemory(void *addr, size_t size) {
    if (addr) {
        g_rtFree(addr);
    }
}

class NVMeoFNdsTransportTest : public ::testing::Test {
   public:
    std::shared_ptr<mooncake::TransferMetadata> metadata_client;
    void *addr = nullptr;
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
        ASSERT_EQ(g_aclrtSetDevice(device_id_), 0)
            << "Failed to set NPU device " << device_id_;

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
        addr = allocateNpuMemory(ram_buffer_size, device_id_);
        ASSERT_NE(addr, nullptr);
        int rc = engine->registerLocalMemory(addr, ram_buffer_size, "cpu:0");
        ASSERT_EQ(rc, 0);
        segment_id = engine->openSegment(FLAGS_segment_id.c_str());
        bindToSocket(0);
        segment_desc = engine->getMetadata()->getSegmentDescByID(segment_id);
        remote_base = 0;
    }

    void TearDown() override {
        engine->unregisterLocalMemory(addr);
        freeNpuMemory(addr, ram_buffer_size);
        addr = nullptr;
        g_aclrtResetDevice(device_id_);
        google::ShutdownGoogleLogging();
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

    for (size_t offset = 0; offset < kDataLength; ++offset)
        *((char *)(addr) + offset) = 'a' + lrand48() % 26;

    auto batch_id = xport->allocateBatchID(1);
    TransferRequest entry;
    entry.opcode = TransferRequest::WRITE;
    entry.length = kDataLength;
    entry.source = (uint8_t *)(addr);
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

    for (size_t offset = 0; offset < kDataLength; ++offset)
        *((char *)(addr) + offset) = 'a' + lrand48() % 26;

    auto batch_id = xport->allocateBatchID(1);
    TransferRequest write_entry;
    write_entry.opcode = TransferRequest::WRITE;
    write_entry.length = kDataLength;
    write_entry.source = (uint8_t *)(addr);
    write_entry.target_id = segment_id;
    write_entry.target_offset = remote_base;

    Status s = xport->submitTransfer(batch_id, {write_entry});
    ASSERT_EQ(s, Status::OK());
    waitForCompletion(batch_id);
    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    batch_id = xport->allocateBatchID(1);
    TransferRequest read_entry;
    read_entry.opcode = TransferRequest::READ;
    read_entry.length = kDataLength;
    read_entry.source = (uint8_t *)(addr) + kDataLength;
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

    int ret = memcmp((uint8_t *)(addr), (uint8_t *)(addr) + kDataLength,
                     kDataLength);
    EXPECT_EQ(ret, 0);
}

TEST_F(NVMeoFNdsTransportTest, MultiWrite) {
    const size_t kDataLength = 4096000;
    int times = 10;
    while (times--) {
        for (size_t offset = 0; offset < kDataLength; ++offset)
            *((char *)(addr) + offset) = 'a' + lrand48() % 26;

        auto batch_id = xport->allocateBatchID(1);
        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)(addr);
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
        for (size_t offset = 0; offset < kDataLength; ++offset)
            *((char *)(addr) + offset) = 'a' + lrand48() % 26;

        auto batch_id = xport->allocateBatchID(1);
        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)(addr);
        entry.target_id = segment_id;
        entry.target_offset = remote_base;

        Status s = xport->submitTransfer(batch_id, {entry});
        ASSERT_EQ(s, Status::OK());
        waitForCompletion(batch_id);
        s = xport->freeBatchID(batch_id);
        ASSERT_EQ(s, Status::OK());
    }

    times = 10;
    while (times--) {
        auto batch_id = xport->allocateBatchID(1);
        TransferRequest entry;
        entry.opcode = TransferRequest::READ;
        entry.length = 4096000;
        entry.source = (uint8_t *)(addr) + 4096000;
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

        int ret = memcmp((uint8_t *)(addr), (uint8_t *)(addr) + 4096000,
                         4096000);
        EXPECT_EQ(ret, 0);
    }
}

TEST_F(NVMeoFNdsTransportTest, BatchWriteAndRead) {
    const size_t kDataLength = 1024 * 1024;
    const size_t kBatchSize = 4;

    auto batch_id = xport->allocateBatchID(kBatchSize);
    std::vector<TransferRequest> entries;

    for (size_t i = 0; i < kBatchSize; ++i) {
        char *src = (char *)(addr) + i * kDataLength;
        for (size_t offset = 0; offset < kDataLength; ++offset)
            src[offset] = 'A' + (i % 26);

        TransferRequest entry;
        entry.opcode = TransferRequest::WRITE;
        entry.length = kDataLength;
        entry.source = (uint8_t *)src;
        entry.target_id = segment_id;
        entry.target_offset = remote_base + i * kDataLength;
        entries.push_back(entry);
    }

    Status s = xport->submitTransfer(batch_id, entries);
    ASSERT_EQ(s, Status::OK());

    for (size_t i = 0; i < kBatchSize; ++i) {
        waitForCompletion(batch_id, i);
        TransferStatus status;
        xport->getTransferStatus(batch_id, i, status);
        EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);
    }

    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    batch_id = xport->allocateBatchID(kBatchSize);
    entries.clear();

    for (size_t i = 0; i < kBatchSize; ++i) {
        char *src = (char *)(addr) + (kBatchSize + i) * kDataLength;
        TransferRequest entry;
        entry.opcode = TransferRequest::READ;
        entry.length = kDataLength;
        entry.source = (uint8_t *)src;
        entry.target_id = segment_id;
        entry.target_offset = remote_base + i * kDataLength;
        entries.push_back(entry);
    }

    s = xport->submitTransfer(batch_id, entries);
    ASSERT_EQ(s, Status::OK());

    for (size_t i = 0; i < kBatchSize; ++i) {
        waitForCompletion(batch_id, i);
        TransferStatus status;
        xport->getTransferStatus(batch_id, i, status);
        EXPECT_EQ(status.s, TransferStatusEnum::COMPLETED);
    }

    s = xport->freeBatchID(batch_id);
    ASSERT_EQ(s, Status::OK());

    for (size_t i = 0; i < kBatchSize; ++i) {
        int ret = memcmp((char *)(addr) + i * kDataLength,
                         (char *)(addr) + (kBatchSize + i) * kDataLength,
                         kDataLength);
        EXPECT_EQ(ret, 0);
    }
}

}  // namespace mooncake

int main(int argc, char **argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, false);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
