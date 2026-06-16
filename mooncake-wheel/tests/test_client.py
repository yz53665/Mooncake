import os
import sys

# sys.path.insert(0, "/home/sxx/code/mycode/Mooncake/mooncake-wheel")
from mooncake.store import MooncakeDistributedStore # type: ignore

os.environ['MC_STORE_CLIENT_MIN_PORT'] = "14236"
os.environ['MC_STORE_CLIENT_MAX_PORT'] = "14236"
os.environ['MC_TE_ENDPOINT'] = "51.62.5.33:14236"

store = MooncakeDistributedStore()

store.setup(
    "51.62.5.33",
    "etcd://127.0.0.1:2379",
    512 * 1024 * 1024 * 1024,
    64  * 1024 * 1024 * 1024,
    "nvmeof",
    "",
    "51.62.5.33:50051"
)

input("enter to wait")

store.put("hello_key", b"hello, Mooncake Store!")

data = store.get("hello_key")  # OUTPUT: b"hello, Mooncake Store!"
print(data.decode()) 

input("enter to exit")

store.close()
