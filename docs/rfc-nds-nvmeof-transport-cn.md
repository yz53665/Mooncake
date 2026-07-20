# RFC: 在 NVMe-oF Transport 中引入 NDS (NPU Direct Storage) 分支以支持昇腾 NPU 直连存储

## 1. 引言

本 RFC 提议在现有的 `nvmeof_transport` 中增加一条与 GDS（GPU Direct Storage）平行的 **NDS（NPU Direct Storage）分支**，使得基于昇腾 NPU（HBM）的推理/训练场景也能像 NVIDIA GPU + GDS 一样，直接通过 NVMe-oF 访问远端 SSD Pool，从而扩展 KV Cache 的容量上限。

本提案与社区已有的两条相关工作互补但不重叠：

- [#1940 SSD pool over NVMe-oF (SPDK 路线)](https://github.com/kvcache-ai/Mooncake/issues/1940)：在 host 侧通过 SPDK wrapper 走存储后端，并改造 LMCache 内存为 huge page + `cudaHostRegister` 以实现 zero-copy。
- [#2084 SPDK 集成补充](https://github.com/kvcache-ai/Mooncake/pull/2084)：在 1940 基础上补齐了 SSD 注册脚本、NoF 心跳、多副本清理与监控指标等。

我们的方案与上述两条路线的区别在于：**保留 transport 层的 GDS 抽象，平行引入 NDS 后端**，无需引入 SPDK、无需替换既有 CUDA pinned memory 分配逻辑，对存量 GDS 路径零侵入。

## 2. 背景与动机

### 2.1 现状

当前 `mooncake-transfer-engine/src/transport/nvmeof_transport/` 中存在一份 NVMe-oF transport 参考实现，但它强依赖于 NVIDIA GDS（`cufile.h`），只能在 NVIDIA GPU + CUDA 环境下使用：

- `CuFileContext` 通过 `cuFileHandleRegister` 注册文件句柄；
- `CUFileDescPool` 通过 `cuFileBatchIOSetUp / cuFileBatchIOSubmit` 进行批量提交；
- `NVMeoFTransport::registerLocalMemory` 调用 `cuFileBufRegister` 注册 GPU 显存。

这导致以下问题：

1. **NPU（昇腾）用户无法使用该 transport**：`cufile.h` 不存在于昇腾环境，编译直接失败；
2. **现有社区方案改造面较大**：#1940 / #2084 引入了 SPDK wrapper、NoF segment manager、心跳与脚本化注册，并且需要把 LMCache 的 pinned memory 由 `cudaHostAlloc` 改为 SPDK huge page + `cudaHostRegister`，对宿主内存管理改动较大；
3. **缺少一条与 GDS 对称、最小侵入的 NPU 直连路径**：在已有 NDS（NPU Direct Storage，昇腾提供的用户态库，等价于 GDS 的角色）能力下，理论上可以复用 transport 层的绝大多数逻辑，仅替换底层 API。

### 2.2 目标

- **零侵入 GDS 路径**：通过编译宏 `USE_NDS` 切换，不破坏现有 NVIDIA + GDS 用户的使用。
- **NPU HBM 与 NVMe-oF 直连**：通过 NDS 库（`nds_init / nds_buf_register / nds_read / nds_write`）实现 HBM ↔ NVMe-oF 的直接搬运，无需经过 host 内存中转。
- **不引入 SPDK 依赖**：避免对 LMCache pinned memory 做替换，避免新增 SPDK wrapper、NoF segment manager 等模块。
- **与现有 batch 提交模型解耦**：NDS 当前未提供 batch API，因此采用线程池 + 任务队列模型，保留后续切换为 batch API 的接口空间。

### 2.3 与 #1940 / #2084 的对比

| 维度 | #1940 / #2084 (SPDK 路线) | 本提案 (NDS 路线) |
| --- | --- | --- |
| 适用硬件 | NVIDIA GPU（仍依赖 CUDA） | 昇腾 NPU（基于 NDS 用户态库） |
| 存储后端接入方式 | 自研 `SpdkWrapper` + `SpdkNofWorkerPool` | 复用 NDS C API，无 SPDK 依赖 |
| Host 内存改造 | 需把 LMCache pinned memory 改为 SPDK huge page + `cudaHostRegister` | 不改动，NPU HBM 直接经 NDS 落盘 |
| Segment 管理 | 新增 `NoFSegmentManager`、心跳、注册脚本等 | 复用既有 `nvmeof_buffers` 与 `SegmentDesc` |
| Transport 改动 | 新增 store 层模块 | 仅在 `nvmeof_transport` 内部新增平行分支 |
| 与 GDS 关系 | 替代/并行于既有 transport | 与 GDS 完全平行，编译宏切换 |
| 风险面 | 较大（内存管理 + SPDK 部署） | 较小（仅 transport 内部分支） |

## 3. 总体架构

### 3.1 模块结构图

```mermaid
graph TB
    subgraph Existing["既有 NVMe-oF Transport (GDS 路径)"]
        TE[TransferEngine]
        NVT[NVMeoFTransport]
        CFC[CuFileContext]
        CDP[CUFileDescPool]
        TE --> NVT
        NVT -->|USE_NDS=OFF| CFC
        NVT -->|USE_NDS=OFF| CDP
        CFC -->|cuFileHandleRegister| GDS[(NVIDIA GDS Lib)]
        CDP -->|cuFileBatchIOSubmit| GDS
    end

    subgraph Proposed["本提案新增 (NDS 路径)"]
        NDS_CTX[NdsFileContext]
        NDS_API[nds.h C API]
        NVT -->|USE_NDS=ON| NDS_CTX
        NVT -->|USE_NDS=ON| TP[NdsWorkerThreadPool]
        NDS_CTX --> NDS_API
        TP --> NDS_API
        NDS_API --> NDSL[(libnds.so / NPU Direct Storage)]
    end

    NDSL -.HBM 直连.-> NVME[(NVMe-oF Target / SSD Pool)]
    GDS -.GPU 显存直连.-> NVME
```

### 3.2 编译期分支

```mermaid
flowchart LR
    SRC[nvmeof_transport.cpp] --> IS_NDS{USE_NDS?}
    IS_NDS -->|是| INC1[nds_context.h<br/>nds.h]
    IS_NDS -->|否| INC2[cufile_context.h<br/>cufile_desc_pool.h]
    INC1 --> LINK1[libnds.so + ascendcl]
    INC2 --> LINK2[libcufile.so + CUDA]
```

`CMakeLists.txt` 中通过 `USE_NDS` 选项控制：

- `USE_NDS=ON`：仅编译 `nvmeof_transport.cpp`，链接 `libnds.so` 与 `ascendcl`；
- `USE_NDS=OFF`（默认）：额外编译 `cufile_context.cpp`、`cufile_desc_pool.cpp`，链接 GDS。

## 4. 类图设计

### 4.1 NVMeoFTransport 与两条后端分支

```mermaid
classDiagram
    class Transport {
        <<interface>>
        +allocateBatchID(batch_size) BatchID
        +submitTransferTask(task_list) Status
        +submitTransfer(batch_id, entries) Status
        +getTransferStatus(batch_id, task_id, status) Status
        +freeBatchID(batch_id) Status
        +registerLocalMemory(addr, length, location, ...) int
        +unregisterLocalMemory(addr) int
    }

    class NVMeoFTransport {
        -install(...) int
        -registerLocalMemory(...) int
        -unregisterLocalMemory(...) int
        +addSliceToTask(...)
        +allocateBatchID(batch_size) BatchID
        +submitTransferTask(task_list) Status
        +submitTransfer(...) Status
        +getTransferStatus(...) Status
        +freeBatchID(batch_id) Status
    }

    class CuFileContext {
        -CUfileHandle_t handle
        -CUfileDescr_t desc
        +CuFileContext(filename)
        +getHandle() CUfileHandle_t
    }

    class CUFileDescPool {
        -descs_[256]
        -handle_pool_
        +allocCUfileDesc(batch_size) int
        +pushParams(idx, params) int
        +submitBatch(idx) int
        +getTransferStatus(idx, slice_id) CUfileIOEvents_t
        +freeCUfileDesc(idx) int
    }

    class NdsFileContext {
        -nds_Handle handle_
        -int fd_
        -int32_t device_id_
        +NdsFileContext(filename, device_id)
        +getHandle() nds_Handle
        +getDeviceId() int32_t
    }

    class NdsWorkerThreadPool {
        -nds_workers_
        -nds_task_queue_
        -nds_queue_mutex_
        -nds_queue_cv_
        -nds_running_
        +initializeNdsThreadPool()
        +stopNdsThreadPool()
        +ndsWorkerThread()
        +submitNdsSlice(slice)
    }

    Transport <|.. NVMeoFTransport
    NVMeoFTransport o-- CuFileContext : USE_NDS=OFF
    NVMeoFTransport o-- CUFileDescPool : USE_NDS=OFF
    NVMeoFTransport o-- NdsFileContext : USE_NDS=ON
    NVMeoFTransport o-- NdsWorkerThreadPool : USE_NDS=ON
```

### 4.2 NDS C API 抽象（`nds.h`）

```mermaid
classDiagram
    class nds_file_ctx_t {
        +int fd
        +int dummy
    }
    class nds_Handle {
        <<typedef>>
    }
    class read_parameter {
        +nds_Handle nds_handle
        +void* buf
        +size_t nbyte
        +off_t offset
        +ssize_t result_len
    }
    class NDSAPI {
        <<C functions>>
        +nds_init(device_id) int
        +nds_deinit(device_id) void
        +nds_buf_register(device_id, buf, len) int
        +nds_buf_deregister(device_id, buf) int
        +nds_file_register(fd) nds_Handle
        +nds_file_deregister(fd) int
        +nds_read(handle, device_id, buf, nbyte, offset) ssize_t
        +nds_write(handle, buf, nbyte, offset) ssize_t
    }
    nds_file_ctx_t ..> nds_Handle
    read_parameter ..> nds_Handle
    NDSAPI ..> nds_Handle
```

## 5. 关键流程图

### 5.1 初始化与内存注册流程

```mermaid
sequenceDiagram
    participant App as Application
    participant TE as TransferEngine
    participant NVT as NVMeoFTransport
    participant NDS as NDS Lib

    App->>TE: install / registerLocalMemory(addr, len)
    TE->>NVT: registerLocalMemory(addr, len, ...)
    alt USE_NDS=ON 且未初始化
        NVT->>NDS: nds_init(device_id)
        NDS-->>NVT: 0
        NVT->>NVT: initializeNdsThreadPool()
    end
    NVT->>NDS: nds_buf_register(device_id, addr, len)
    NDS-->>NVT: 0
    NVT-->>TE: 0
```

### 5.2 Batch 提交与 Slice 执行流程（NDS 路径）

```mermaid
sequenceDiagram
    participant App as Application
    participant NVT as NVMeoFTransport
    participant Pool as NdsWorkerThreadPool
    participant Ctx as NdsFileContext
    participant NDS as NDS Lib

    App->>NVT: submitTransfer(batch_id, entries)
    loop 每个 TransferRequest
        NVT->>NVT: 计算 slice 与 file_offset
        NVT->>Ctx: 查找/创建 NdsFileContext(file_path, device_id)
        Ctx->>NDS: nds_file_register(fd)
        NDS-->>Ctx: nds_Handle
        NVT->>NVT: addSliceToTask(...) 构造 Slice
        NVT->>Pool: submitNdsSlice(slice)
        Pool->>Pool: 入队 (锁 + condition_variable)
    end
    NVT-->>App: Status::OK()

    par Worker 线程并发消费
        Pool->>Ctx: 读取 nds_Handle
        alt READ
            Pool->>NDS: nds_read(handle, dev_id, buf, len, offset)
        else WRITE
            Pool->>NDS: nds_write(handle, buf, len, offset)
        end
        NDS-->>Pool: ssize_t
        alt result >= 0
            Pool->>Pool: slice->markSuccess()
        else
            Pool->>Pool: slice->markFailed()
        end
    end
```

### 5.3 状态查询流程

```mermaid
flowchart TD
    Q[getTransferStatus batch_id, task_id] --> CT{USE_NDS?}
    CT -->|ON| AGG[聚合 task.success_slice_count<br/>task.failed_slice_count]
    AGG --> J1{success+failed == slice_count?}
    J1 -->|否| W[WAITING]
    J1 -->|是| J2{failed > 0?}
    J2 -->|是| F[FAILED]
    J2 -->|否| C[COMPLETED]
    CT -->|OFF| EV[desc_pool_->getTransferStatus<br/>CUfileIOEvents_t]
    EV --> MAP[from_cufile_transfer_status]
    MAP --> OUT[返回 status]
```

### 5.4 NDS 路径整体数据流

```mermaid
flowchart LR
    subgraph NPU["NPU (Ascend)"]
        HBM[HBM 显存<br/>nds_buf_register]
    end
    subgraph Host["Host Process"]
        NVT[NVMeoFTransport]
        TP[NdsWorkerThreadPool]
        Ctx[NdsFileContext<br/>fd + nds_Handle]
    end
    subgraph Backend["Storage Backend"]
        NVME[(NVMe-oF Target)]
    end

    HBM -->|nds_read/nds_write| NVME
    NVT --> TP
    TP --> Ctx
    Ctx -. 打开文件 .-> NVME
    TP -. HBM 直传 .-> NVME
```

## 6. 文件结构

```mermaid
graph LR
    subgraph "include/transport/nvmeof_transport/"
        H1[nds.h<br/>NDS C API 声明]
        H2[nds_context.h<br/>NdsFileContext]
        H3[cufile_context.h<br/>既有 GDS]
        H4[cufile_desc_pool.h<br/>既有 GDS]
        H5[nvmeof_transport.h<br/>编译宏分支]
    end
    subgraph "src/transport/nvmeof_transport/"
        S1[nvmeof_transport.cpp<br/>双分支实现]
        S2[cufile_context.cpp<br/>既有 GDS]
        S3[cufile_desc_pool.cpp<br/>既有 GDS]
        S4[CMakeLists.txt<br/>USE_NDS 开关]
    end
    H5 --> H3
    H5 --> H4
    H5 --> H2
    H2 --> H1
    S1 --> H5
    S1 --> S2
    S1 --> S3
    S4 --> S1
```

## 7. 配置与可观测性

通过环境变量进行运行时配置（无需改代码）：

| 环境变量 | 含义 | 默认值 |
| --- | --- | --- |
| `USE_NDS`（编译期） | 启用 NDS 分支 | `OFF` |
| `MC_NDS_DEVICE_ID` | NPU device id | `-1`（不初始化） |
| `MC_NDS_THREAD_POOL_SIZE` | NDS worker 线程数 | `8`（范围 1–64） |

状态查询在 NDS 路径下直接基于 `task.success_slice_count / failed_slice_count` 聚合，与 GDS 路径基于 `CUfileIOEvents_t` 的查询语义保持一致（参见 5.3）。

## 8. 与社区路线的协同

- 与 #1940 / #2084 互补：本提案聚焦 transport 层的 NPU 直连，不触及 store 层的 NoF segment 管理、心跳、SSD 注册脚本，这些能力可由 #1940 / #2084 提供，二者可叠加使用。
- 不修改既有 GDS 分支：`CuFileContext`、`CUFileDescPool` 保持原样，存量用户编译/行为不变。
- 不替换 LMCache pinned memory：避免 #1940 中 `cudaHostAlloc` → SPDK huge page + `cudaHostRegister` 的改造，降低跨组件耦合。

## 9. 后续工作

1. 在 NDS 提供 batch API 后，将 `NdsWorkerThreadPool` 替换为与 `CUFileDescPool` 对称的 batch 提交模型；
2. 在 `Slice` 层面引入更细粒度的 `transferred_bytes` 上报，对齐 GDS 路径的事件粒度；
3. 评估是否抽取公共 `NvmeOfFileContext` 抽象基类，统一 GDS/NDS 的句柄管理接口。

## 10. 参考文献

- [#1940 SSD pool over NVMe-oF (SPDK)](https://github.com/kvcache-ai/Mooncake/issues/1940)
- [#2084 NVMe-oF SSD cache support (SPDK 补充)](https://github.com/kvcache-ai/Mooncake/pull/2084)
- NVIDIA GDS: https://docs.nvidia.com/gpudirect-storage/
