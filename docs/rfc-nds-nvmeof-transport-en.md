# RFC: Introduce an NDS (NPU Direct Storage) Branch in NVMe-oF Transport to Support Ascend NPU Direct-to-Storage Access

## 1. Introduction

This RFC proposes adding a **NDS (NPU Direct Storage) branch** parallel to the existing GDS (GPU Direct Storage) path inside `nvmeof_transport`. With this branch, inference/training workloads running on Ascend NPU (HBM) can directly access remote SSD pools over NVMe-oF, just as NVIDIA GPUs do via GDS, thereby extending the capacity ceiling of KV Cache.

This proposal is complementary to, but does not overlap with, two existing community efforts:

- [#1940 SSD pool over NVMe-oF (SPDK path)](https://github.com/kvcache-ai/Mooncake/issues/1940): routes the storage backend through an in-house SPDK wrapper on the host side, and reworks LMCache memory into huge pages + `cudaHostRegister` to achieve zero-copy.
- [#2084 SPDK integration supplement](https://github.com/kvcache-ai/Mooncake/pull/2084): builds on #1940 with SSD registration scripts, NoF heartbeat, multi-replica cleanup, and monitoring metrics.

Our approach differs from the two above in that we **keep the GDS abstraction in the transport layer and introduce NDS as a parallel backend**, without pulling in SPDK and without replacing the existing CUDA pinned-memory allocation logic. The existing GDS path remains untouched.

## 2. Background and Motivation

### 2.1 Current State

The current `mooncake-transfer-engine/src/transport/nvmeof_transport/` ships a reference NVMe-oF transport, but it hard-depends on NVIDIA GDS (`cufile.h`) and only works under NVIDIA GPU + CUDA environments:

- `CuFileContext` registers file handles via `cuFileHandleRegister`;
- `CUFileDescPool` performs batch submission via `cuFileBatchIOSetUp / cuFileBatchIOSubmit`;
- `NVMeoFTransport::registerLocalMemory` registers GPU memory via `cuFileBufRegister`.

This leads to the following issues:

1. **NPU (Ascend) users cannot use this transport**: `cufile.h` is unavailable in the Ascend environment, so compilation fails outright.
2. **Existing community solutions require substantial rework**: #1940 / #2084 introduce an SPDK wrapper, a NoF segment manager, heartbeat, and scripted registration; they also require changing LMCache pinned memory from `cudaHostAlloc` to SPDK huge pages + `cudaHostRegister`, which is intrusive to host-side memory management.
3. **No GDS-symmetric, minimally invasive NPU direct path exists**: With NDS (NPU Direct Storage — a user-space library from Ascend that plays the same role as GDS) already available, the vast majority of the transport-layer logic could be reused by simply swapping the underlying API.

### 2.2 Goals

- **Zero intrusion into the GDS path**: switch via the compile-time macro `USE_NDS`; existing NVIDIA + GDS users are unaffected.
- **NPU HBM ↔ NVMe-oF direct access**: via the NDS library (`nds_init / nds_buf_register / nds_read / nds_write`), data is moved directly between HBM and NVMe-oF, with no host-memory detour.
- **No SPDK dependency**: we avoid rewriting LMCache pinned memory and avoid introducing SPDK wrapper / NoF segment manager modules.
- **Decouple from the existing batch submission model**: NDS does not currently expose a batch API, so we adopt a thread-pool + task-queue model, leaving room to switch to a batch API later.

### 2.3 Comparison with #1940 / #2084

| Dimension | #1940 / #2084 (SPDK path) | This proposal (NDS path) |
| --- | --- | --- |
| Target hardware | NVIDIA GPU (still depends on CUDA) | Ascend NPU (via NDS user-space library) |
| Storage backend access | In-house `SpdkWrapper` + `SpdkNofWorkerPool` | Reuse NDS C API, no SPDK dependency |
| Host memory changes | LMCache pinned memory → SPDK huge page + `cudaHostRegister` | None; NPU HBM written to disk directly via NDS |
| Segment management | New `NoFSegmentManager`, heartbeat, registration scripts | Reuse existing `nvmeof_buffers` and `SegmentDesc` |
| Transport-layer changes | New store-layer modules | Only a new parallel branch inside `nvmeof_transport` |
| Relationship with GDS | Replaces / runs alongside existing transport | Fully parallel to GDS, switched by compile macro |
| Risk surface | Large (memory management + SPDK deployment) | Small (transport-internal branch only) |

## 3. Overall Architecture

### 3.1 Module Structure

```mermaid
graph TB
    subgraph Existing["Existing NVMe-oF Transport (GDS path)"]
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

    subgraph Proposed["Proposed (NDS path)"]
        NDS_CTX[NdsFileContext]
        NDS_API[nds.h C API]
        NVT -->|USE_NDS=ON| NDS_CTX
        NVT -->|USE_NDS=ON| TP[NdsWorkerThreadPool]
        NDS_CTX --> NDS_API
        TP --> NDS_API
        NDS_API --> NDSL[(libnds.so / NPU Direct Storage)]
    end

    NDSL -.HBM direct.-> NVME[(NVMe-oF Target / SSD Pool)]
    GDS -.GPU memory direct.-> NVME
```

### 3.2 Compile-time Branching

```mermaid
flowchart LR
    SRC[nvmeof_transport.cpp] --> IS_NDS{USE_NDS?}
    IS_NDS -->|Yes| INC1[nds_context.h<br/>nds.h]
    IS_NDS -->|No| INC2[cufile_context.h<br/>cufile_desc_pool.h]
    INC1 --> LINK1[libnds.so + ascendcl]
    INC2 --> LINK2[libcufile.so + CUDA]
```

`CMakeLists.txt` controls the switch via the `USE_NDS` option:

- `USE_NDS=ON`: compile only `nvmeof_transport.cpp`; link `libnds.so` and `ascendcl`.
- `USE_NDS=OFF` (default): additionally compile `cufile_context.cpp` and `cufile_desc_pool.cpp`; link GDS.

## 4. Class Diagrams

### 4.1 NVMeoFTransport and the Two Backend Branches

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

### 4.2 NDS C API Abstraction (`nds.h`)

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

## 5. Key Flow Diagrams

### 5.1 Initialization and Memory Registration

```mermaid
sequenceDiagram
    participant App as Application
    participant TE as TransferEngine
    participant NVT as NVMeoFTransport
    participant NDS as NDS Lib

    App->>TE: install / registerLocalMemory(addr, len)
    TE->>NVT: registerLocalMemory(addr, len, ...)
    alt USE_NDS=ON and not yet initialized
        NVT->>NDS: nds_init(device_id)
        NDS-->>NVT: 0
        NVT->>NVT: initializeNdsThreadPool()
    end
    NVT->>NDS: nds_buf_register(device_id, addr, len)
    NDS-->>NVT: 0
    NVT-->>TE: 0
```

### 5.2 Batch Submission and Slice Execution (NDS path)

```mermaid
sequenceDiagram
    participant App as Application
    participant NVT as NVMeoFTransport
    participant Pool as NdsWorkerThreadPool
    participant Ctx as NdsFileContext
    participant NDS as NDS Lib

    App->>NVT: submitTransfer(batch_id, entries)
    loop For each TransferRequest
        NVT->>NVT: compute slice and file_offset
        NVT->>Ctx: lookup/create NdsFileContext(file_path, device_id)
        Ctx->>NDS: nds_file_register(fd)
        NDS-->>Ctx: nds_Handle
        NVT->>NVT: addSliceToTask(...) to build Slice
        NVT->>Pool: submitNdsSlice(slice)
        Pool->>Pool: enqueue (mutex + condition_variable)
    end
    NVT-->>App: Status::OK()

    par Worker threads consume concurrently
        Pool->>Ctx: read nds_Handle
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

### 5.3 Status Query Flow

```mermaid
flowchart TD
    Q[getTransferStatus batch_id, task_id] --> CT{USE_NDS?}
    CT -->|ON| AGG[Aggregate task.success_slice_count<br/>task.failed_slice_count]
    AGG --> J1{success+failed == slice_count?}
    J1 -->|No| W[WAITING]
    J1 -->|Yes| J2{failed > 0?}
    J2 -->|Yes| F[FAILED]
    J2 -->|No| C[COMPLETED]
    CT -->|OFF| EV[desc_pool_->getTransferStatus<br/>CUfileIOEvents_t]
    EV --> MAP[from_cufile_transfer_status]
    MAP --> OUT[Return status]
```

### 5.4 End-to-End Data Flow (NDS path)

```mermaid
flowchart LR
    subgraph NPU["NPU (Ascend)"]
        HBM[HBM memory<br/>nds_buf_register]
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
    Ctx -. open file .-> NVME
    TP -. HBM direct transfer .-> NVME
```

## 6. File Structure

```mermaid
graph LR
    subgraph "include/transport/nvmeof_transport/"
        H1[nds.h<br/>NDS C API declaration]
        H2[nds_context.h<br/>NdsFileContext]
        H3[cufile_context.h<br/>existing GDS]
        H4[cufile_desc_pool.h<br/>existing GDS]
        H5[nvmeof_transport.h<br/>compile-time branch]
    end
    subgraph "src/transport/nvmeof_transport/"
        S1[nvmeof_transport.cpp<br/>dual-branch impl]
        S2[cufile_context.cpp<br/>existing GDS]
        S3[cufile_desc_pool.cpp<br/>existing GDS]
        S4[CMakeLists.txt<br/>USE_NDS switch]
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

## 7. Configuration and Observability

Runtime configuration via environment variables (no code changes required):

| Variable | Meaning | Default |
| --- | --- | --- |
| `USE_NDS` (compile-time) | Enable the NDS branch | `OFF` |
| `MC_NDS_DEVICE_ID` | NPU device id | `-1` (do not initialize) |
| `MC_NDS_THREAD_POOL_SIZE` | Number of NDS worker threads | `8` (range 1–64) |

On the NDS path, status queries directly aggregate `task.success_slice_count / failed_slice_count`, which is semantically consistent with the GDS path's `CUfileIOEvents_t`-based queries (see 5.3).

## 8. Coordination with Community Roadmap

- Complementary to #1940 / #2084: this proposal focuses on NPU direct access at the transport layer and does not touch the store-layer NoF segment management, heartbeat, or SSD registration scripts. Those capabilities can be provided by #1940 / #2084 and stacked with this work.
- No changes to the existing GDS branch: `CuFileContext` and `CUFileDescPool` remain as-is; existing users see no behavioral or compilation changes.
- No replacement of LMCache pinned memory: we avoid the `cudaHostAlloc` → SPDK huge page + `cudaHostRegister` rework from #1940, reducing cross-component coupling.

## 9. Future Work

1. Once NDS exposes a batch API, replace `NdsWorkerThreadPool` with a batch submission model symmetric to `CUFileDescPool`.
2. Introduce finer-grained `transferred_bytes` reporting at the `Slice` level to align with the event granularity of the GDS path.
3. Evaluate extracting a common `NvmeOfFileContext` abstract base class to unify the handle-management interface across GDS / NDS.

## 10. References

- [#1940 SSD pool over NVMe-oF (SPDK)](https://github.com/kvcache-ai/Mooncake/issues/1940)
- [#2084 NVMe-oF SSD cache support (SPDK supplement)](https://github.com/kvcache-ai/Mooncake/pull/2084)
- NVIDIA GDS: https://docs.nvidia.com/gpudirect-storage/
