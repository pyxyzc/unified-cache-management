#!/bin/bash

cd /sgl-workspace/kvcache/
rm -rf *

cd /sgl-workspace/sglang/

MODEL_PATH=/home/models/QwQ-32B
PORT=30000
TP=2

HICACHE_CONFIG='{
  "backend_name":"unifiedcache",
  "module_path":"ucm.integration.sglang.unifiedcache_store",
  "class_name":"UnifiedCacheStore",
  "interface_v1":1,
  "kv_connector_extra_config":{
    "ucm_connector_name":"UcmPipelineStore",
    "ucm_connector_config":{
      "storage_backends":"/sgl-workspace/kvcache"
    }
  }
}'

export CUDA_VISIBLE_DEVICES=1,2

python python/sglang/launch_server.py \
    --model-path "$MODEL_PATH" \
    --page-size 128 \
    --tp "$TP" \
    --port "$PORT" \
    --enable-hierarchical-cache \
    --hicache-mem-layout page_first \
    --hicache-write-policy write_through \
    --hicache-storage-backend dynamic \
    --hicache-storage-prefetch-policy wait_complete \
    --hicache-storage-backend-extra-config "$HICACHE_CONFIG"

