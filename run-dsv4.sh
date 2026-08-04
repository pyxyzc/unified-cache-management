export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=1024
export VLLM_ASCEND_ENABLE_FLASHCOMM1=1
export VLLM_ASCEND_APPLY_DSV4_PATCH=1
export TASK_QUEUE_ENABLE=1
export HCCL_OP_EXPANSION_MODE="AIV"
export ENABLE_UCM_PATCH=1

vllm serve /models/DeepSeek-V4-Flash-w8a8-mtp \
    --max_model_len 1048576 \
    --max-num-batched-tokens 4096 \
    --served-model-name dsv4 \
    --gpu-memory-utilization 0.92 \
    --data-parallel-size 1 \
    --tensor-parallel-size 8 \
    --enable-expert-parallel \
    --tokenizer-mode deepseek_v4 \
    --tool-call-parser deepseek_v4 \
    --enable-auto-tool-choice \
    --reasoning-parser deepseek_v4 \
    --safetensors-load-strategy 'prefetch' \
    --enable-prefix-caching \
    --model-loader-extra-config='{"enable_multithread_load": "true", "num_threads": 128}' \
    --quantization ascend \
    --port 8900 \
    --block-size 128 \
    --speculative-config '{"num_speculative_tokens": 1,"method": "mtp","enforce_eager": true}' \
    --compilation-config '{"cudagraph_mode": "FULL_DECODE_ONLY"}'\
    --async-scheduling \
    --no-disable-hybrid-kv-cache-manager \
    --additional-config '
    {"ascend_compilation_config":{
        "enable_npugraph_ex":true,
        "enable_static_kernel":false
        },
    "enable_cpu_binding": false,
    "enable_dsa_cp": true,
    "multistream_overlap_shared_expert":true}' \
    --kv-transfer-config \
    '{
        "kv_connector": "UCMConnector",
        "kv_connector_module_path": "ucm.integration.vllm.ucm_connector",
        "kv_role": "kv_both",
        "kv_connector_extra_config": {"UCM_CONFIG_FILE": "/home/yp/unified-cache-management/examples/ucm_config_example.yaml"}
    }'
