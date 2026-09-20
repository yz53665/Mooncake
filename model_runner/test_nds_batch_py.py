#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NDS 批量多缓冲接口测试（Python 层）

测试对象：
  - store.batch_put_from_multi_buffers(keys, all_buffer_ptrs, all_sizes, config)
  - store.batch_get_into_multi_buffers(keys, all_buffer_ptrs, all_sizes, prefer_alloc_in_same_node)

功能：
  - key 数量、每个 key 的 buffer 数量、每块 buffer 大小均可配置
  - 覆盖 NDS 注册流程：register_buffer + register_nds_buffer（内部会 nds_init + nds_buf_register）
  - 写入后回读并校验数据一致性，统计各阶段耗时与吞吐
  - 可选：每轮写完后清除内存副本（batch_replica_clear），强制下一次读走 3FS/disk，
    避免 Mooncake 读命中本地 MEMORY 缓存，无需重启 store

用法示例：
  # 默认：16 个 key，每个 key 2 块 4MB buffer
  python3 test_nds_batch_py.py

  # 自定义规模
  python3 test_nds_batch_py.py --num-keys 64 --buffers-per-key 4 --chunk-size 8388608

  # 关闭数据校验、跑多轮
  python3 test_nds_batch_py.py --rounds 5 --no-verify

  # 每轮写完后等 lease 过期并清除内存副本，让下一次读真正走 3FS/disk
  # （master 默认 default_kv_lease_ttl=5000ms，--lease-wait-ms 需大于它）
  python3 test_nds_batch_py.py --rounds 3 --clear-after-put --lease-wait-ms 6000
