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
    """A dummy shell object; not used in current tests."""
    pass


class DummyKVPoolMLA:
    """A KVCache mock implementation for is_mla=True (MLA format)."""

    def __init__(
        self,
        layer_num: int,
        num_slots: int = 1280,
        dim: int = 1,
        device: str | torch.device = "cuda",
        empty: bool = False
    ):
        # Store attributes with names similar to the real implementation
        self.store_dtype = torch.float16
        self.kv_cache_dim = dim          # Equivalent to kv_lora_rank + qk_rope_head_dim
        self.layer_num = layer_num
        self.size = num_slots
        self.device = torch.device(device)

        # MLA: each layer has a tensor of shape (size, 1, kv_cache_dim)
        # Corresponds to MLATokenToKVPool where:
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
    """A KVCache mock implementation for is_mla=False (MHA format)."""

    def __init__(
        self,
        layer_num: int,
        num_slots: int = 1280,
        dim: int = 1,
        head_num: int = 1,
        device: str | torch.device = "cuda",
        empty: bool = False
    ):
        # Store attributes
        self.store_dtype = torch.float16
        self.head_num = head_num         # Number of attention heads
        self.head_dim = dim              # Dimension per head
        self.layer_num = layer_num
        self.size = num_slots
        self.device = torch.device(device)

        # MHA: k_buffer / v_buffer, each layer is a tensor of shape (size, head_num, head_dim)
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
        Before each test, clean all content inside /sgl-workspace/sglang_data
        while keeping the directory itself.
        """
        self.storage_dir = "/sgl-workspace/sglang_data"

        # Create directory if not exists
        os.makedirs(self.storage_dir, exist_ok=True)

        # Remove all files/subdirectories inside it, keep the directory itself
        for entry in os.listdir(self.storage_dir):
            path = os.path.join(self.storage_dir, entry)
            try:
                if os.path.isfile(path) or os.path.islink(path):
                    os.unlink(path)
                elif os.path.isdir(path):
                    shutil.rmtree(path)
            except Exception as e:
                print(f"Failed to delete {path}: {e}")

    # --------------------- Common constructors --------------------- #
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
        # 10 blocks, each block has 128 slots, total = 1280
        token_slots = torch.arange(10).unsqueeze(1) + torch.arange(0, 1280, 10)
        self.assertEqual(token_slots.shape, (10, 128))
        return token_slots

    # ------------------- build_transfer_data tests ------------------- #
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
            # The content must match
            assert torch.allclose(
                block_tensors[i], tensors[i]
            ), f"KV content mismatch at index {i}"

            # Memory addresses must differ (BlockPool copies or reassigns KV)
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
            # The content must match
            assert torch.allclose(
                block_tensors[i], tensors[i]
            ), f"KV content mismatch at index {i}"

            # Memory addresses must differ
            assert block_tensors[i].data_ptr() != tensors[i].data_ptr(), (
                f"KV tensors share memory at index {i}, but they should not"
            )

    # ------------------- Common logic for dump + load ------------------- #
    def _run_dump_and_load(self, is_mla: bool):
        """
        Common dump + load test workflow:
        1. Build a connector with a non-empty KVPool, prepare data and run dump.
        2. Build another connector with an empty KVPool, run lookup + load.
        3. Assert correctness of loaded KV content.
        """
        # ====== 1. Dump-side connector ====== #
        uc_config = self._build_uc_config(is_mla=is_mla)
        env_config = self._build_env_config(is_mla=is_mla, layer_num=1, empty=False)
        connector_dump = UnifiedCacheConnector("UcmNfsStore", uc_config, env_config)
        self.assertIsNotNone(connector_dump)

        token_slots = self._build_token_slots()
        layer_id = 0

        # MLA / MHA use different build_transfer_data
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

        # Build token_ids and request_id
        if is_mla:
            token_ids = list(range(1280))
        else:
            token_ids = list(range(1280, 1280 * 2))
        self.assertEqual(len(token_ids), 1280)
        request_id = f"test_request_dump_{'mla' if is_mla else 'mha'}"

        # First lookup: Expect no hits before dumping
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
        # Expect 10 blocks
        self.assertEqual(len(block_ids), 10)

        # Actual dump
        dump_block_ids = block_ids * (1 if connector_dump.is_mla else 2)
        task = connector_dump.connector.dump(dump_block_ids, offsets, tensors)
        self.assertIsNotNone(task)

        ret = connector_dump.connector.wait(task)
        self.assertEqual(ret, 0, "dump task failed, wait() should return 0 on success")
        connector_dump.connector.commit(block_ids, True)

        # ====== 2. Load-side connector (empty KVPool) ====== #
        env_config_load = self._build_env_config(is_mla=is_mla, layer_num=1, empty=True)
        connector_load = UnifiedCacheConnector("UcmNfsStore", uc_config, env_config_load)
        self.assertIsNotNone(connector_load)

        # Second lookup: should hit something after dump
        request_id_load = f"test_request_load_{'mla' if is_mla else 'mha'}"
        storage_hit_num_2 = connector_load.get_num_new_matched_tokens(
            request_id_load, token_ids, 0
        )
        self.assertGreater(
            storage_hit_num_2,
            0,
            "lookup for load should hit something after dump",
        )

        # Record KV state before load
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

        # Call high-level API to load KV
        connector_load.start_load_kv(token_slots, request_id_load)
        connector_load.wait_for_layer_load(0)

        # ====== 3. Assertions after load ====== #
        if is_mla:
            after_kv = connector_load.token_to_kv_pool.kv_buffer
            # Some KV must change because dumped values were random
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

    # ------------------- Entry tests for MLA / MHA ------------------- #
    def test_dump_and_load_mla(self):
        self._run_dump_and_load(is_mla=True)

    def test_dump_and_load_mha(self):
        self._run_dump_and_load(is_mla=False)


if __name__ == "__main__":
    unittest.main()
