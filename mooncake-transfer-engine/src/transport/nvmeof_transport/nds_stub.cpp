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

// Stub implementation of NDS (NPU Data Service) API for testing without
// real NDS hardware/library. All functions return success and simulate
// immediate I/O completion so the async batch flow can be tested end-to-end.
// Note: ACL functions (aclrtGetDevice/aclrtSetDevice) are NOT stubbed here
// since they come from the standard ascendcl library which is always available.

#include "transport/nvmeof_transport/nds.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// ============================================================================
// NDS internal structures (opaque types declared in nds.h)
// ============================================================================

struct nds_file_ctx_t {
    int fd;
    int32_t device_id;
};

struct nds_batch_context {
    unsigned max_nr;
    std::vector<nds_batch_io_params_t> params;
    bool submitted;
};

// fd -> handle map for deregister lookup
static std::mutex g_fd_map_lock;
static std::unordered_map<int, nds_Handle> g_fd_map;

// ============================================================================
// NDS initialization / deinitialization stubs
// ============================================================================

extern "C" int nds_init(int32_t device_id) {
    (void)device_id;
    return 0;
}

extern "C" void nds_deinit(int32_t device_id) {
    (void)device_id;
}

// ============================================================================
// NDS buffer registration stubs
// ============================================================================

extern "C" int nds_buf_register(int32_t device_id, void *buf, size_t len) {
    (void)device_id;
    (void)buf;
    (void)len;
    return 0;
}

extern "C" int nds_buf_deregister(int32_t device_id, void *buf) {
    (void)device_id;
    (void)buf;
    return 0;
}

// ============================================================================
// NDS segment info stub
// ============================================================================

extern "C" int nds_get_segment_info(void *buf, nds_segment_infos_t *out) {
    (void)buf;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return 0;
}

// ============================================================================
// NDS file registration stubs
// ============================================================================

extern "C" nds_Handle nds_file_register(int fd) {
    auto *ctx = new nds_file_ctx_t();
    ctx->fd = fd;
    ctx->device_id = 0;
    {
        std::lock_guard<std::mutex> lock(g_fd_map_lock);
        g_fd_map[fd] = ctx;
    }
    return ctx;
}

extern "C" int nds_file_deregister(int fd) {
    std::lock_guard<std::mutex> lock(g_fd_map_lock);
    auto it = g_fd_map.find(fd);
    if (it != g_fd_map.end()) {
        delete it->second;
        g_fd_map.erase(it);
    }
    return 0;
}

// ============================================================================
// NDS synchronous read/write stubs (return full transfer)
// ============================================================================

extern "C" ssize_t nds_read(nds_Handle nds_handle, int32_t device_id,
                            void *buf, size_t nbyte, off_t offset) {
    (void)nds_handle;
    (void)device_id;
    (void)buf;
    (void)offset;
    return static_cast<ssize_t>(nbyte);
}

extern "C" ssize_t nds_read_imported(nds_Handle nds_handle,
                                     const nds_segment_info_t *segment,
                                     void *buf, size_t nbyte, off_t offset) {
    (void)nds_handle;
    (void)segment;
    (void)buf;
    (void)offset;
    return static_cast<ssize_t>(nbyte);
}

extern "C" ssize_t nds_write(nds_Handle nds_handle, int32_t device_id,
                             void *buf, size_t nbyte, off_t offset) {
    (void)nds_handle;
    (void)device_id;
    (void)buf;
    (void)offset;
    return static_cast<ssize_t>(nbyte);
}

extern "C" ssize_t nds_write_imported(nds_Handle nds_handle,
                                      const nds_segment_info_t *segment,
                                      void *buf, size_t nbyte, off_t offset) {
    (void)nds_handle;
    (void)segment;
    (void)buf;
    (void)offset;
    return static_cast<ssize_t>(nbyte);
}

// ============================================================================
// NDS async batch I/O stubs
// ============================================================================

extern "C" int nds_batch_io_setup(nds_batch_handle_t *handle, unsigned max_nr) {
    if (!handle) return -1;
    auto *ctx = new nds_batch_context();
    ctx->max_nr = max_nr;
    ctx->submitted = false;
    *handle = ctx;
    return 0;
}

extern "C" int nds_batch_io_submit(nds_batch_handle_t handle, unsigned nr,
                                nds_batch_io_params_t *params, unsigned flags) {
    (void)flags;
    if (!handle || !params) return -1;
    handle->params.assign(params, params + nr);
    handle->submitted = true;
    return 0;
}

extern "C" int nds_batch_io_get_status(nds_batch_handle_t handle, unsigned min_nr,
                                   unsigned *nr, nds_batch_io_events_t *events,
                                   const struct timespec *timeout) {
    (void)min_nr;
    (void)timeout;
    if (!handle || !nr || !events) return -1;
    if (!handle->submitted) {
        *nr = 0;
        return 0;
    }
    // Simulate immediate completion for all submitted slices
    unsigned count = static_cast<unsigned>(handle->params.size());
    if (*nr < count) count = *nr;
    for (unsigned i = 0; i < count; ++i) {
        events[i].cookie = handle->params[i].cookie;
        events[i].status = NDS_BATCH_IO_COMPLETED;
        events[i].ret = static_cast<ssize_t>(handle->params[i].u.batch.size);
        events[i].error = 0;
    }
    *nr = count;
    return 0;
}

extern "C" int nds_batch_io_destroy(nds_batch_handle_t handle) {
    if (handle) {
        delete handle;
    }
    return 0;
}