"""

import argparse
import sys
import time

import torch
import torch_npu
from mooncake.store import MooncakeDistributedStore, ReplicateConfig


def parse_args():
    p = argparse.ArgumentParser(description="NDS batch multi-buffer I/O test")
    # --- 测试规模 ---
    p.add_argument("--num-keys", type=int, default=16,
                   help="number of keys (default: 16)")
    p.add_argument("--buffers-per-key", type=int, default=2,
                   help="number of HBM buffers per key (default: 2)")
    p.add_argument("--chunk-size", type=int, default=4 * 1024 * 1024,
                   help="bytes per buffer (default: 4MB)")
    p.add_argument("--rounds", type=int, default=1,
                   help="put+get rounds to run (default: 1)")
    p.add_argument("--no-verify", action="store_true",
                   help="skip data verification")
    # --- 避免内存缓存：写后清除内存副本，强制走 3FS ---
    p.add_argument("--clear-after-put", action="store_true",
                   help="after each put, wait for lease expiry then clear "
                        "memory replicas (batch_replica_clear on the local "
                        "memory segment) so the next get is forced to read "
                        "from 3FS/disk")
    p.add_argument("--lease-wait-ms", type=int, default=6000,
                   help="ms to sleep after put before batch_replica_clear "
                        "(must exceed master default_kv_lease_ttl, default "
                        "5000ms; default: 6000)")
    # --- 环境 ---
    p.add_argument("--device", type=int, default=0, help="NPU device id")
    p.add_argument("--host", type=str, default="127.0.0.1",
                   help="local node address")
    p.add_argument("--metadata-server", type=str, default="P2PHANDSHAKE",
                   help="metadata server connection string")
    p.add_argument("--master", type=str, default="127.0.0.1:50088",
                   help="master service address")
    p.add_argument("--segment-size", type=int, default=64 * 1024 * 1024,
                   help="segment size for setup")
    p.add_argument("--local-buffer-size", type=int, default=64 * 1024 * 1024,
                   help="local buffer size for setup")
    return p.parse_args()


def verify_roundtrip(src_cpu, dst_npu):
    """Copy dst HBM back to CPU and compare against src CPU pattern.

    Returns (num_buffers, num_ok, first_bad_desc)
    """
    num_ok = 0
    first_bad = None
    num_buffers = 0
    for k, key_cpu in enumerate(src_cpu):
        for b, cpu_src in enumerate(key_cpu):
            num_buffers += 1
            got = dst_npu[k][b].cpu()
            if torch.equal(got, cpu_src):
                num_ok += 1
            elif first_bad is None:
                # Locate first mismatch for diagnostics
                diff = (got != cpu_src)
                idx = int(diff.nonzero()[0][0])
                first_bad = (k, b, idx,
                             int(cpu_src[idx]), int(got[idx]))
    return num_buffers, num_ok, first_bad


def main():
    args = parse_args()

    torch.npu.set_device(args.device)
    store = MooncakeDistributedStore()
    store.setup(
        args.host,                 # 本机地址
        args.metadata_server,      # 元数据服务
        args.segment_size,         # segment 大小
        args.local_buffer_size,    # 本地 buffer 大小
        "ascend",                  # 传输协议（NDS 场景固定 ascend）
        "",                        # RDMA 设备自动发现
        args.master,               # master 地址
    )

    keys = [f"nds_batch_test_{i:06d}" for i in range(args.num_keys)]
    per_key_bytes = args.buffers_per_key * args.chunk_size
    total_bytes = args.num_keys * per_key_bytes
    print("=" * 70)
    print(f"NDS batch multi-buffer test")
    print(f"  num_keys          = {args.num_keys}")
    print(f"  buffers_per_key   = {args.buffers_per_key}")
    print(f"  chunk_size        = {args.chunk_size} bytes "
          f"({args.chunk_size / 1024 / 1024:.2f} MiB)")
    print(f"  per_key_total     = {per_key_bytes} bytes "
          f"({per_key_bytes / 1024 / 1024:.2f} MiB)")
    print(f"  total_data        = {total_bytes} bytes "
          f"({total_bytes / 1024 / 1024:.2f} MiB)")
    print("=" * 70)

    # --- 分配 HBM buffer 并注册（NDS 使用前必须 register_nds_buffer） ---
    # src_cpu 保留 CPU 侧基准数据用于校验；src_npu/dst_npu 为 HBM 侧。
    src_cpu, src_npu, dst_npu = [], [], []
    all_src_ptrs, all_dst_ptrs, all_sizes = [], [], []

    for k in range(args.num_keys):
        key_src_cpu, key_src_npu, key_dst_npu = [], [], []
        key_src_ptrs, key_dst_ptrs, key_sizes = [], [], []
        for b in range(args.buffers_per_key):
            # 确定性字节 pattern（便于定位问题）
            base = (0xA1 + 37 * k) & 0xFF
            cpu_src = torch.full((args.chunk_size,), base, dtype=torch.uint8)
            npu_src = cpu_src.npu()
            npu_dst = torch.zeros(args.chunk_size, dtype=torch.uint8).npu()

            # RDMA 与 NDS 双侧注册（NDS 内部完成 nds_init + nds_buf_register）
            for ptr, size in ((npu_src.data_ptr(), args.chunk_size),
                              (npu_dst.data_ptr(), args.chunk_size)):
                store.register_buffer(ptr, size)
                store.register_nds_buffer(ptr, size)

            key_src_cpu.append(cpu_src)
            key_src_npu.append(npu_src)
            key_dst_npu.append(npu_dst)
            key_src_ptrs.append(npu_src.data_ptr())
            key_dst_ptrs.append(npu_dst.data_ptr())
            key_sizes.append(args.chunk_size)
        src_cpu.append(key_src_cpu)
        src_npu.append(key_src_npu)
        dst_npu.append(key_dst_npu)
        all_src_ptrs.append(key_src_ptrs)
        all_dst_ptrs.append(key_dst_ptrs)
        all_sizes.append(key_sizes)

    config = ReplicateConfig()
    config.prefer_alloc_in_same_node = False

    print(f"Buffers allocated & registered. Starting {args.rounds} round(s)...\n")

    total_put_s, total_get_s = 0.0, 0.0
    for r in range(args.rounds):
        t0 = time.time()

        put_ret = store.batch_put_from_multi_buffers(
            keys, all_src_ptrs, all_sizes, config)
        t1 = time.time()

        # 可选：清除内存副本，确保 get 真正从 3FS/disk 读。
        # 注意：必须传本地内存 segment 名（store.get_hostname()），否则空
        # segment_name 会把整个 key（含 3FS disk 副本）一并删除，get 报 not found。
        if args.clear_after_put:
            if not all(isinstance(v, int) and v == 0 for v in put_ret):
                print(f"[round {r + 1}] FAIL: put failed, skip replica clear")
                sys.exit(1)
            time.sleep(args.lease_wait_ms / 1000.0)
            cleared = store.batch_replica_clear(keys, store.get_hostname())
            print(f"[round {r + 1}] replica_clear: "
                  f"{len(cleared)}/{len(keys)} memory replicas cleared on "
                  f"segment '{store.get_hostname()}', "
                  f"next get forced to 3FS/disk")
            t1 = time.time()  # put 计时截止到 clear 之前，清晰区分 put/clear

        get_ret = store.batch_get_into_multi_buffers(
            keys, all_dst_ptrs, all_sizes, False)
        t2 = time.time()

        put_ok = all(isinstance(v, int) and v == 0 for v in put_ret)
        get_ok = all(isinstance(v, int) and v >= 0 for v in get_ret)
        print(f"[round {r + 1}] put: {t1 - t0:.3f}s  get: {t2 - t1:.3f}s  "
              f"(put ret: {put_ret if len(put_ret) <= 8 else '...'}, "
              f"get ret: {get_ret if len(get_ret) <= 8 else '...'})")

        if not put_ok:
            bad = [i for i, v in enumerate(put_ret) if v != 0]
            print(f"[round {r + 1}] FAIL: batch_put_from_multi_buffers "
                  f"returned non-zero for keys {bad}")
            sys.exit(1)
        if not get_ok:
            bad = [i for i, v in enumerate(get_ret) if v < 0]
            print(f"[round {r + 1}] FAIL: batch_get_into_multi_buffers "
                  f"returned negative for keys {bad}")
            sys.exit(1)

        if not args.no_verify:
            num_bufs, num_ok, first_bad = verify_roundtrip(src_cpu, dst_npu)
            if num_ok != num_bufs:
                k, b, idx, exp, got = first_bad
                print(f"[round {r + 1}] FAIL: data mismatch "
                      f"{num_ok}/{num_bufs} buffers ok; "
                      f"first bad key={k} buf={b} offset={idx} "
                      f"expect=0x{exp:02x} got=0x{got:02x}")
                sys.exit(1)
            print(f"[round {r + 1}] verify: {num_ok}/{num_bufs} buffers PASS")

        total_put_s += t1 - t0
        total_get_s += t2 - t1

    print("-" * 70)
    avg_put = total_put_s / args.rounds
    avg_get = total_get_s / args.rounds
    print(f"avg put : {avg_put:.3f}s  -> {total_bytes / avg_put / 1024 / 1024:.2f} MiB/s"
          if avg_put > 0 else "avg put : n/a")
    print(f"avg get : {avg_get:.3f}s  -> {total_bytes / avg_get / 1024 / 1024:.2f} MiB/s"
          if avg_get > 0 else "avg get : n/a")
    print(f"all {args.rounds} round(s) PASS!")


if __name__ == "__main__":
    main()
