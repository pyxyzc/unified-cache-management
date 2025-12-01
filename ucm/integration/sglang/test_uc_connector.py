import os
import shutil
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
from ucm.integration.sglang.uc_utils import hash_request_tokens, md5
from sglang.srt.utils.hf_transformers_utils import get_tokenizer  


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
        empty: bool = False
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
        if empty:
            self.kv_buffer = [
                torch.zeros(
                    (self.size, 1, self.kv_cache_dim),
                    dtype=self.store_dtype,
                    device=self.device,
                )
                for _ in range(self.layer_num)
            ]
        else:
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
        empty: bool = False
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
        if empty:
            self.k_buffer = [
                torch.zeros(
                    (self.size, self.head_num, self.head_dim),
                    dtype=self.store_dtype,
                    device=self.device,
                )
                for _ in range(self.layer_num)
            ]
            self.v_buffer = [
                torch.zeros(
                    (self.size, self.head_num, self.head_dim),
                    dtype=self.store_dtype,
                    device=self.device,
                )
                for _ in range(self.layer_num)
            ]
        else:
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
    
    def setUp(self):
        """
        在每次测试前，清空 /sgl-workspace/sglang_data 目录中的内容，但保留目录本体。
        """
        self.storage_dir = "/sgl-workspace/sglang_data"

        # 若目录不存在，则创建它
        os.makedirs(self.storage_dir, exist_ok=True)

        # 删除目录中所有文件和子目录，但保留目录本体
        for entry in os.listdir(self.storage_dir):
            path = os.path.join(self.storage_dir, entry)
            try:
                if os.path.isfile(path) or os.path.islink(path):
                    os.unlink(path)
                elif os.path.isdir(path):
                    shutil.rmtree(path)
            except Exception as e:
                print(f"Failed to delete {path}: {e}")

    # --------------------- 公共构造工具 --------------------- #
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
        dir = "/sgl-workspace/sglang_data"
        return UnifiedCacheConfig(
            storage_backends=dir,
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
        empty=False,
    ) -> EnvironmentConfig:
        if is_mla:
            kv_pool = DummyKVPoolMLA(layer_num=layer_num, empty=empty)
        else:
            kv_pool = DummyKVPoolMHA(layer_num=layer_num, empty=empty)
        return EnvironmentConfig(
            total_tp_size=1,
            is_mla=is_mla,
            layer_num=layer_num,
            tp_group=None,
            req_to_token_pool=DummyReqToTokenPool(),
            token_to_kv_pool=kv_pool,
        )

    def _create_block_ids(self, token_ids: List[int]):
        return hash_request_tokens(md5, 128, token_ids)

    def _build_token_slots(self) -> torch.Tensor:
        # 10 个 block，每个 block 128 个 slot，共 1280
        token_slots = torch.arange(10).unsqueeze(1) + torch.arange(0, 1280, 10)
        self.assertEqual(token_slots.shape, (10, 128))
        return token_slots

    # ------------------- build_transfer_data 保留 ------------------- #
    def test_build_transfer_data_mla(self):
        uc_config = self._build_uc_config(is_mla=True)
        env_config = self._build_env_config(is_mla=True, layer_num=1)
        connector = UnifiedCacheConnector("UcmNfsStore", uc_config, env_config)
        self.assertIsNotNone(connector)

        token_slots = self._build_token_slots()
        kv_layer = connector.kvcache[0]
        layer_id = 0

        tensors, offsets, cuda_blocks = connector._build_transfer_data_mla(
            layer_id, token_slots
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
        connector = UnifiedCacheConnector("UcmNfsStore", uc_config, env_config)
        self.assertIsNotNone(connector)

        token_slots = self._build_token_slots()
        kv_layer = connector.kvcache[0]
        layer_id = 0

        tensors, offsets, cuda_blocks = connector._build_transfer_data_mha(
            layer_id, token_slots
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

    # ------------------- dump + load 合并的公共逻辑 ------------------- #
    def _run_dump_and_load(self, is_mla: bool):
        """
        通用的 dump + load 测试流程：
        1. 用非空 KVPool 构造 connector，准备数据并执行 dump。
        2. 用空 KVPool 构造新的 connector，执行 lookup + load。
        3. 对加载后的 KV 内容做必要的断言。
        """
        # ====== 1. 准备 dump 侧 connector ====== #
        uc_config = self._build_uc_config(is_mla=is_mla)
        env_config = self._build_env_config(is_mla=is_mla, layer_num=1, empty=False)
        connector_dump = UnifiedCacheConnector("UcmNfsStore", uc_config, env_config)
        self.assertIsNotNone(connector_dump)

        token_slots = self._build_token_slots()
        layer_id = 0

        # 这里根据 MLA / MHA 走不同的 build_transfer_data
        if is_mla:
            tensors, offsets, cuda_blocks = connector_dump._build_transfer_data_mla(
                layer_id, token_slots
            )
        else:
            tensors, offsets, cuda_blocks = connector_dump._build_transfer_data_mha(
                layer_id, token_slots
            )

        self.assertIsInstance(tensors, (list, tuple))
        self.assertGreater(len(tensors), 0)
        self.assertEqual(len(cuda_blocks), len(offsets))

        # 构造 token_ids 和 request_id
        if is_mla:
            token_ids = list(range(1280))
        else:
            token_ids = list(range(1280, 1280 * 2))
        self.assertEqual(len(token_ids), 1280)
        request_id = f"test_request_dump_{'mla' if is_mla else 'mha'}"

        # 第一次 lookup：对于 dump 场景我们期望是「没有命中，全部需要 dump」
        storage_hit_num = connector_dump.get_num_new_matched_tokens(
            request_id, token_ids, 0
        )
        self.assertEqual(
            storage_hit_num,
            0,
            "first lookup before dump should have 0 storage hits",
        )

        connector_dump.update_state_after_alloc(request_id)
        status: ReqStatus = connector_dump.req_status_dict.get(request_id, None)
        self.assertIsNotNone(status)

        block_ids = status.block_hashes
        dump_start = status.dump_index
        block_ids = block_ids[dump_start:]
        # 10 个 block
        self.assertEqual(len(block_ids), 10)

        # 真正进行 dump
        dump_block_ids = block_ids * (1 if connector_dump.is_mla else 2)
        task = connector_dump.connector.dump(dump_block_ids, offsets, tensors)
        self.assertIsNotNone(task)

        ret = connector_dump.connector.wait(task)
        self.assertEqual(ret, 0, "dump task failed, wait() should return 0 on success")
        connector_dump.connector.commit(block_ids, True)

        # ====== 2. 准备 load 侧 connector（空 KVPool） ====== #
        env_config_load = self._build_env_config(is_mla=is_mla, layer_num=1, empty=True)
        connector_load = UnifiedCacheConnector("UcmNfsStore", uc_config, env_config_load)
        self.assertIsNotNone(connector_load)

        # 第二次 lookup：这次应该能从存储中命中一部分 / 全部 block
        request_id_load = f"test_request_load_{'mla' if is_mla else 'mha'}"
        storage_hit_num_2 = connector_load.get_num_new_matched_tokens(
            request_id_load, token_ids, 0
        )
        # 至少应该有命中（>0），具体数量看实现策略
        self.assertGreater(
            storage_hit_num_2,
            0,
            "lookup for load should hit something after dump",
        )

        # 记录下 load 前 KV 的状态，用于之后对比
        if is_mla:
            before_kv = [
                buf.clone()
                for buf in connector_load.token_to_kv_pool.kv_buffer
            ]
        else:
            before_k = [
                buf.clone()
                for buf in connector_load.token_to_kv_pool.k_buffer
            ]
            before_v = [
                buf.clone()
                for buf in connector_load.token_to_kv_pool.v_buffer
            ]

        # 调用高层接口进行 KV 加载
        connector_load.start_load_kv(token_slots, request_id_load)
        connector_load.wait_for_layer_load(0)

        # ====== 3. 对 load 后 KV 内容做断言 ====== #
        if is_mla:
            after_kv = connector_load.token_to_kv_pool.kv_buffer
            # 至少某一层 KV 不再全部为零（因为我们前面 dump 的是随机数）
            for layer_id in range(len(after_kv)):
                self.assertFalse(
                    torch.allclose(after_kv[layer_id], before_kv[layer_id]),
                    f"MLA layer {layer_id} KV buffer did not change after load",
                )
        else:
            after_k = connector_load.token_to_kv_pool.k_buffer
            after_v = connector_load.token_to_kv_pool.v_buffer
            for layer_id in range(len(after_k)):
                self.assertFalse(
                    torch.allclose(after_k[layer_id], before_k[layer_id]),
                    f"MHA layer {layer_id} K buffer did not change after load",
                )
                self.assertFalse(
                    torch.allclose(after_v[layer_id], before_v[layer_id]),
                    f"MHA layer {layer_id} V buffer did not change after load",
                )

    # ------------------- 两种模型类型的 dump+load 测试入口 ------------------- #
    def test_dump_and_load_mla(self):
        self._run_dump_and_load(is_mla=True)

    def test_dump_and_load_mha(self):
        self._run_dump_and_load(is_mla=False)


if __name__ == "__main__":
    unittest.main()