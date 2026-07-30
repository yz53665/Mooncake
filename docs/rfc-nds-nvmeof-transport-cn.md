# RFC: 引入 NDS 分支并扩展 Master 层 SSD Segment 管理以支持昇腾 NPU 直连 NVMe-oF 存储

## 1. 引言

本 RFC 提议为昇腾 NPU 场景补齐 NVMe-oF 直连存储能力，包含两项紧密关联的改动：

1. **Transport 层 NDS 分支**：在 `nvmeof_transport` 中增加一条与 GDS（GPU Direct Storage）平行的 **NDS（NPU Direct Storage）分支**，使基于昇腾 NPU（HBM）的推理/训练场景能像 NVIDIA GPU + GDS 一样直接通过 NVMe-oF 访问远端 SSD Pool，扩展 KV Cache 容量上限。
2. **Master 层 SSD Segment 管理扩展**：针对普通块设备地址偏移寻址 SSD 盘的共享与故障特点，在既有 `NoFSegmentManager` 基础上扩展多 client 共享 segment（`client_refs` 引用计数）、探针注入（`NoFProbeFn`），并复用既有故障强制卸载与副本清理链路，使 NDS 路径与既有 SPDK 路线共享同一套 segment 生命周期管理框架。

两项改动一脉相承：transport 层提供 NPU 直连数据面，master 层提供与存储后端解耦的 segment 管理控制面，共同构成完整的 NPU 直连 SSD 方案。

本提案与社区已有的 SPDK NoF 路线互补但不重叠：

