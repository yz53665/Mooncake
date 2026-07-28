# 启动vllm的benchmark服务，用于向模型服务发送请求
# 混合PD模式

export LD_LIBRARY_PATH=/home/ssu/cann_b070/ascend-toolkit/latest/python/site-packages:$LD_LIBRARY_PATH
export MOONCAKE_CONFIG_PATH="/home/ssu/code/model_runner/mooncake.json" # 修改为mooncake.json的路径
export MC_YLT_LOG_LEVEL=info
# export LMCACHE_CONFIG_FILE="/data/xxx/lmcache_mooncake_config.yaml" #lmcache配置
export ASCEND_RT_VISIBLE_DEVICES=0
export PYTHONHASHSEED=0
export ACL_OP_INIT_MODE=1
export ASCEND_BUFFER_POOL=4:8
export ASCEND_CONNECT_TIMEOUT=10000
export ASCEND_TRANSFER_TIMEOUT=10000
export VLLM_LOGGING_LEVEL=INFO

# export VLLM_TORCH_PROFILER_WITH_STACK=0
# export VLLM_TORCH_PROFILER_WITH_PROFILE_MEMORY=0
# export VLLM_TORCH_PROFILER_WITH_FLOPS=1
# export VLLM_TORCH_PROFILER_DIR=/data/xxx/xxx/profiling/vllm # profiling路径
# export VLLM_TORCH_PROFILER_WITH_STACK=0
# 如果需要打profile，加上参数 --profile \
# sharegpt-output-len控制模型生成token长度 num-prompts控制发送请求个数

vllm bench serve \
     --model /home/ssu/Qwen2.5-7B-Instruct \
     --backend vllm \
     --port 8100 \
     --temperature 0 \
     --save-result \
     --save-detailed \
     --endpoint /v1/completions \
     --sharegpt-output-len 10 \
     --dataset-name sharegpt \
     --dataset-path /home/ssu/datasets/ShareGPT_V3_unfiltered_cleaned_split.json \
     --num-prompts 100 \
     --request_rate 10 \
     --burstiness 1 \
     --max-concurrency 10 \
     --result-dir "/home/ssu/log/"