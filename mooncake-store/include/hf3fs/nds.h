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

#define NDS_HOST_ID (-1)

/**
 * @brief 声明nds句柄 nds_Handle
 */
typedef struct nds_file_ctx_t* nds_Handle;
typedef struct nds_batch_context* nds_batch_handle_t;

/**
 * @brief Metadata required to access a remote registered NDS segment.
 * @note The actual remote address and length are supplied by the imported
 *       read/write arguments. The caller must guarantee that buf/nbyte is in
 *       the exported registered segment.
 */
typedef struct nds_segment_info {
    uint8_t eid[16];
    uint32_t uasid;
    uint32_t jetty_id;
    uint32_t token_id;
} nds_segment_info_t;

typedef struct nds_segment_infos {
    nds_segment_info_t h2d_segment_info;
    nds_segment_info_t rh2d_segment_info;
} nds_segment_infos_t;

/**
 * @brief nds read/write 输入参数
 */
typedef struct {
    nds_Handle nds_handle;    // nds 句柄
    int32_t device_id;        // buf 注册时使用的 device_id，Host DDR 使用 NDS_HOST_ID
    void* buf;                // HBM 或 Host DDR 内存数据缓冲区
    size_t nbyte;             // 期望读写长度
    off_t offset;             // nds_handle 中的文件/块设备偏移
} nds_io_parameter;

typedef enum {
    NDS_BATCH_IO_READ = 0,
    NDS_BATCH_IO_WRITE = 1,
} nds_batch_io_op_t;

typedef enum {
    NDS_BATCH_IO_WAITING = 0,
    NDS_BATCH_IO_COMPLETED = 1,
    NDS_BATCH_IO_FAILED = 2,
} nds_batch_io_status_t;

/**
 * @brief Submit parameter for one batch async I/O entry.
 */
typedef struct {
    nds_Handle nds_handle;
    int32_t device_id;
    void *buf;
    size_t nbyte;
    off_t offset;
    nds_batch_io_op_t opcode;
    void *cookie;
} nds_batch_io_params_t;

/**
 * @brief Completion event for one batch async I/O entry.
 */
typedef struct {
    void *cookie;
    nds_batch_io_status_t status;
    ssize_t ret;
    int error;
} nds_batch_io_events_t;

/**
 * @brief Initialize NDS user-space library
 * This function performs the initialization of the NDS component, including:
 * @param device_id NPU device ID
 * @return 0 on success, -1 on failure
 * @note Call once per process. Host DDR URMA resources are initialized automatically when available.
 *       Their initialization failure does not block the existing NPU path.
 * @warning This function must be called before any other NDS interfaces.
 * @see nds_deinit
 */
int nds_init(int32_t device_id);

/**
 * @brief Release NDS user-space library resources
 * This function releases all resources occupied by the NDS component, including:
 * @param device_id NPU device ID passed to nds_init
 * @note Even if some resources fail to be released, the function will continue to release other resources.
 * @warning Must be called after all buffers are deregistered, otherwise resource leaks may occur.
 * @see nds_init
 */
void nds_deinit(int32_t device_id);

/**
 * @brief Register a local NDS memory buffer
 * This function registers a local memory buffer to the NDS component, making it available for RDMA communication.
 * After registration, the remote access key (MemKey) for this memory region is obtained for subsequent remote memory access.
 * @param device_id NPU device ID, or NDS_HOST_ID for Host DDR memory
 * @param buf HBM or Host DDR memory buffer address
 * @param len Buffer length in bytes
 * @return 0 on success, -1 on failure
 * @note Must call nds_init with an NPU device ID before registration.
 * @warning Re-registering the same device_id/buf/len is idempotent. Re-registering the same buf
 *          with a different len is not supported. Memory must not be freed before deregistration.
 * @see nds_buf_deregister
 */
int nds_buf_register(int32_t device_id, void *buf, size_t len);

/**
 * @brief Deregister a local NDS memory buffer
 * This function deregisters the previously registered local memory buffer and releases related resources.
 * After deregistration, this memory region can no longer be used for RDMA communication.
 * @param device_id NPU device ID, or NDS_HOST_ID for Host DDR memory
 * @param buf HBM or Host DDR memory buffer address
 * @return 0 on success, -1 on failure
 * @note Must call nds_buf_register to register the buffer first.
 * @warning After deregistration, the buffer should not be used for RDMA operations.
 * @see nds_buf_register
 */
int nds_buf_deregister(int32_t device_id, void *buf);

/**
 * @brief Get the remote-access metadata for a registered HBM segment address.
 * This function exports the minimum segment metadata required by imported NDS
 * read/write operations. The caller can pass either the registered buffer base
 * address or an address inside the registered range.
 * @param buf Any address inside a registered HBM buffer.
 * @param out Segment metadata used to import the registered remote segment.
 * @return 0 on success, -1 on failure
 * @note Must call nds_init and nds_buf_register before this function.
 * @warning The exported metadata is valid only while the source process keeps
 *          the corresponding NDS device and buffer registration alive.
 * @see nds_buf_register, nds_read_imported, nds_write_imported
 */
int nds_get_segment_info(void *buf, nds_segment_infos_t *out);

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