- [#1940 SSD pool over NVMe-oF](https://github.com/kvcache-ai/Mooncake/issues/1940)：提出了基于 SPDK 的 NVMe-oF SSD Pool 整体架构方案。
- [#2084 NVMe-oF SSD cache support](https://github.com/kvcache-ai/Mooncake/pull/2084)：在 #1940 基础上实现了 store 层基础设施（`SpdkWrapper`、`NoFSegmentManager`、NoF 心跳与自动卸载、多副本清理、监控指标等），是本仓库中 SPDK NoF 路线的实际代码基础。

本提案通过编译宏 `USE_NVMEOF_NDS` 在 transport 层启用 NDS 直连路径（替代 GDS），通过 `USE_NOF` 在 master 层启用 NoF segment 管理基础设施。两个宏作用在不同层次，组合使用即可构成完整的 NPU 直连 SSD 方案。由于 NPU HBM 直接经 NDS 与 NVMe-oF target 间 DMA，因此无需像 SPDK 路径那样通过 `SpdkWrapper::Alloc` 分配 host 侧 DMA buffer。

## 2. 背景与动机

### 2.1 Transport 层：NPU 直连存储路径缺失

当前仓库中围绕 NVMe-oF 直连存储存在两条已实现的路径，但二者都面向 NVIDIA GPU：

1. **GDS 参考实现（transport 层）**：`mooncake-transfer-engine/src/transport/nvmeof_transport/` 强依赖 NVIDIA GDS（`cufile.h`），通过 `cuFileHandleRegister` 注册句柄、`cuFileBatchIOSubmit` 批量提交。需要指出的是，该实现仅为最小可运行路径（`CuFileContext` 句柄注册、`CUFileDescPool` 批量提交、基于 `CUfileIOEvents_t` 的状态查询），要求用户自行发现并挂载远端 NVMe-oF target，缺少自动化的 segment 生命周期管理、心跳检测、副本放置策略等上层能力。
2. **SPDK 路线（#1940 / #2084，store 层）**：在 host 侧通过 SPDK 用户态驱动直接访问远端 NVMe-oF target，提供了独立的 NoF segment 管理、心跳与故障隔离、多副本支持等完整的 SSD Pool 能力。这是一条面向 NVIDIA GPU + CUDA 环境的完整工程化方案。

两条路径共同的问题在于：**昇腾 NPU 场景缺失**。GDS 依赖 `cufile.h` 与 CUDA 环境，SPDK 路线同样依赖 CUDA 环境，二者在昇腾环境下均不可用。

与此同时，昇腾侧已提供与 GDS 角色对等的 **NDS（NPU Direct Storage）用户态库**（`nds_init / nds_buf_register / nds_read / nds_write`），能够在 NPU HBM 与 NVMe-oF target 之间直接发起 DMA，具备构建对等直连路径的底层能力。因此本提案在 transport 层引入与 GDS 平行的 NDS 分支。

### 2.2 Master 层：SSD 共享与故障场景下生命周期管理不足

无论上层是 GDS、SPDK 还是 NDS，transport 层只负责"在给定 fd + offset 上发起一次 DMA"，并不感知这块 SSD 是谁挂载的、是否还健康、还有多少容量、其他 client 是否也在使用。这些职责由 master 层的 `NoFSegmentManager` 承担。然而现有 `NoFSegmentManager` 的设计存在三处与 SSD 实际使用方式不匹配的缺口：

1. **1:1 挂载语义无法表达多 client 共享**。现有 `NoFSegmentManager` 假定一个 segment 由一个 client 独占使用。但在 NVMe-oF + SSD Pool 场景下，一块物理 SSD 经常被多个推理/训练 client 同时挂载使用。如果沿用 1:1 语义，则每个 client 各自挂载一份会产生重复的 segment 对象与重复的容量计数；若强制串行化则又限制并发。
2. **缺少与具体驱动解耦的健康探针**。既有心跳探测直接调用 `SpdkWrapper::ProbeNofSegment`，探针与 SPDK 强绑定。NDS 路径下不存在 SPDK wrapper，若沿用既有实现则心跳形同虚设。
3. **SSD 故障的不可迁移性需要专门的强制卸载路径**。SSD/NVMe-oF target 一旦不可达，其上数据无法被读取，因此无法走"先迁移再卸载"的 Drain 路径。既有代码已为 SPDK 路线实现了强制卸载链路，本提案需要让 NDS 路径也能复用该链路。

综上，master 层扩展的动机是：**让 NDS 路径能复用既有 SPDK 路线已验证的 segment 生命周期管理框架**（共享、心跳、故障隔离、副本清理），而不是为 NDS 另起一套平级 manager。具体设计见第 5 章。

### 2.3 目标

- **补齐 NPU 直连存储路径**：通过编译宏 `USE_NVMEOF_NDS` 在 transport 层引入与 GDS 平行的 NDS 分支，通过 `USE_NOF` 在 master 层启用 NoF segment 管理。
- **NPU HBM 与 NVMe-oF 直连**：通过 NDS 库实现 HBM ↔ NVMe-oF 的直接搬运，数据面不经过 host 内存中转。
- **零侵入既有路径**：不修改 GDS 分支，不引入 SPDK 依赖，不触动 host 侧 buffer 分配逻辑。
- **扩展 master 层 SSD segment 管理**：在既有 `NoFSegmentManager` 基础上补齐多 client 共享（`client_refs`）、探针注入（`NoFProbeFn`）、复用强制卸载链路，使 NDS 路径与既有 SPDK 路线共享同一套 segment 生命周期管理框架。
- **与现有 batch 提交模型对等**：NDS 提供与 GDS `cuFileBatchIOSetUp / cuFileBatchIOSubmit / cuFileBatchIOGetStatus` 对等的 batch 异步 IO API（`ndsBatchIOSetUp / ndsBatchIOSubmit / ndsBatchIOGetStatus`），`NdsDescPool` 封装 NDS batch 上下文管理与 handle 复用池，与 `CUFileDescPool` 的设计模式对齐。

### 2.4 与 SPDK 路线的定位差异

本提案与 SPDK 路线（#1940 / #2084）**面向不同硬件、互不替代**。NDS 路径由两个编译宏协同启用：`USE_NOF` 负责 master 层 NoF segment 管理基础设施，`USE_NVMEOF_NDS` 负责 transport 层的 NDS 直连数据路径。只定义 `USE_NOF` 时保持原有 SPDK 路径不变（store 层使用 SpdkWrapper）；同时定义 `USE_NOF` 与 `USE_NVMEOF_NDS` 时，master 层复用 NoF segment 管理，transport 层替换为 NDS 直连。在混合集群中，GPU 节点仅编译 `USE_NOF`，NPU 节点同时编译 `USE_NOF` 与 `USE_NVMEOF_NDS`，共享同一套 master 和 segment 池。下表列出二者在定位上的差异：

| 维度             | SPDK 路线 (#1940 / #2084)                                                    | 本提案 (NDS 路线)                                         |
| ---------------- | ---------------------------------------------------------------------------- | --------------------------------------------------------- |
| 适用硬件         | NVIDIA GPU（依赖 CUDA）                                                      | 昇腾 NPU（基于 NDS 用户态库）                             |
| 存储后端接入方式 | SPDK 用户态驱动直连 NVMe-oF target                                           | 复用 NDS C API                                            |
| Host 内存        | 通过 SPDK 分配 host 侧 DMA buffer                                            | 不涉及，NPU HBM 直接经 NDS 落盘                           |
| Segment 管理     | 独立的`NoFSegmentManager`、心跳、注册脚本                                  | 在既有 NoFSegmentManager 基础上扩展共享/心跳/故障隔离     |
| Transport 改动   | 新增 store 层模块                                                            | transport 层新增平行分支 + master 层扩展 NoF segment 管理 |
| 与 GDS 关系      | 替代/并行于既有 transport                                                    | 与 GDS 完全平行，编译宏切换                               |
| 集群级共存       | GPU 节点编译 `USE_NOF`，NPU 节点编译 `USE_NOF + USE_NVMEOF_NDS`，共享 master 与 segment 池 | 同左                                                      |

在实现路径上，本提案不引入 SPDK 依赖、不涉及 host 侧 buffer 分配、复用既有 `NoFSegmentManager` 框架，与 SPDK 路线在代码层面完全解耦。两条路径的共存策略详见第 6.5 节。

## 3. 总体架构

### 3.1 模块结构图

```mermaid
graph TB
    subgraph Store["Mooncake Store (Master 层)"]
        MS[MasterService]
        NSM[NoFSegmentManager<br/>本提案扩展]
        MS --> NSM
        NSM -->|挂载/卸载/心跳<br/>地址偏移寻址| SEG[(SSD Segment<br/>块设备)]
    end

    subgraph Existing["既有 NVMe-oF Transport (GDS 路径)"]
        TE[TransferEngine]
        NVT[NVMeoFTransport]
        CFC[CuFileContext]
        CDP[CUFileDescPool]
        TE --> NVT
        NVT -->|USE_NVMEOF_NDS=OFF| CFC
        NVT -->|USE_NVMEOF_NDS=OFF| CDP
        CFC -->|cuFileHandleRegister| GDS[(NVIDIA GDS Lib)]
        CDP -->|cuFileBatchIOSubmit| GDS
    end

    subgraph Proposed["本提案新增 (NDS 路径)"]
        NDS_CTX[NdsFileContext]
        NDS_BATCH[NdsDescPool]
        NDS_API[nds.h C API]
        NVT -->|USE_NVMEOF_NDS=ON| NDS_CTX
        NVT -->|USE_NVMEOF_NDS=ON| NDS_BATCH
        NDS_CTX --> NDS_API
        NDS_BATCH --> NDS_API
        NDS_API --> NDSL[(libnds.so / NPU Direct Storage)]
    end

    SEG -. fd + file_offset .-> NVT
    NDSL -.HBM 直连.-> SEG
    GDS -.GPU 显存直连.-> SEG
```

本提案覆盖两个层面：transport 层的 NDS 分支（替代 GDS API 完成数据搬运）与 master 层的 SSD segment 管理（在既有 `NoFSegmentManager` 基础上扩展共享/心跳/故障隔离能力）。SSD segment 基于普通块设备地址偏移寻址（`base + offset`），由 master 通过 fd + offset 向 transport 层下发任务。

### 3.2 编译期分支

```mermaid
flowchart LR
    SRC[nvmeof_transport.cpp] --> IS_NOF{USE_NOF?}
    IS_NOF -->|否| INC_GDS[cufile_context.h<br/>cufile_desc_pool.h]
    IS_NOF -->|是| IS_NDS{USE_NVMEOF_NDS?}
    IS_NDS -->|否| INC_SPDK[spdk_wrapper.h<br/>spdk_nof_worker_pool.h]
    IS_NDS -->|是| INC_NDS[nds_context.h<br/>nds_desc_pool.h<br/>nds.h]
    INC_GDS --> LINK_GDS[libcufile.so + CUDA]
    INC_SPDK --> LINK_SPDK[SPDK 静态库]
    INC_NDS --> LINK_NDS[libnds.so + ascendcl]
```

`CMakeLists.txt` 中通过 `USE_NOF` 与 `USE_NVMEOF_NDS` 选项组合控制：

- `USE_NOF=ON, USE_NVMEOF_NDS=ON`：编译 `nvmeof_transport.cpp` 的 NDS 分支 + `nds_desc_pool.cpp`，链接 `libnds.so` 与 `ascendcl`；可选 `NDS_USE_STUB=ON` 编译 `nds_stub.cpp`（无需真实 NDS 硬件即可测试）；
- `USE_NOF=ON, USE_NVMEOF_NDS=OFF`（默认）：编译 store 层 SPDK 路径（`SpdkWrapper` + `SpdkNofWorkerPool`），链接 SPDK 静态库；
- `USE_NOF=OFF, USE_NVMEOF_NDS=OFF`（默认）：编译 GDS 分支（`cufile_context.cpp`、`cufile_desc_pool.cpp`），链接 `libcufile.so`。

## 4. Transport 层设计

### 4.1 类图

#### 4.1.1 NVMeoFTransport 与两条后端分支

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

    class NdsDescPool {
        -ndsBatchHandle_t handle_pool_[]
        -NdsBatchDesc* descs_[256]
        +allocNdsDesc(batch_size) int
        +pushParams(idx, params, slice) int
        +submitBatch(idx) int
        +getTransferStatus(idx, slice_id) ndsBatchIOEvents_t
        +getSliceNum(idx) int
        +freeNdsDesc(idx) int
        +getDesc(idx) NdsBatchDesc*
    }

    class NdsFileContext {
        -nds_Handle handle_
        -int fd_
        -int32_t device_id_
        +NdsFileContext(filename, device_id)
        +getHandle() nds_Handle
        +getDeviceId() int32_t
    }

    Transport <|.. NVMeoFTransport
    NVMeoFTransport o-- CuFileContext : USE_NVMEOF_NDS=OFF
    NVMeoFTransport o-- CUFileDescPool : USE_NVMEOF_NDS=OFF
    NVMeoFTransport o-- NdsFileContext : USE_NVMEOF_NDS=ON
    NVMeoFTransport o-- NdsDescPool : USE_NVMEOF_NDS=ON
```

#### 4.1.2 NDS C API 抽象（`nds.h`）

```mermaid
classDiagram
    class nds_file_ctx_t {
        +int fd
        +int dummy
    }
    class nds_Handle {
        <<typedef>>
    }
    class ndsBatchHandle_t {
        <<typedef>>
    }
    class nds_segment_info {
        +uint8_t eid[16]
        +uint32_t uasid
        +uint32_t jetty_id
        +uint32_t token_id
    }
    class ndsBatchIOParams_t {
        +ndsBatchMode_t mode
        +ndsBatchIOParamBatch_t u.batch
        +nds_Handle nds_handle
        +ndsBatchIOOp_t opcode
        +void* cookie
        +int32_t device_id
    }
    class ndsBatchIOEvents_t {
        +void* cookie
        +ndsBatchIOStatus_t status
        +ssize_t ret
        +int error
    }
    class NDSAPI {
        <<C functions>>
        +nds_init(device_id) int
        +nds_deinit(device_id) void
        +nds_buf_register(device_id, buf, len) int
        +nds_buf_deregister(device_id, buf) int
        +nds_get_segment_info(buf, out) int
        +nds_file_register(fd) nds_Handle
        +nds_file_deregister(fd) int
        +nds_read(handle, device_id, buf, nbyte, offset) ssize_t
        +nds_write(handle, device_id, buf, nbyte, offset) ssize_t
        +nds_read_imported(handle, segment, buf, nbyte, offset) ssize_t
        +nds_write_imported(handle, segment, buf, nbyte, offset) ssize_t
        +ndsBatchIOSetUp(handle, max_nr) int
        +ndsBatchIOSubmit(handle, nr, params, flags) int
        +ndsBatchIOGetStatus(handle, min_nr, nr, events, timeout) int
        +ndsBatchIOReset(handle) int
        +ndsBatchIODestroy(handle) int
    }
    nds_file_ctx_t ..> nds_Handle
    ndsBatchIOParams_t ..> nds_Handle
    NDSAPI ..> nds_Handle
    NDSAPI ..> ndsBatchHandle_t
```

### 4.2 关键流程图

#### 4.2.1 初始化与内存注册流程

```mermaid
sequenceDiagram
    participant App as Application
    participant TE as TransferEngine
    participant NVT as NVMeoFTransport
    participant NDS as NDS Lib

    App->>TE: install / registerLocalMemory(addr, len)
    TE->>NVT: registerLocalMemory(addr, len, ...)
    Note over NVT: 构造时已通过 aclrtGetDevice<br/>或 MC_NDS_DEVICE_ID 获取 device_id
    alt USE_NVMEOF_NDS=ON 且未初始化
        NVT->>NDS: nds_init(device_id)
        NDS-->>NVT: 0
        NVT->>NVT: aclrtSetDevice(device_id)
    end
    NVT->>NDS: nds_buf_register(device_id, addr, len)
    NDS-->>NVT: 0
    NVT-->>TE: 0
```

#### 4.2.2 Batch 提交与 Slice 执行流程（NDS 路径）

```mermaid
sequenceDiagram
    participant App as Application
    participant NVT as NVMeoFTransport
    participant NdsPool as NdsDescPool
    participant FileCtx as NdsFileContext
    participant NDS as NDS Lib

    App->>NVT: submitTransfer(batch_id, entries)
    NVT->>NdsPool: allocNdsDesc(batch_size)
    loop 每个 TransferRequest
        NVT->>NVT: 计算 slice 与 file_offset
        NVT->>FileCtx: 查找/创建 NdsFileContext(file_path, device_id)
        FileCtx->>NDS: nds_file_register(fd)
        NDS-->>FileCtx: nds_Handle
        Note over NVT,NdsPool: addSliceToNdsBatch 将 Slice* 作为 cookie<br/>NdsDescPool::pushParams 关联 params 与 slice
        NVT->>NdsPool: pushParams(idx, ndsBatchIOParams_t, slice)
    end
    NdsPool->>NDS: ndsBatchIOSubmit(handle, nr, params, 0)
    NDS-->>NdsPool: 0 (提交成功)
    NVT-->>App: Status::OK()

    Note over NdsPool,NDS: 异步执行：NPU HBM ↔ NVMe-oF target DMA
```

#### 4.2.3 状态查询流程

```mermaid
flowchart TD
    Q["getTransferStatus(batch_id, task_id)"] --> CT{"USE_NVMEOF_NDS?"}
    CT -->|ON| NDSGET["ndsBatchIOGetStatus<br/>返回 ndsBatchIOEvents_t"]
    NDSGET --> UPDATE["通过 cookie (Slice指针) 更新<br/>Slice::markSuccess / markFailed"]
    UPDATE --> MAP["解析 status / ret / error"]
    MAP --> OUT["返回 status"]
    CT -->|OFF| EV["desc_pool_.getTransferStatus<br/>CUfileIOEvents_t"]
    EV --> MAP2["from_cufile_transfer_status"]
    MAP2 --> OUT2["返回 status"]
```

### 4.3 GDS 与 NDS 核心 API 对比

本节对照两条路径在 transport 层使用的关键 API，说明其在功能上的对应关系与差异。

#### 4.3.1 API 对照表

| 阶段         | GDS API (NVIDIA)                               | NDS API (Ascend)                                           | 说明                                                 |
| ------------ | ---------------------------------------------- | ---------------------------------------------------------- | ---------------------------------------------------- |
| 驱动初始化   | `cuFileDriverOpen()`                         | `nds_init(device_id)`                                    | NDS 显式接收`device_id`，与多卡场景对齐            |
| 驱动释放     | `cuFileDriverClose()`                        | `nds_deinit(device_id)`                                  | —                                                   |
| 设备内存注册 | `cuFileBufRegister(addr, len, flags)`        | `nds_buf_register(device_id, buf, len)`                  | NDS 同样以 HBM 地址为输入                            |
| 设备内存注销 | `cuFileBufDeregister(addr)`                  | `nds_buf_deregister(device_id, buf)`                     | —                                                   |
| 文件句柄注册 | `cuFileHandleRegister(&handle, &desc)`       | `nds_file_register(fd)`                                  | NDS 直接接收 fd，返回`nds_Handle`                  |
| 文件句柄注销 | `cuFileHandleDeregister(handle)`             | `nds_file_deregister(fd)`                                | NDS 以 fd 为索引                                     |
| 单次读       | `cuFileRead(handle, buf, len, offset, ...)`  | `nds_read(handle, device_id, buf, len, offset)`          | NDS 显式传入`device_id`                            |
| 单次写       | `cuFileWrite(handle, buf, len, offset, ...)` | `nds_write(handle, device_id, buf, len, offset)`         | —                                                   |
| 远程段导出   | (无对等 API)                                   | `nds_get_segment_info(buf, out)`                          | 导出已注册 HBM 段的远程访问元数据                     |
| 远程导入读   | (无对等 API)                                   | `nds_read_imported(handle, segment, buf, nbyte, offset)`  | 跨进程/跨节点读取远端已注册 HBM 段                   |
| 远程导入写   | (无对等 API)                                   | `nds_write_imported(handle, segment, buf, nbyte, offset)` | 跨进程/跨节点写入远端已注册 HBM 段                   |
| 批量提交     | `cuFileBatchIOSetUp / cuFileBatchIOSubmit`   | `ndsBatchIOSetUp / ndsBatchIOSubmit`                      | 两者均支持批量异步 IO，参数结构不同                   |
| 批量状态查询 | `cuFileBatchIOGetStatus`                     | `ndsBatchIOGetStatus`                                     | NDS 返回`ndsBatchIOEvents_t`，含 cookie/status/ret |
| 批量重置     | (需销毁重建)                                   | `ndsBatchIOReset`                                         | NDS 支持重置 batch 上下文以复用                       |
| 批量销毁     | `cuFileBatchIODestroy`                       | `ndsBatchIODestroy`                                       | —                                                   |

#### 4.3.2 设计差异说明

- **设备标识**：GDS 通过 CUDA context 隐式确定设备；NDS 在每个 API 显式传入 `device_id`，便于在多 NPU 卡场景下精确路由。
- **批量能力**：GDS 与 NDS 均提供完整的 batch 异步 IO API（`SetUp / Submit / GetStatus / Destroy`）。NDS 的 batch 参数通过 `ndsBatchIOParams_t` 描述（含 `mode`、`nds_handle`、`opcode`、`cookie`、`device_id`），与 GDS 的 `CUfileIOParams_t` 语义对等。NDS 额外提供 `ndsBatchIOReset` 用于复用 batch 上下文。
- **远程段访问**：NDS 提供 `nds_get_segment_info` / `nds_read_imported` / `nds_write_imported` 三个跨进程 API，允许一个进程导出已注册的 HBM 段元数据，另一进程通过导入该元数据直接对远端 HBM 发起 DMA 读写，GDS 无对等功能。
- **句柄语义**：GDS 的 `CUfileHandle_t` 是不透明指针；NDS 的 `nds_Handle` 同样为不透明指针，但其生命周期与 fd 绑定，注销时以 fd 为索引。

## 5. Master 层 SSD Segment 管理

本节描述 master 层针对普通块设备地址偏移寻址 SSD 盘的 segment 管理设计，核心能力包括多 client 共享 segment、心跳健康检查、故障隔离。本提案面向基于块设备地址偏移寻址的普通 SSD 盘，segment 内空间由既有 `OffsetBufferAllocator` 管理，与现有 `NoFSegmentManager` 的寻址模型保持一致。

### 5.1 设计要点

| 要点                   | 说明                                                                                                                                           |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| Segment 寻址模型       | 普通块设备地址偏移寻址（`base + offset`），复用既有 `OffsetBufferAllocator`                                                                |
| 多 client 共享 segment | 一块物理 SSD 可被多个 client 同时挂载使用，通过`client_refs` 引用计数管理生命周期；重复挂载同一 `device_name` 仅增加引用，不创建新 segment |
| 卸载语义               | `Unmount(device_name, client_id)`：引用计数减一；引用归零才真正销毁 segment。与既有 `NoFSegmentManager` 的 1:1 挂载语义不同                |
| 心跳健康检查           | 后台线程定期探测每个 OK 状态的 SSD segment，连续失败超阈值（`interval × threshold`）触发强制卸载，使新写入自动流向健康 segment              |
| 故障处理               | 设备不可达时直接`ForceUnmountSegment` 强制卸载（绕过 `client_refs`），**不走 Drain 路径**——故障设备无法读取也就无法迁移数据        |
| Client 透明            | Client 每次操作向 master 请求 descriptor，故障段的 descriptor 会被`ClearInvalidHandles` 清理；Client 无需显式感知卸载                        |

### 5.2 类图

```mermaid
classDiagram
    direction TB

    class MountedNoFSegment {
        +NoFSegment segment
        +SegmentStatus status
        +size_t remaining_size
        +set~UUID~ client_refs
    }

    class NoFSegmentManagerExt {
        -shared_mutex segment_mutex_
        -map~string,MountedNoFSegment~ mounted_segments_
        -map~UUID,set~string~~ client_segments_
        -mutex heartbeat_mutex_
        -unordered_map~string,HeartbeatState~ heartbeat_states_
        -thread heartbeat_thread_
        -NoFProbeFn probe_fn_
        +getNoFSegmentAccess() ScopedNoFSegmentAccess
        +allocate(size, count, preferred, excluded, strategy) vector~AllocResult~
    }

    class ScopedNoFSegmentAccess {
        +MountSegment(NoFSegment, client_id) ErrorCode
        +PrepareUnmountSegment(device_name, client_id, &dec_capacity) ErrorCode
        +CommitUnmountSegment(device_name, dec_capacity) ErrorCode
        +ForceUnmountSegment(device_name, &dec_capacity) ErrorCode
        +Allocate(...) vector~AllocResult~
        +Deallocate(device_name, offset, size) void
        +GetRefCount(device_name) size_t
    }

    class HeartbeatState {
        +string device_name
        +time_point next_probe_at
        +time_point last_success_at
        +uint32 consecutive_failures
        +string last_error_reason
    }

    class NoFProbeFn {
        <<typedef>>
        +operator()(device_name, timeout_ms, error_reason*) bool
    }

    NoFSegmentManagerExt *-- MountedNoFSegment
    NoFSegmentManagerExt --> ScopedNoFSegmentAccess
    NoFSegmentManagerExt o-- HeartbeatState : heartbeat_states_
    NoFSegmentManagerExt ..> NoFProbeFn : probe_fn_
```

### 5.3 挂载与卸载流程

```mermaid
sequenceDiagram
    participant Client
    participant MS as MasterService
    participant SA as ScopedNoFSegmentAccess

    Client->>MS: MountNoFSegment(NoFSegment, client_id)
    MS->>SA: getNoFSegmentAccess().MountSegment()
    Note over SA: 获取 segment_mutex_ 写锁

    alt device_name 已存在（重复挂载）
        SA->>SA: client_refs.insert(client_id)
        SA->>SA: remaining_size 不变
        SA-->>MS: OK
    else device_name 不存在（首次挂载）
        SA->>SA: 创建 MountedNoFSegment<br/>client_refs={client_id}
        SA->>SA: 创建 OffsetBufferAllocator
        SA->>SA: 更新 Metrics
        SA-->>MS: OK
    end
    MS-->>Client: OK

    Note over Client,SA: 卸载流程
    Client->>MS: UnmountNoFSegment(device_name, client_id)
    MS->>SA: PrepareUnmountSegment(device_name, client_id, &dec)
    SA->>SA: client_refs.erase(client_id)
    alt client_refs 不为空
        SA-->>MS: OK（仅减引用）
    else client_refs 为空
        SA->>SA: status = UNMOUNTING
        SA->>SA: dec = segment.size
        MS->>SA: CommitUnmountSegment(device_name, dec)
        SA->>SA: mounted_segments_.erase(device_name)
    end
    MS-->>Client: OK
```

### 5.4 心跳与健康检查

后台心跳线程每 100ms 醒来，对所有 OK 状态的 SSD segment 进行轮询探测。`NoFProbeFn` 以函数注入方式提供（master 不依赖具体驱动实现），探针实现由 transport 层提供——在 NDS 路径下可基于一次轻量 `nds_read` 完成。NDS 路径下的具体探针替换方案见第 6.4 节。

```mermaid
flowchart TD
    A[HeartbeatThreadFunc 每 100ms 醒来] --> B[同步 heartbeat_states_ 表<br/>新增段加入/已卸载段移除]
    B --> C[筛选 next_probe_at <= now 的段]
    C --> D{有待探测段?}
    D -->|否| F[sleep 100ms]
    D -->|是| E[对每段调用 ProbeFn]
    E --> G{探测结果}
    G -->|成功| H[consecutive_failures=0<br/>更新 last_success_at]
    G -->|失败| I[consecutive_failures++]
    I --> J{now - last_success_at >= alive_timeout?}
    J -->|否| K[记录失败，等待下次]
    J -->|是| L[HandleFailure → ForceUnmountSegment]
    H --> M[更新 next_probe_at = now + interval]
    K --> M
    L --> M
    M --> F
```

`alive_timeout = interval × threshold`（如默认 10s × 3 = 30s）。

### 5.5 故障处理与强制卸载

故障设备不可达，**不走 Drain 路径**（Drain 要求源段可读以迁移数据）。直接 `ForceUnmountSegment` 强制卸载：绕过 `client_refs` 检查、清空引用集合、清理 `client_segments_` 中所有引用、置 `UNMOUNTING`，随后通过 `ClearInvalidHandles` 遍历 `ObjectMetadata` 删除匹配 `device_name` 的副本（对象仍有其他健康副本则降级存活，否则删除 key），最终 `CommitUnmountSegment` 移除 segment 并扣减容量指标。

```mermaid
flowchart TD
    A[HandleFailure device_name] --> B[ForceUnmountSegment]
    B --> B1[绕过 client_refs 检查]
    B1 --> B2[清空 client_refs]
    B2 --> B3[清理 client_segments_ 中所有引用]
    B3 --> B4[status = UNMOUNTING]
    B4 --> C[ClearInvalidHandles]
    C --> C1[遍历 ObjectMetadata]
    C1 --> C2[删除 device_name 匹配的副本]
    C2 --> C3{对象还有其他有效副本?}
    C3 -->|是| C4[保留对象，降级存活]
    C3 -->|否| C5[删除整个 key 数据丢失]
    C4 --> D[CommitUnmountSegment]
    C5 --> D
    D --> D1[mounted_segments_.erase]
    D1 --> D2[扣减容量指标]
    D2 --> E[清理 heartbeat_states_ 条目]
```

数据丢失是硬件故障的必然结果，系统能做的是：新写入自动分配到健康段（`Allocate()` 跳过非 OK 段）、有冗余的对象降级存活、无冗余的对象返回 `OBJECT_NOT_FOUND`。Client 无需显式感知卸载，通过现有 `GetReplicaList` 机制自然切换到健康副本。

## 6. NDS Transport 接入 NoFSegment 的适配方案

本章描述 NDS 路径下的 `NVMeoFTransport` 如何接入 `NoFSegmentManager` 管理的 SSD segment，以及如何与既有 SPDK NoF 路径共存。

### 6.1 两条 NoF 数据路径的架构对比

当前仓库中存在两条独立的 NoF 数据路径，它们在 `replica.is_nof_replica()` 之前共享完全相同的上游链路（master 分配 → `NoFDescriptor` → `transport_endpoint_` + `buffer_address_`），仅在 `TransferSubmitter::submit()` 的 NoF 分支内部通过编译宏分流到不同的传输实现：

```mermaid
graph TB
    subgraph Shared["上游共享链路（两条路径完全一致）"]
        MASTER[MasterService::PutStart<br/>从 NoFSegmentManager 分配]
        DESC[NoFDescriptor<br/>transport_endpoint_ + buffer_address_]
        CLIENT[Client::Put/Get<br/>获取 Replica::Descriptor]
        MASTER --> DESC --> CLIENT
    end

    subgraph Fork["编译期分流（TransferSubmitter::submit）"]
        FORK{replica.is_nof_replica<br/>编译宏选择}
        FORK -->|"USE_NOF（无 USE_NVMEOF_NDS）"| SPDK_PATH[SpdkWrapper + SpdkNofWorkerPool<br/>SPDK 用户态驱动直连 target]
        FORK -->|"USE_NOF + USE_NVMEOF_NDS"| NDS_PATH[TransferEngine → NVMeoFTransport<br/>NDS 库直连 target]
    end

    CLIENT --> FORK
    SPDK_PATH --> TARGET[远端 NVMe-oF Target]
    NDS_PATH --> TARGET
```

两条路径的关键差异：

| 维度         | SPDK NoF 路径                                                  | NDS NoF 路径                                                                   |
| ------------ | -------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| 编译宏       | `USE_NOF`                                                    | `USE_NOF + USE_NVMEOF_NDS`                                                                   |
| 传输层位置   | store 层内`SpdkWrapper` + `SpdkNofWorkerPool`              | transfer-engine 层`NVMeoFTransport`（NDS 分支）                              |
| 数据路径     | host 内存 → SPDK → 网络 → target                            | NPU HBM → NDS → PCIe → target                                               |
| host 内存    | 需要`spdk_zmalloc` 分配 DMA buffer                           | 不涉及                                                                         |
| segment 寻址 | `transport_endpoint_` 作为 NVMe-oF transport string 直接连接 | `transport_endpoint_` 作为 segment name 传给 `TransferEngine::openSegment` |
| 流控         | `SpdkNofQos`（per-segment inflight 限制）                    | 复用 NDS batch API 的内置异步能力，`NdsDescPool` 与 `CUFileDescPool` 模式对齐                  |

### 6.2 Store 层路由扩展

当前 `TransferSubmitter::submit()` 中 NoF 副本的处理分支仅支持 `USE_NOF`，适配方案是在该分支中增加 `USE_NVMEOF_NDS` 的备选路径：

```mermaid
flowchart TD
    A[TransferSubmitter::submit<br/>replica, slices, op_code, ptr, size] --> B{replica 类型?}
    B -->|memory| M[submitMemoryReadOperation<br/>或 submitMemcpyOperation / submitTransferEngineOperation]
    B -->|nof_replica| N{编译宏}
    B -->|disk| D[submitFileReadOperation]
    N -->|"USE_NOF（无 USE_NVMEOF_NDS）"| NOF_SPDK[submitSpdkNofOperation<br/>handle → SpdkWrapper::OpenNofSegment<br/>→ SpdkNofWorkerPool::submitTask]
    N -->|"USE_NOF + USE_NVMEOF_NDS"| NOF_NDS[submitNdsNofOperation<br/>handle → engine_.openSegment<br/>→ submitTransfer → NVMeoFTransport]
    N -->|均未定义| ERR[LOG ERROR + 返回 nullopt]
```

新增的 `submitNdsNofOperation` 核心逻辑：

| 步骤 | 操作                                                | 说明                                                                                                 |
| ---- | --------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| 1    | `engine_.openSegment(handle.transport_endpoint_)` | 以`transport_endpoint_` 作为 segment name 打开 segment，获取 `SegmentHandle`                     |
| 2    | 构造`TransferRequest`                             | `target_id = seg`，`target_offset = handle.buffer_address_`，`source = ptr`，`length = size` |
| 3    | `submitTransfer({request})`                       | 通过 TransferEngine 提交，由`NVMeoFTransport`（NDS 分支）执行                                      |

此方案复用现有的 `submitTransfer` 路径（与 `submitTransferEngineOperation` 一致），无需在 store 层新增 worker 线程池。流控方面，NDS 的 `ndsBatchIOSubmit` 提供与 GDS `cuFileBatchIOSubmit` 对等的批量异步提交能力，由 `NdsDescPool` 封装（与 `CUFileDescPool` 模式对齐）。

### 6.3 Segment 元数据注册

NDS 路径下，`NVMeoFTransport` 通过 `TransferMetadata::getSegmentDescByID(target_id)` 获取 segment 描述符，从中读取 `nvmeof_buffers` 中的本地块设备路径来执行 IO。因此，当 master 挂载 NoF segment 之后，client 侧需要将 segment 元数据注册到本地 TransferEngine 的 metadata 中。

注册流程在 **client 初始化 TransferEngine 时自动进行**：client 启动后，通过 RPC 从 master 拉取当前所有已挂载的 NoF segment 列表，逐个注册到本地的 `TransferMetadata` 中。后续 master 新挂载 segment 时，通过已有的心跳/状态同步机制使 client 增量更新。

```mermaid
sequenceDiagram
    participant Client as Client 进程
    participant TE as TransferEngine
    participant TM as TransferMetadata
    participant MS as MasterService
    participant NSM as NoFSegmentManager

    Note over Client,NSM: Client 初始化阶段
    Client->>TE: 初始化 TransferEngine
    TE->>MS: 拉取已挂载的 NoF segment 列表
    MS->>NSM: 查询 mounted_segments_
    NSM-->>MS: [{te_endpoint, device_path, size}, ...]
    MS-->>TE: segment 列表

    loop 每个 segment
        TE->>TM: addSegmentDesc(SegmentDesc{<br/>  name=te_endpoint,<br/>  protocol="nvmeof",<br/>  nvmeof_buffers=[{device_path, size}]})
        TM-->>TE: OK
    end

    TE-->>Client: 初始化完成
```

`SegmentDesc` 各字段的填充规则：

| 字段                                | 来源                                   | 说明                                                                       |
| ----------------------------------- | -------------------------------------- | -------------------------------------------------------------------------- |
| `name`                            | `NoFSegment.te_endpoint`             | 与`transport_endpoint_` 一致，作为 segment 的唯一标识                    |
| `protocol`                        | 固定`"nvmeof"`                       | 使`TransferSubmitter::submitTransfer` 能识别并路由到 `NVMeoFTransport` |
| `nvmeof_buffers[].local_path_map` | `NoFSegment.device_path`（新增字段） | 本地 NVMe 块设备路径，如`/dev/nvme0n1`                                   |
| `nvmeof_buffers[].length`         | `NoFSegment.size`                    | segment 总大小                                                             |

`device_path` 是 `NoFSegment` 结构体中新增的字段，由 SSD 注册脚本在挂载时填充。该字段仅在 NDS 路径下使用，SPDK 路径不需要（SPDK 通过 `te_endpoint` 中的 transport string 直接连接远端 target）。

### 6.4 心跳探测接口替换

当前 `MasterService::ProbeNoFSegment` 被 `#ifdef USE_NOF` 守卫，默认探针绑定 `SpdkWrapper::ProbeNofSegment`。当同时定义 `USE_NOF` 与 `USE_NVMEOF_NDS` 时，需要替换为 NDS 探针。

```mermaid
flowchart TD
    subgraph Init["MasterService 构造"]
        A{编译宏} -->|"USE_NOF（无 USE_NVMEOF_NDS）"| B[默认绑定 SpdkWrapper::ProbeNofSegment]
        A -->|"USE_NOF + USE_NVMEOF_NDS"| C[默认绑定 NDS 探针<br/>基于 nds_read 的轻量读操作]
        A -->|均未定义| D[ProbeNoFSegment 直接返回 false]
    end

    subgraph Runtime["心跳线程调用"]
        E[ProbeNoFSegment<br/>te_endpoint, timeout_ms, error_reason]
        E --> F[持锁拷贝 nof_probe_fn_]
        F --> G[调用 probe_fn]
    end

    B --> F
    C --> F
```

NDS 探针实现要点：

| 项目               | 说明                                                                                                |
| ------------------ | --------------------------------------------------------------------------------------------------- |
| 探针类型           | `NoFProbeFn`，签名 `bool(const string& te_endpoint, uint32_t timeout_ms, string* error_reason)` |
| 实现方式           | 基于`nds_read` 执行一次轻量读操作（LBA 0，1 block），验证 segment 可访问性                        |
| 超时控制           | 由心跳线程传入`nof_heartbeat_probe_timeout_ms_`，探针内部通过轮询 + 超时判断实现                  |
| 测试注入           | 保留`SetNoFProbeFnForTesting` 接口，允许测试代码注入 mock 探针                                    |
| 与 SPDK 探针的关系 | 两者通过`NoFProbeFn` 统一接口隔离，master 层心跳逻辑不感知探针的具体实现                          |

### 6.5 与 SPDK NoF 路径的共存策略

两条路径通过以下机制实现互不干扰的共存：

**编译期隔离**：

| 编译宏                                      | 编译范围                                          | 链接依赖      |
| ------------------------------------------- | ------------------------------------------------- | ------------- |
| `USE_NOF=ON, USE_NVMEOF_NDS=OFF`          | store 层：`SpdkWrapper` + `SpdkNofWorkerPool` | SPDK 静态库   |
| `USE_NOF=ON, USE_NVMEOF_NDS=ON`           | transport 层：`NVMeoFTransport` NDS 分支        | `libnds.so` |

`USE_NOF` 控制 master/store 层的 NoF segment 管理基础设施，`USE_NVMEOF_NDS` 控制 transport 层是否使用 NDS 直连（否则使用 GDS）。两者作用在不同层次，可独立组合。

**运行时隔离**：

| 路径 | 传输方式                                                                                 | 探针实现                         |
| ---- | ---------------------------------------------------------------------------------------- | -------------------------------- |
| SPDK | `submitSpdkNofOperation`：解析 NVMe-oF transport string，SPDK 用户态驱动连接           | `SpdkWrapper::ProbeNofSegment` |
| NDS  | `submitNdsNofOperation`：`transport_endpoint_` 作为 segment name 传给 TransferEngine | 基于`nds_read` 的轻量探针      |

**Master 层共享**：

两条路径共享同一个 `NoFSegmentManager`，包括 segment 挂载/卸载、心跳健康检查、故障隔离、多副本清理。master 只管理 segment 元数据和分配空间，不关心 client 侧用什么传输方式。

**叠加部署**：

在混合集群中，GPU 节点编译 `USE_NOF=ON, USE_NVMEOF_NDS=OFF` 走 SPDK 路径，NPU 节点编译 `USE_NOF=ON, USE_NVMEOF_NDS=ON` 走 NDS 路径，两者共享同一个 master 和同一套 NoF segment 池。

### 6.6 端到端数据流

本节描述一次完整的 read/write 请求在 NDS NoF 路径下的端到端数据流向，用于说明各组件的职责边界与"直连"语义。

**参与角色**

- **NPU (Ascend)**：物理 NPU 卡，其 HBM 是 KV Cache 数据的实际驻留位置。HBM 缓冲区在初始化阶段通过 `nds_buf_register` 注册到 NDS 库，NDS 后续可直接对该地址发起 DMA。
- **Host Process**：运行 `mooncake-transfer-engine` 的用户态进程，包含三个组件：
  - `NVMeoFTransport`：负责将上层 `TransferRequest` 分解为若干 `Slice`，每个 Slice 描述一段 (HBM 地址, 文件偏移, 长度) 的映射；
  - `NdsDescPool`：封装 NDS batch 异步 IO 上下文（`ndsBatchIOSetUp / ndsBatchIOSubmit / ndsBatchIOGetStatus`），维护 `ndsBatchHandle_t` 复用池与 `NdsBatchDesc` 数组，负责将批量 Slice 转化为 NDS batch 提交调用；
  - `NdsFileContext`：持有目标文件 `fd` 与 NDS 句柄 `nds_Handle`，是 `NdsDescPool` 构造 `ndsBatchIOParams_t` 时必须的上下文。
- **Storage Backend**：远端 NVMe-oF target / SSD Pool，对 host 表现为一个块设备文件路径。

**数据流要点**

1. **控制流经 host，数据流不经过 host**：`NVMeoFTransport` 与 `NdsDescPool` 都运行在 host CPU 上，但它们只负责"任务分解"和"调用 NDS batch API"；真正搬运数据的 DMA 由 NDS 库在 NPU HBM 与 NVMe-oF target 之间直接完成，**不经过 host 内存中转**，从而实现与 GDS 对称的 zero-copy 语义。
2. **文件句柄由 host 打开**：`NdsFileContext` 通过 `open(filename, O_RDWR)` 获得宿主机视角的 `fd`，再经 `nds_file_register(fd)` 转换为 NDS 句柄。这一步是控制面操作，不涉及数据拷贝。
3. **NDS batch API 的执行模式**：`NdsDescPool` 将一组 Slice 封装为 `ndsBatchIOParams_t` 数组后调用 `ndsBatchIOSubmit` 批量提交，随后通过 `ndsBatchIOGetStatus` 轮询各 Slice 的完成状态（`ndsBatchIOEvents_t`）。为减少 `ndsBatchIOSetUp/Destroy` 的开销，`NdsDescPool` 内部维护 `ndsBatchHandle_t` 复用池。该模式与 GDS 路径的 `CUFileDescPool` 完全对称。
4. **与 GDS 路径的对称性**：GDS 路径下 `cuFileBatchIOSubmit` 在 GPU 显存与 NVMe-oF target 间直接 DMA；NDS 路径下 `ndsBatchIOSubmit` 在 HBM 与 NVMe-oF target 间直接 DMA。两者都绕过 host DRAM，差别仅在底层库与目标设备。

**架构级数据流**

```mermaid
flowchart LR
    subgraph NPU["NPU (Ascend)"]
        HBM[HBM 显存<br/>nds_buf_register 注册]
    end
    subgraph Host["Host Process (控制面)"]
        NVT[NVMeoFTransport<br/>分解 TransferRequest → Slice]
        NdsBatch[NdsDescPool<br/>ndsBatchIOSubmit 批量提交]
        Ctx[NdsFileContext<br/>fd + nds_Handle]
    end
    subgraph Backend["Storage Backend (数据面)"]
        NVME[(NVMe-oF Target / SSD Pool)]
    end

    App[Application<br/>submitTransfer] --> NVT
    NVT -->|Slice| NdsBatch
    NVT -->|open + register| Ctx
    Ctx -->|提供 nds_Handle| NdsBatch
    Ctx -. open(fd) .-> NVME
    NdsBatch -->|调用 ndsBatchIOSubmit| NDS_API[NDS Lib<br/>驱动 DMA]
    HBM ==>|DMA 直传<br/>不经过 host DRAM| NVME
```

**端到端时序**

```mermaid
sequenceDiagram
    participant App as 上层应用
    participant RC as RealClient
    participant C as Client
    participant TS as TransferSubmitter
    participant TE as TransferEngine
    participant NVT as NVMeoFTransport<br/>(NDS 分支)
    participant NdsBatch as NdsDescPool
    participant NDS as libnds.so
    participant TGT as NVMe-oF Target

    App->>RC: put/get
    RC->>C: Put/Get
    C->>TS: submit(replica.is_nof_replica())
    Note over TS: USE_NOF + USE_NVMEOF_NDS 编译分支
    TS->>TE: openSegment(transport_endpoint_)
    TE-->>TS: SegmentHandle
    TS->>TE: submitTransfer({request})
    TE->>NVT: submitTransferTask(task_list)
    NVT->>NVT: 从 SegmentDesc.nvmeof_buffers<br/>解析 file_path + file_offset
    Note over NVT,NdsBatch: addSliceToNdsBatch 封装<br/>ndsBatchIOParams_t + Slice*
    NVT->>NdsBatch: allocNdsDesc + pushParams
    NdsBatch->>NDS: ndsBatchIOSubmit(handle, nr, params, 0)
    NDS->>TGT: NPU HBM ↔ NVMe-oF DMA
    TGT-->>NDS: 完成
    NDS-->>NdsBatch: ndsBatchIOGetStatus → events[]
    NdsBatch-->>NVT: Status
    NVT-->>TE: Status
    TE-->>TS: TransferFuture
    TS-->>C: future
    C-->>RC: OK
    RC-->>App: OK
```

## 7. 配置与可观测性

通过环境变量进行运行时配置（无需改代码）：

| 环境变量                    | 含义              | 默认值              |
| --------------------------- | ----------------- | ------------------- |
| `USE_NOF`（编译期）             | 启用 NoF 基础设施（master 层） | `OFF`             |
| `USE_NVMEOF_NDS`（编译期）     | 启用 NDS 传输分支（transport 层） | `OFF`             |
| `MC_NDS_DEVICE_ID`              | NPU device id                   | `-1`（从 aclrtGetDevice 获取）  |

master 层 SSD segment 的心跳参数（探测间隔、超时、失败阈值）通过 `MasterServiceConfig` 注入，默认值参见第 5.4 节。

状态查询在 NDS 路径下通过 `ndsBatchIOGetStatus` 获取 `ndsBatchIOEvents_t`（含 `status`、`ret`、`error`），与 GDS 路径基于 `CUfileIOEvents_t` 的查询语义保持一致（参见第 4.2.3 节）。

## 8. 与社区路线的协同

- 与 SPDK 路线互补（定位差异见第 2.4 节）：本提案在 transport 层引入 NPU 直连分支，并在 master 层扩展 NoF segment 的共享/心跳/故障隔离能力。SSD 注册脚本、运维工具等仍可由 SPDK 路线提供，二者可叠加使用。
- 不修改既有 GDS 分支：`CuFileContext`、`CUFileDescPool` 保持原样，存量用户编译/行为不变。

## 9. 后续工作

1. **合入 transport 层 NDS 分支**：将 `NdsFileContext`、`NdsDescPool` 及 `NVMeoFTransport` 中的 NDS 编译分支（第 4 章）作为独立 PR 合入社区 `mooncake-transfer-engine`；
2. 评估是否抽取公共 `NvmeOfFileContext` 抽象基类，统一 GDS/NDS 的句柄管理接口；
3. 评估 master 层 `ScopedNoFSegmentAccess` 与既有 `ScopedSegmentAccess` 是否抽取公共基类，统一引用计数与心跳接口；
4. 实现 `submitNdsNofOperation` 及 `TransferSubmitter` 中的 `USE_NOF + USE_NVMEOF_NDS` 路由分支（第 6.2 节）；
5. 在 `NoFSegment` 中增加 `device_path` 字段，并在 client 初始化 TransferEngine 时完成 `SegmentDesc` 的 metadata 自动注册（第 6.3 节）；
6. 在 `MasterService` 构造中为 `USE_NOF + USE_NVMEOF_NDS` 绑定基于 `nds_read` 的默认 `NoFProbeFn`，替换当前仅 `USE_NOF` 的探针绑定（第 6.4 节）；
7. 在 `NVMeoFTransport` 的 transport 层补齐 QoS 流控能力，使其达到与 SPDK 路径 `SpdkNofQos` 对等的水平。

## 10. 参考文献

- [#1940 SSD pool over NVMe-oF](https://github.com/kvcache-ai/Mooncake/issues/1940)
- [#2084 NVMe-oF SSD cache support](https://github.com/kvcache-ai/Mooncake/pull/2084)
- NVIDIA GDS: https://docs.nvidia.com/gpudirect-storage/
