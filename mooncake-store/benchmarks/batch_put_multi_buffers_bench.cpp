// Copyright 2025 Mooncake Authors
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

// Standalone benchmark for RealClient::batch_put_from_multi_buffers().
//
// It only repeatedly calls batch_put_from_multi_buffers(), with no built-in
// timing wrapper, so it is suitable for clean flame-graph profiling with perf.
//
// Usage:
//   ./batch_put_multi_buffers_bench \
//       --master_server=127.0.0.1:50051 \
//       --metadata_server=http://127.0.0.1:8080/metadata \
//       --protocol=tcp \
//       --num_keys=1000 \
//       --addrs_per_key=8 \
//       --addr_size=4096 \
//       --iterations=100

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "real_client.h"

DEFINE_string(master_server, "127.0.0.1:50051", "Master server address");
DEFINE_string(metadata_server, "http://127.0.0.1:8080/metadata",
              "Metadata server URL");
DEFINE_string(protocol, "tcp", "Transfer protocol: tcp|rdma");
DEFINE_string(device_name, "", "Device name for RDMA");
DEFINE_string(local_hostname, "localhost", "Local hostname");
DEFINE_uint64(global_segment_size_mb, 3200,
              "Global segment size in MB");
DEFINE_uint64(local_buffer_size_mb, 512, "Local buffer size in MB");

DEFINE_uint64(num_keys, 1000,
              "Number of keys per batch_put_from_multi_buffers call");
DEFINE_uint64(addrs_per_key, 1, "Number of buffer addresses per key");
DEFINE_uint64(addr_size, 4096, "Size in bytes of each buffer address");
DEFINE_uint64(iterations, 100,
              "Number of repeated batch_put_from_multi_buffers calls");
DEFINE_bool(prefer_same_node, false,
            "Set ReplicateConfig.prefer_alloc_in_same_node=true");

namespace {

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    const size_t total_bytes =
        static_cast<size_t>(FLAGS_num_keys * FLAGS_addrs_per_key *
                            FLAGS_addr_size);
    std::cout << "=== batch_put_from_multi_buffers benchmark ===" << "\n";
    std::cout << "keys=" << FLAGS_num_keys
              << " addrs_per_key=" << FLAGS_addrs_per_key
              << " addr_size=" << FLAGS_addr_size
              << " total_buffer_bytes=" << total_bytes
              << " iterations=" << FLAGS_iterations << "\n";
    std::cout << "protocol=" << FLAGS_protocol
              << " device_name=" << FLAGS_device_name << "\n";

    if (FLAGS_num_keys == 0 || FLAGS_addrs_per_key == 0 ||
        FLAGS_addr_size == 0 || FLAGS_iterations == 0) {
        LOG(ERROR) << "num_keys/addrs_per_key/addr_size/iterations must be > 0";
        return 1;
    }

    auto client = mooncake::RealClient::create();
    if (!client) {
        LOG(ERROR) << "Failed to create RealClient";
        return 1;
    }

    const size_t global_segment_size =
        static_cast<size_t>(FLAGS_global_segment_size_mb) * 1024 * 1024;
    const size_t local_buffer_size =
        static_cast<size_t>(FLAGS_local_buffer_size_mb) * 1024 * 1024;
    int ret = client->setup_real(
        FLAGS_local_hostname, FLAGS_metadata_server, global_segment_size,
        local_buffer_size, FLAGS_protocol, FLAGS_device_name,
        FLAGS_master_server);
    if (ret != 0) {
        LOG(ERROR) << "setup_real failed, retcode=" << ret;
        return 1;
    }

    // Allocate and register one contiguous buffer.
    void* buffer = nullptr;
    if (posix_memalign(&buffer, 4096, total_bytes) != 0) {
        LOG(ERROR) << "Failed to allocate aligned buffer of "
                   << total_bytes << " bytes";
        return 1;
    }
    std::memset(buffer, 0xAB, total_bytes);

    ret = client->register_buffer(buffer, total_bytes);
    if (ret != 0) {
        LOG(ERROR) << "register_buffer failed, retcode=" << ret;
        std::free(buffer);
        return 1;
    }

    // Pre-build all argument lists before the profiling loop. Each iteration
    // uses a unique key set so repeated Put calls do not hit
    // OBJECT_ALREADY_EXISTS.
    const uint64_t warmup = std::min<uint64_t>(3, std::max<uint64_t>(1, FLAGS_iterations / 10));
    const uint64_t total_rounds = FLAGS_iterations + warmup;
    std::vector<std::vector<std::string>> keys_by_iteration(total_rounds);
    for (uint64_t it = 0; it < total_rounds; ++it) {
        auto& keys = keys_by_iteration[it];
        keys.reserve(FLAGS_num_keys);
        for (uint64_t i = 0; i < FLAGS_num_keys; ++i) {
            keys.emplace_back("bench_multi_buffers_" + std::to_string(it) +
                              "_" + std::to_string(i));
        }
    }

    std::vector<std::vector<void*>> all_buffers(FLAGS_num_keys);
    std::vector<std::vector<size_t>> all_sizes(FLAGS_num_keys);
    char* base = static_cast<char*>(buffer);
    for (uint64_t i = 0; i < FLAGS_num_keys; ++i) {
        auto& ptrs = all_buffers[i];
        auto& sizes = all_sizes[i];
        ptrs.reserve(FLAGS_addrs_per_key);
        sizes.reserve(FLAGS_addrs_per_key);
        for (uint64_t j = 0; j < FLAGS_addrs_per_key; ++j) {
            const size_t offset =
                (i * FLAGS_addrs_per_key + j) * FLAGS_addr_size;
            ptrs.emplace_back(base + offset);
            sizes.emplace_back(FLAGS_addr_size);
        }
    }

    mooncake::ReplicateConfig config;
    config.replica_num = 1;
    config.prefer_alloc_in_same_node = FLAGS_prefer_same_node;

    // Warmup.
    for (uint64_t it = 0; it < warmup; ++it) {
        auto results = client->batch_put_from_multi_buffers(
            keys_by_iteration[it], all_buffers, all_sizes, config);
        for (size_t i = 0; i < results.size(); ++i) {
            if (results[i] != 0) {
                LOG(ERROR) << "Warmup failed: key=" << keys_by_iteration[it][i]
                           << " ret=" << results[i];
                client->unregister_buffer(buffer);
                std::free(buffer);
                return 1;
            }
        }
    }

    // Repeatedly call the target API. No timing is added here; use perf or
    // py-spy externally to measure/profile the call itself.
    for (uint64_t it = 0; it < FLAGS_iterations; ++it) {
        auto results = client->batch_put_from_multi_buffers(
            keys_by_iteration[warmup + it], all_buffers, all_sizes, config);
        for (size_t i = 0; i < results.size(); ++i) {
            if (results[i] != 0) {
                LOG(ERROR) << "Iteration failed: key="
                           << keys_by_iteration[warmup + it][i]
                           << " ret=" << results[i];
                client->unregister_buffer(buffer);
                std::free(buffer);
                return 1;
            }
        }
    }

    client->unregister_buffer(buffer);
    std::free(buffer);
    std::cout << "batch_put_from_multi_buffers benchmark finished\n";

    google::ShutdownGoogleLogging();
    return 0;
}