/**
 * @brief Read data from a file or block device into local registered NPU HBM.
 * @param nds_handle NDS file handle returned by nds_file_register.
 * @param device_id NPU device ID used when registering buf, or NDS_HOST_ID for Host DDR.
 * @param buf Destination buffer address in local registered NPU HBM or Host DDR.
 * @param nbyte Number of bytes to transfer.
 * @param offset File or block device offset.
 * @return Number of bytes read on success, -1 on failure.
 * @note buf can be any address inside a registered buffer range.
 * @see nds_buf_register, nds_file_register
 */
ssize_t nds_read(nds_Handle nds_handle, int32_t device_id, void *buf, size_t nbyte, off_t offset);

/**
 * @brief Read data from a file or block device into an imported remote NPU HBM segment.
 * This function uses metadata returned by nds_get_segment_info in another
 * process or node. The destination buffer address uses the same addressing
 * semantic as nds_read: it is the actual remote NPU VA to be written and may
 * point inside the exported registered segment.
 * @param nds_handle NDS file handle returned by nds_file_register.
 * @param segment Imported remote segment metadata returned by nds_get_segment_info.
 * @param buf Destination remote NPU buffer address.
 * @param nbyte Number of bytes to transfer.
 * @param offset File or block device offset.
 * @return Number of bytes read on success, -1 on failure.
 * @note The process that produced segment must keep the corresponding NDS
 *       device and buffer registration alive until this operation completes.
 *       The caller must guarantee that buf/nbyte is inside that remote
 *       registered segment.
 * @warning Imported I/O is only supported for block-device NDS paths.
 * @see nds_get_segment_info, nds_write_imported
 */
ssize_t nds_read_imported(nds_Handle nds_handle, const nds_segment_info_t *segment,
    void *buf, size_t nbyte, off_t offset);

/*
 * @brief Write data from NPU device memory or Host DDR to a file or block device
 * For regular files, HBM data is copied to Host staging memory before file
 * write, while Host DDR data is written directly with pwrite. Block-device
 * writes use the NDS URMA path for both HBM and Host DDR buffers.
 * @param nds_handle NDS file handle returned by nds_file_register.
 * @param device_id NPU device ID used when registering buf, or NDS_HOST_ID for Host DDR
 * @param buf      Source buffer address in NPU HBM (device memory) or Host DDR
 * @param nbyte    Number of bytes to transfer
 * @param offset   File offset for the write operation
 * @return Number of bytes written on success, -1 on failure
 * @note The caller is responsible for opening/closing the target fd.
 * @warning buf must point to registered HBM memory on the NPU device or registered Host DDR memory.
 *          Use nds_buf_register() to register the buffer first.
 * @see nds_buf_register, nds_buf_deregister
 */
ssize_t nds_write(nds_Handle nds_handle, int32_t device_id, void *buf, size_t nbyte, off_t offset);

/**
 * @brief Write data from an imported remote NPU HBM segment to a file or block device
 * This function uses metadata returned by nds_get_segment_info in another
 * process or node. The source buffer address uses the same addressing semantic
 * as nds_write: it is the actual remote NPU VA to be read and may point inside
 * the exported registered segment.
 * @param nds_handle NDS file handle returned by nds_file_register.
 * @param segment Imported remote segment metadata returned by nds_get_segment_info.
 * @param buf Source remote NPU buffer address.
 * @param nbyte Number of bytes to transfer.
 * @param offset File or block device offset.
 * @return Number of bytes written on success, -1 on failure.
 * @note The process that produced segment must keep the corresponding NDS
 *       device and buffer registration alive until this operation completes.
 *       The caller must guarantee that buf/nbyte is inside that remote
 *       registered segment.
 * @warning Regular-file imported write uses a temporary user-space URMA transfer path.
 * @see nds_get_segment_info, nds_read_imported
 */
ssize_t nds_write_imported(nds_Handle nds_handle, const nds_segment_info_t *segment,
    void *buf, size_t nbyte, off_t offset);

/**
 * @brief Create a batch async I/O context.
 * @param handle Output batch handle.
 * @param max_io_count Maximum number of I/O entries supported by this batch handle.
 * @return 0 on success, -1 on failure
 */
int nds_batch_io_setup(nds_batch_handle_t *handle, unsigned max_io_count);

/**
 * @brief Submit a batch of async I/O entries.
 * @param handle Batch handle returned by nds_batch_io_setup.
 * @param io_count Number of I/O entries to submit.
 * @param params I/O parameter array.
 * @return 0 on submit accepted, -1 on failure
 */
int nds_batch_io_submit(nds_batch_handle_t handle, unsigned io_count, nds_batch_io_params_t *params);

/**
 * @brief Query completion events for a submitted batch.
 * @param handle Batch handle returned by nds_batch_io_setup.
 * @param event_count Input event capacity, output number of returned events.
 * @param events Completion output array.
 * @return 0 on success, -1 on failure
 */
int nds_batch_io_get_status(nds_batch_handle_t handle, unsigned *event_count,
    nds_batch_io_events_t *events);

/**
 * @brief Reset a completed batch async I/O context for reuse.
 * @param handle Batch handle returned by nds_batch_io_setup.
 * @return 0 on success, -1 on failure
 */
int nds_batch_io_reset(nds_batch_handle_t handle);

/**
 * @brief Destroy a batch async I/O context and release related resources.
 * @param handle Batch handle returned by nds_batch_io_setup.
 * @return 0 on success, -1 on failure
 */
int nds_batch_io_destroy(nds_batch_handle_t handle);


#ifdef __cplusplus
}
#endif
 
#endif /* NDS_H */
