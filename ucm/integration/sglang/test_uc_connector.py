import unittest
from unittest.mock import MagicMock, patch
from types import SimpleNamespace
from typing import List

import torch
from ucm.integration.sglang.uc_connector import (
    UnifiedCacheConnector,
    UnifiedCacheConfig,
    EnvironmentConfig,
    ReqStatus,
)


class DummyReqToTokenPool:
    """一个空壳即可，当前测试中不使用它的能力。"""
    pass


class DummyKVPoolMLA:
    """用于 is_mla=True 的 KVCache 伪实现（MLA 形状）。"""

    def __init__(
        self,
        layer_num: int,
        num_slots: int = 1280,
        dim: int = 1,
        device: str | torch.device = "cuda",
    ):
        # 存储相关属性（与真实实现保持相似的字段名）
        self.store_dtype = torch.float16
        self.kv_cache_dim = dim          # 相当于 kv_lora_rank + qk_rope_head_dim
        self.layer_num = layer_num
        self.size = num_slots
        self.device = torch.device(device)

        # MLA: 每层一个 tensor，形状 (size, 1, kv_cache_dim)
        # 对应 MLATokenToKVPool 中的：
        #   torch.zeros((size, 1, kv_lora_rank + qk_rope_head_dim), ...)
        self.kv_buffer = [
            torch.randn(
                (self.size, 1, self.kv_cache_dim),
                dtype=self.store_dtype,
                device=self.device,
            )
            for _ in range(self.layer_num)
        ]


class DummyKVPoolMHA:
    """用于 is_mla=False 的 KVCache 伪实现（MHA 形状）。"""

    def __init__(
        self,
        layer_num: int,
        num_slots: int = 1280,
        dim: int = 1,
        head_num: int = 1,
        device: str | torch.device = "cuda",
    ):
        # 存储相关属性
        self.store_dtype = torch.float16
        self.head_num = head_num         # 注意力头数
        self.head_dim = dim              # 每个头的维度
        self.layer_num = layer_num
        self.size = num_slots
        self.device = torch.device(device)

        # MHA: k_buffer / v_buffer: 每层一个 tensor，
        # 形状 (size, head_num, head_dim)
        self.k_buffer = [
            torch.randn(
                (self.size, self.head_num, self.head_dim),
                dtype=self.store_dtype,
                device=self.device,
            )
            for _ in range(self.layer_num)
        ]
        self.v_buffer = [
            torch.randn(
                (self.size, self.head_num, self.head_dim),
                dtype=self.store_dtype,
                device=self.device,
            )
            for _ in range(self.layer_num)
        ]



class TestUnifiedCacheConnector(unittest.TestCase):
    def _build_uc_config(self, is_mla: bool) -> UnifiedCacheConfig:
        head_dim = 1
        layer_num = 1
        element_size = torch._utils._element_size(torch.uint8)
        page_size = 128
        config_base = page_size * element_size * head_dim
        head_num = 1
        tp_size = 1
        kv_block_size = config_base * layer_num \
            * (1 if is_mla else head_num * tp_size * 2)
        io_size = config_base * (1 if is_mla else head_num)
        return UnifiedCacheConfig(
            storage_backends="/sgl-workspace/sglang_data",
            max_cache_size=1024,
            kv_block_size=kv_block_size,
            device=0,
            role="worker",
            io_size=io_size,
        )

    def _build_env_config(
        self,
        is_mla: bool,
        layer_num: int = 1,
    ) -> EnvironmentConfig:
        if is_mla:
            kv_pool = DummyKVPoolMLA(layer_num=layer_num)
        else:
            kv_pool = DummyKVPoolMHA(layer_num=layer_num)
        return EnvironmentConfig(
            total_tp_size=1,
            is_mla=is_mla,
            layer_num=layer_num,
            tp_group=None,
            req_to_token_pool=DummyReqToTokenPool(),
            token_to_kv_pool=kv_pool,
        )

    def test_build_transfer_data_mla(self):
        uc_config = self._build_uc_config(is_mla=True)
        env_config = self._build_env_config(is_mla=True, layer_num=1)
        connector = UnifiedCacheConnector("UcmNfsStore" ,uc_config, env_config)
        self.assertIsNotNone(connector)
        token_slots = torch.arange(10).unsqueeze(1) + torch.arange(0, 1280, 10)
        kv_layer = connector.kvcache[0]
        
        layer_id = 1

        tensors, offsets, cuda_blocks = connector._build_transfer_data_mla(
            kv_layer, layer_id, token_slots
        )

        block_tensors = []
        for blk_id in token_slots.view(-1, 128):
            block_tensors.append(kv_layer[0][blk_id])
        
        for i in range(len(block_tensors)):
            # 内容必须相等
            assert torch.allclose(
                block_tensors[i], tensors[i]
            ), f"KV content mismatch at index {i}"

            # 地址必须不同（BlockPool 会复制或重新分配 KV）
            assert block_tensors[i].data_ptr() != tensors[i].data_ptr(), (
                f"KV tensors share memory at index {i}, but they should not"
            )
    
    def test_build_transfer_data_mha(self):
        uc_config = self._build_uc_config(is_mla=False)
        env_config = self._build_env_config(is_mla=False, layer_num=1)
        connector = UnifiedCacheConnector("UcmNfsStore" ,uc_config, env_config)
        self.assertIsNotNone(connector)
        token_slots = torch.arange(10).unsqueeze(1) + torch.arange(0, 1280, 10)
        kv_layer = connector.kvcache[0]
        
        layer_id = 0

        tensors, offsets, cuda_blocks = connector._build_transfer_data_mha(
            kv_layer, layer_id, token_slots
        )

        block_k_tensors = []
        block_v_tensors = []
        for blk_id in token_slots.view(-1, 128):
            block_k_tensors.append(kv_layer[0][blk_id])
            block_v_tensors.append(kv_layer[1][blk_id])
        
        block_tensors = block_k_tensors + block_v_tensors
        
        for i in range(len(block_tensors)):
            # 内容必须相等
            assert torch.allclose(
                block_tensors[i], tensors[i]
            ), f"KV content mismatch at index {i}"

            # 地址必须不同（BlockPool 会复制或重新分配 KV）
            assert block_tensors[i].data_ptr() != tensors[i].data_ptr(), (
                f"KV tensors share memory at index {i}, but they should not"
            )





if __name__ == "__main__":
    unittest.main()
