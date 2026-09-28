// ThreeFS NDS batch I/O test (直接调用 3FS USRBIO 接口)
//
// 目的：绕过 Mooncake 高层封装，直接调用 3FS 的 USRBIO 接口
//   (hf3fs_iovcreate / hf3fs_iorcreate / hf3fs_prep_npu_direct_io /
//    hf3fs_submit_ios / hf3fs_wait_for_ios)
// 对一批"key"（每个 key 对应 3FS 挂载点下的一个文件）做 NDS 批量写入与回读校验。
//
// 前置条件：
//   - 已启用 USE_NDS + USE_3FS 编译（nds_init/buf_register 为不带 device_id 的版本）
//   - NPU 设备可用（CANN，acl 接口）
//   - 3FS 已挂载，--mount_point 指向挂载点
//
// NDS 使用前必须完成：
//   1. aclInit + aclrtSetDevice
//   2. nds_init()
//   3. nds_buf_register(hbm, size) 注册 HBM buffer
//   4. nds_get_segment_info 获取 segment 元信息（一次获取，I/O 时复用）
//
// 用法示例：
//   # 8 个 key，每个 4MB，写+读校验
//   ./threefs_nds_batch_test --mount_point=/mnt/3fs --test_dir=/nds_batch_test \
//       --num_keys=8 --chunk_size=4194304
//
//   # 只写（不校验），清理文件
//   ./threefs_nds_batch_test --read_verify=false --cleanup=true
//
// 注意：ReadBackVerify 依赖 WriteBatch 先运行（gtest 默认按定义顺序执行），
//   或读取上次运行写入的文件（pattern 确定，可跨进程复验）。

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef USE_NDS
// 必须先包含 mooncake 版 nds.h（不带 device_id 的签名），再包含 3FS 的
// hf3fs_usrbio.h（其内部 "nds.h" 因同名 NDS_H 保护宏而不会重复定义）。
#include <hf3fs/nds.h>
#include "hf3fs/nds_cache.h"
#include <hf3fs_usrbio.h>
#include "acl/acl.h"
#endif

DEFINE_string(mount_point, "/", "3FS mount point");
DEFINE_string(test_dir, "/nds_batch_test", "test directory under mount point");
DEFINE_uint32(num_keys, 8, "number of keys (one file per key)");
DEFINE_uint32(chunk_size, 4 * 1024 * 1024, "bytes per key");
DEFINE_uint32(device_id, 0, "NPU device id");
DEFINE_uint32(ior_entries, 64, "ior ring entries (clamped to >= num_keys)");
DEFINE_int32(io_depth, 0, "hf3fs ior io_depth (0 = process all ASAP)");
DEFINE_bool(read_verify, true, "read back and verify data after write");
DEFINE_bool(cleanup, false, "delete created test files after the test");

namespace {

#ifdef USE_NDS

// 每个 key 使用一个确定性的单字节填充 pattern（便于 memcmp 校验与定位）。
uint8_t PatternFor(int key) {
    return static_cast<uint8_t>(0xA0 + ((key * 0x11) & 0x7F));
}

class ThreeFsNdsBatchTest : public ::testing::Test {
 protected:
    void SetUp() override {
        num_keys_ = static_cast<int>(FLAGS_num_keys);
        chunk_size_ = static_cast<size_t>(FLAGS_chunk_size);
        device_id_ = static_cast<int32_t>(FLAGS_device_id);
        total_size_ = num_keys_ * chunk_size_;
        ASSERT_GT(num_keys_, 0) << "num_keys must be > 0";
        ASSERT_GT(chunk_size_, 0u) << "chunk_size must be > 0";

        // 1. ACL 初始化
        ASSERT_EQ(aclInit(nullptr), ACL_SUCCESS) << "aclInit failed";
        ASSERT_EQ(aclrtSetDevice(device_id_), ACL_SUCCESS)
            << "aclrtSetDevice failed, device=" << device_id_;

        // 2. 分配 HBM
        ASSERT_EQ(aclrtMalloc(&hbm_, total_size_, ACL_MEM_MALLOC_HUGE_FIRST),
                  ACL_SUCCESS)
            << "aclrtMalloc failed, size=" << total_size_;
        ASSERT_NE(hbm_, nullptr);

        // 3. NDS 初始化 + 注册 buffer（NDS 使用前必须完成）
        ASSERT_EQ(nds_init(), 0) << "nds_init failed";
        ASSERT_EQ(nds_buf_register(hbm_, total_size_), 0)
            << "nds_buf_register failed";
        ASSERT_EQ(nds_get_segment_info(hbm_, &seg_infos_), 0)
            << "nds_get_segment_info failed";
        nds_initialized_ = true;

        // 4. 创建测试目录与文件（一个 key 一个文件）
        dir_ = std::string(FLAGS_mount_point) + std::string(FLAGS_test_dir);
        if (mkdir(dir_.c_str(), 0755) != 0 && errno != EEXIST) {
            FAIL() << "mkdir failed for " << dir_ << ": " << strerror(errno);
        }
        for (int k = 0; k < num_keys_; ++k) {
            std::string path = KeyPath(k);
            int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
            ASSERT_GE(fd, 0) << "open failed for " << path << ": "
                             << strerror(errno);
            fds_.push_back(fd);
            ASSERT_EQ(hf3fs_reg_fd(fd, 0), 0)
                << "hf3fs_reg_fd failed for " << path;
        }

        // 5. USRBIO 资源（mount point 需与文件所在挂载点一致）
        const int entries =
            static_cast<int>(std::max<size_t>(FLAGS_ior_entries, num_keys_));
        ASSERT_EQ(hf3fs_iovcreate(&iov_, FLAGS_mount_point.c_str(), total_size_,
                        0 /*block_size*/, -1 /*numa*/),
                  0)
            << "hf3fs_iovcreate failed";
        ASSERT_EQ(hf3fs_iorcreate(&ior_w_, FLAGS_mount_point.c_str(), entries,
                                  false /*for_read*/, FLAGS_io_depth, -1), 0)
            << "hf3fs_iorcreate(write) failed";
        ASSERT_EQ(hf3fs_iorcreate(&ior_r_, FLAGS_mount_point.c_str(), entries,
                                  true /*for_read*/, FLAGS_io_depth, -1), 0)
            << "hf3fs_iorcreate(read) failed";
        usrbio_ready_ = true;
    }

