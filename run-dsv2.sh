#!/bin/bash

# 设置环境变量
export ASCEND_RT_VISIBLE_DEVICES=6,7
export ENABLE_UCM_PATCH=1
export UC_LOGGER_LEVEL="debug"

# 启动服务
vllm serve /mnt/model/DeepSeek-V2-Lite-Chat/ \
    --tensor-parallel-size 2 \
    --block-size 128 \
    --gpu-memory-utilization 0.85 \
    --trust-remote-code \
    --distributed-executor-backend mp \
    --host 0.0.0.0 \
    --port 8010 \
    --kv-transfer-config '{"kv_connector":"UCMConnector","kv_connector_module_path":"ucm.integration.vllm.ucm_connector","kv_role":"kv_both","kv_connector_extra_config":{"UCM_CONFIG_FILE":"/workspace/ucm_config_asu.yaml"}}'
