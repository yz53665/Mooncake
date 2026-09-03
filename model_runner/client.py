from mooncake.store import MooncakeDistributedStore, ReplicateConfig
import torch
import torch_npu

device = torch.npu.set_device(0)

# 1. Create store instance
store = MooncakeDistributedStore()

# 2. Setup with all required parameters
store.setup(
    "127.0.0.1",           # Your node's address
    "P2PHANDSHAKE",    # HTTP metadata server
    64*1024*1024,          # 512MB segment size
    64*1024*1024,          # 128MB local buffer
    "ascend",                             # Use TCP (RDMA for high performance)
    "",                            # Leave empty; Mooncake auto-picks RDMA devices when needed
    "127.0.0.1:50088",        # Master service
)

dim1 = 7
dim2 = 4
dim3 = 4
min_bytes = 4096  # 注册/传输的 buffer 大小不能小于 4096 字节

total_size = dim1 * dim2 * dim3

#tensor = torch.randint(0, 50, (dim1, dim2, dim3), dtype=torch.uint8).npu()
tensor = torch.randn(dim1, dim2, dim3).npu()
data_ptr = tensor.data_ptr()
total_bytes = tensor.element_size() * tensor.numel()

if total_bytes < min_bytes:
    raise ValueError(
        f"buffer 大小 {total_bytes} 字节小于最小要求 {min_bytes} 字节，"
        f"请增大 tensor 维度（当前元素数 {total_size}）"
    )
store.register_buffer(data_ptr, total_bytes)
store.register_nds_buffer(data_ptr, total_bytes)

target_tensor = torch.zeros_like(tensor).npu()
target_data_ptr = target_tensor.data_ptr()
store.register_buffer(target_data_ptr, total_bytes)
store.register_nds_buffer(target_data_ptr, total_bytes)

config = ReplicateConfig()
config.prefer_alloc_in_same_node = False
print("start put from multi buffers*************************************************")
store.batch_put_from(['test'], [data_ptr], [total_bytes], config)
print("start get into multi buffers*************************************************")
store.batch_get_into(['test'], [target_data_ptr], [total_bytes])
print(tensor)
print(target_tensor)

diff = torch.sum(tensor - target_tensor)

print("diff: ", diff)

if diff < 1e-5:
    print("PASS!")
else:
    print("FAILED!")