    void TearDown() override {
        if (usrbio_ready_) {
            hf3fs_iordestroy(&ior_r_);
            hf3fs_iordestroy(&ior_w_);
            hf3fs_iovdestroy(&iov_);
        }
        for (int fd : fds_) {
            hf3fs_dereg_fd(fd);
            close(fd);
        }
        if (FLAGS_cleanup) {
            for (int k = 0; k < num_keys_; ++k) {
                unlink(KeyPath(k).c_str());
            }
            rmdir(dir_.c_str());
        }
        if (nds_initialized_) {
            nds_buf_deregister(hbm_);
            nds_deinit();
        }
        if (hbm_ != nullptr) {
            aclrtFree(hbm_);
        }
        aclFinalize();
    }

    std::string KeyPath(int k) const {
        char name[64];
        snprintf(name, sizeof(name), "key_%06d.bin", k);
        return dir_ + "/" + name;
    }

    // 构造 CPU 基准 pattern 并上传到 HBM（各 key 区域独立填充）。
    uint8_t* BuildPatternAndUpload() {
        uint8_t* host = static_cast<uint8_t*>(malloc(total_size_));
        EXPECT_NE(host, nullptr);
        if (!host) return nullptr;
        for (int k = 0; k < num_keys_; ++k) {
            memset(host + static_cast<size_t>(k) * chunk_size_, PatternFor(k),
                   chunk_size_);
        }
        EXPECT_EQ(aclrtMemcpy(hbm_, total_size_, host, total_size_,
                              ACL_MEMCPY_HOST_TO_DEVICE),
                  ACL_SUCCESS);
        return host;
    }

    void* KeyBuf(int k) const {
        return static_cast<uint8_t*>(hbm_) + static_cast<size_t>(k) * chunk_size_;
    }

    // 一次性 prep + submit + wait 完成全部 key 的批量写。
    void BatchWrite() {
        for (int k = 0; k < num_keys_; ++k) {
            // 预扩展文件，与 NDS 直写路径的预期一致
            ASSERT_EQ(ftruncate(fds_[k], static_cast<off_t>(chunk_size_)), 0)
                << "ftruncate failed for key " << k;
            int ret = hf3fs_prep_npu_direct_io(
                &ior_w_, &iov_, false /*read*/, fds_[k], 0 /*off*/,
                chunk_size_, &seg_infos_, KeyBuf(k), chunk_size_, nullptr);
            ASSERT_GE(ret, 0) << "prep write failed for key " << k << ": " << ret;
        }
        ASSERT_GE(hf3fs_submit_ios(&ior_w_), 0) << "submit write failed";

        std::vector<hf3fs_cqe> cqes(num_keys_);
        int n = hf3fs_wait_for_ios(&ior_w_, cqes.data(), num_keys_, num_keys_,
                                   nullptr);
        ASSERT_EQ(n, num_keys_) << "wait_for_ios(write) returned " << n;
        for (int k = 0; k < num_keys_; ++k) {
            EXPECT_EQ(cqes[k].result, static_cast<int64_t>(chunk_size_))
                << "write result mismatch for key " << k;
        }
    }

