/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and limitations under the License.
==============================================================================*/

#ifndef NDS_CONTEXT_H_
#define NDS_CONTEXT_H_

#ifdef USE_NDS

#include <fcntl.h>
#include <glog/logging.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "transport/nvmeof_transport/nds.h"

namespace mooncake {

class NdsFileContext {
   public:
    nds_Handle getHandle() const { return handle_; }

    explicit NdsFileContext(const char *filename, int32_t device_id)
        : device_id_(device_id) {
        fd_ = open(filename, O_RDWR);
        if (fd_ < 0) {
            LOG(ERROR) << "NdsFileContext: Failed to open file " << filename
                       << ", errno=" << errno;
            handle_ = nullptr;
            return;
        }
        handle_ = nds_file_register(fd_);
        if (!handle_) {
            LOG(ERROR) << "NdsFileContext: nds_file_register failed for "
                       << filename;
            close(fd_);
            fd_ = -1;
            return;
        }
    }

    NdsFileContext(const NdsFileContext &) = delete;
    NdsFileContext &operator=(const NdsFileContext &) = delete;

    ~NdsFileContext() {
        if (handle_) {
            nds_file_deregister(fd_);
            handle_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    int32_t getDeviceId() const { return device_id_; }

   private:
    nds_Handle handle_ = nullptr;
    int fd_ = -1;
    int32_t device_id_;
};

}  // namespace mooncake

#endif  // USE_NDS

#endif  // NDS_CONTEXT_H_
