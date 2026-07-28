export LD_LIBRARY_PATH=/home/ssu/cann_b070/ascend-toolkit/latest/python/site-packages:$LD_LIBRARY_PATH
export PYTHONPATH=$PYTHONPATH:/home/ssu/code/vllm-ascend
export MOONCAKE_CONFIG_PATH="/home/ssu/code/Mooncake/model_runner/mooncake.json"
export ASCEND_RT_VISIBLE_DEVICES=0
export PYTHONHASHSEED=0
export ACL_OP_INIT_MODE=1
#A3
export ASCEND_ENABLE_USE_FABRIC_MEM=0
#A2
#export HCCL_INTRA_ROCE_ENABLE=1
export HCCL_RDMA_TIMEOUT=17
export ASCEND_CONNECT_TIMEOUT=10000
export ASCEND_TRANSFER_TIMEOUT=10000

export MC_NDS_DEVICE_ID=0

vllm serve \
  --config /home/ssu/code/Mooncake/model_runner/qwen2.5_config.yaml