    // 一次性批量读回 HBM，D2H 后与 host 基准比对。
    void ReadBackVerify(const uint8_t* host) {
        for (int k = 0; k < num_keys_; ++k) {
            int ret = hf3fs_prep_npu_direct_io(
                &ior_r_, &iov_, true /*read*/, fds_[k], 0 /*off*/, chunk_size_,
                &seg_infos_, KeyBuf(k), chunk_size_, nullptr);
            ASSERT_GE(ret, 0) << "prep read failed for key " << k << ": " << ret;
        }
        ASSERT_GE(hf3fs_submit_ios(&ior_r_), 0) << "submit read failed";

        std::vector<hf3fs_cqe> cqes(num_keys_);
        int n = hf3fs_wait_for_ios(&ior_r_, cqes.data(), num_keys_, num_keys_,
                                   nullptr);
        ASSERT_EQ(n, num_keys_) << "wait_for_ios(read) returned " << n;
        for (int k = 0; k < num_keys_; ++k) {
            EXPECT_EQ(cqes[k].result, static_cast<int64_t>(chunk_size_))
                << "read result mismatch for key " << k;
        }

        // HBM -> CPU 校验
        uint8_t* readback = static_cast<uint8_t*>(malloc(total_size_));
        ASSERT_NE(readback, nullptr);
        ASSERT_EQ(aclrtMemcpy(readback, total_size_, hbm_, total_size_,
                              ACL_MEMCPY_DEVICE_TO_HOST),
                  ACL_SUCCESS);
        for (int k = 0; k < num_keys_; ++k) {
            EXPECT_EQ(memcmp(readback + static_cast<size_t>(k) * chunk_size_,
                             host + static_cast<size_t>(k) * chunk_size_,
                             chunk_size_),
                      0)
                << "NDS read data mismatch for key " << k;
        }
        free(readback);
    }

    // 用普通 pread 交叉验证磁盘上的数据（确认 NDS 数据真正落盘）。
    void CpuPreadCrossCheck(const uint8_t* host) {
        int ok = 0;
        for (int k = 0; k < num_keys_; ++k) {
            int fd = open(KeyPath(k).c_str(), O_RDONLY);
            ASSERT_GE(fd, 0) << "pread open failed for key " << k;
            std::vector<uint8_t> buf(chunk_size_);
            ssize_t n = pread(fd, buf.data(), chunk_size_, 0);
            close(fd);
            if (n == static_cast<ssize_t>(chunk_size_) &&
                memcmp(buf.data(), host + static_cast<size_t>(k) * chunk_size_,
                       chunk_size_) == 0) {
                ++ok;
            } else {
                ADD_FAILURE() << "CPU pread mismatch for key " << k
                              << " (n=" << n << ")";
            }
        }
        EXPECT_EQ(ok, num_keys_) << "CPU pread cross-check failed";
    }

    void Report(const char* phase, double seconds) const {
        printf("[threefs_nds_batch] %s: %d keys x %zu B = %.2f MiB in "
               "%.3f s (%.2f MiB/s)\n",
               phase, num_keys_, chunk_size_, total_size_ / 1048576.0, seconds,
               total_size_ / 1048576.0 / seconds);
    }

    int num_keys_ = 0;
    size_t chunk_size_ = 0;
    size_t total_size_ = 0;
    int32_t device_id_ = 0;
    void* hbm_ = nullptr;
    bool nds_initialized_ = false;
    bool usrbio_ready_ = false;
    nds_segment_infos_t seg_infos_{};
    std::string dir_;
    std::vector<int> fds_;
    struct hf3fs_iov iov_ {};
    struct hf3fs_ior ior_w_ {};
    struct hf3fs_ior ior_r_ {};
};

// ---------------------------------------------------------------------------
// 用例 1：批量写入（直接调用 3FS 接口 + NDS 直通）
// ---------------------------------------------------------------------------
TEST_F(ThreeFsNdsBatchTest, WriteBatch) {
    uint8_t* host = BuildPatternAndUpload();
    ASSERT_NE(host, nullptr);

    auto t0 = std::chrono::steady_clock::now();
    BatchWrite();
    auto t1 = std::chrono::steady_clock::now();
    Report("NDS batch write", std::chrono::duration<double>(t1 - t0).count());

    free(host);
}

// ---------------------------------------------------------------------------
// 用例 2：批量回读 + 校验（依赖 WriteBatch 先写入，pattern 确定可跨进程复验）
// ---------------------------------------------------------------------------
TEST_F(ThreeFsNdsBatchTest, ReadBackVerify) {
    if (!FLAGS_read_verify) {
        GTEST_SKIP() << "--read_verify=false, skip read-back";
    }
    uint8_t* host = BuildPatternAndUpload();
    ASSERT_NE(host, nullptr);

    auto t0 = std::chrono::steady_clock::now();
    ReadBackVerify(host);
    auto t1 = std::chrono::steady_clock::now();
    Report("NDS batch read", std::chrono::duration<double>(t1 - t0).count());

    CpuPreadCrossCheck(host);
    free(host);
}

#else  // !USE_NDS

TEST(ThreeFsNdsBatchTest, Skipped) {
    GTEST_SKIP() << "threefs_nds_batch_test requires USE_NDS to be enabled";
}

#endif  // USE_NDS

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    return RUN_ALL_TESTS();
}
