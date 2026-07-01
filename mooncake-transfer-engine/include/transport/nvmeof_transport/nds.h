/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and limitations under the License.
==============================================================================*/

#ifndef NDS_H
#define NDS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 声明nds句柄 nds_Handle
 */
typedef struct nds_file_ctx_t* nds_Handle;

/**
 * @brief nds read/write 输入参数
 */
typedef struct {
    nds_Handle nds_handle;    // nds 句柄
    int32_t device_id;        // HBM buf 注册时使用的deivce_id
    void* buf;                // HBM内存数据缓冲区
    size_t nbyte;             // 期望读写长度
    off_t offset;             // nds_handle 中的文件/块设备偏移
} nds_io_parameter;

/**
 * @brief Initialize NDS user-space library
 * This function performs the initialization of the NDS component, including:
 * @param device_id NPU device ID
 * @return 0 on success, -1 on failure
 * @note Each device should be initialized only once. Repeated initialization may cause resource leaks.
 * @warning This function must be called before any other NDS interfaces.
 * @see nds_deinit
 */
int nds_init(int32_t device_id);

/**
 * @brief Release NDS user-space library resources
 * This function releases all resources occupied by the NDS component, including:
 * @param device_id NPU device ID
 * @note Even if some resources fail to be released, the function will continue to release other resources.
 * @warning Must be called after all buffers are deregistered, otherwise resource leaks may occur.
 * @see nds_init
 */
void nds_deinit(int32_t device_id);

/**
 * @brief Register HBM memory buffer
 * This function registers the HBM memory buffer to the NDS component, making it available for RDMA communication.
 * After registration, the remote access key (MemKey) for this memory region is obtained for subsequent remote memory access.
 * @param device_id NPU device ID
 * @param buf HBM memory buffer address
 * @param len Buffer length in bytes
 * @return 0 on success, -1 on failure
 * @note Must call nds_init to initialize the device first.
 * @warning The same buffer cannot be registered multiple times. Memory must not be freed before deregistration.
 * @see nds_buf_deregister
 */
int nds_buf_register(int32_t device_id, void *buf, size_t len);

/**
 * @brief Deregister HBM memory buffer
 * This function deregisters the previously registered HBM memory buffer and releases related resources.
 * After deregistration, this memory region can no longer be used for RDMA communication.
 * @param device_id NPU device ID
 * @param buf HBM memory buffer address
 * @return 0 on success, -1 on failure
 * @note Must call nds_buf_register to register the buffer first.
 * @warning After deregistration, the buffer should not be used for RDMA operations.
 * @see nds_buf_register
 */
int nds_buf_deregister(int32_t device_id, void *buf);

/**
 * @brief 注册文件信息
 * @param fd 文件fd
 * @return nds_handle NDS句柄
 */
nds_Handle nds_file_register(int fd);

/**
 * @brief 取消文件注册，并释放文件fd对应NDS句柄nds_handle资源
 * @param fd 文件fd
 * @return 0 on success, -1 on failure
 */
int nds_file_deregister(int fd);

ssize_t nds_read(nds_Handle nds_handle, int32_t device_id, void *buf, size_t nbyte, off_t offset);

/*
 * @brief Write data from NPU device memory to a file or block device
 * This function copies data from NPU HBM (High Bandwidth Memory) to
 * host memory via aclrtMemcpy (ACL_MEMCPY_DEVICE_TO_HOST), then writes
 * the data to the target device through normal file/block I/O (pwrite).
 * The typical use case is offloading NPU-resident data to persistent
 * storage without an intermediate user-space copy of the full buffer.
 * @param handle   Target device file descriptor (obtained via open/creat)
 * @param buf      Source buffer address in NPU HBM (device memory)
 * @param nbyte    Number of bytes to transfer
 * @param offset   File offset for the write operation
 * @return Number of bytes written on success, -1 on failure
 * @note The caller is responsible for opening/closing the target fd.
 * @warning buf must point to registered HBM memory on the NPU device.
 *          Use nds_buf_register() to register the buffer first.
 * @see nds_buf_register, nds_buf_deregister
 */
ssize_t nds_write(nds_Handle nds_handle, int32_t device_id, void *buf, size_t nbyte, off_t offset);


#ifdef __cplusplus
}
#endif

#endif /* NDS_H */
