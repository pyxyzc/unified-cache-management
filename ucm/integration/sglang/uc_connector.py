from dataclasses import dataclass, asdict, field
from enum import Enum
import hashlib
import pickle
import torch
import torch.distributed as dist
from typing import TYPE_CHECKING, Any, List, Optional, Union
from collections import defaultdict

from ucm.integration.sglang.uc_utils import hash_request_tokens, md5
from ucm.integration.sglang.cuda_block_manager import CUDABlockPool
from ucm.logger import init_logger
from ucm.store.factory import UcmConnectorFactory
from ucm.store.ucmstore import Task

from sglang.srt.managers.schedule_batch import ScheduleBatch
from sglang.srt.mem_cache.memory_pool import ReqToTokenPool, KVCache

logger = init_logger(__name__)


@dataclass
class UnifiedCacheConfig:
    storage_backends: str
    max_cache_size: int
    kv_block_size: int
    device: int
    role: str
    io_size: int


@dataclass
class EnvironmentConfig:
    total_tp_size: int
    is_mla: bool
    layer_num: int
    tp_group: Optional[torch.distributed.ProcessGroup]
    req_to_token_pool: ReqToTokenPool
    token_to_kv_pool: KVCache

@dataclass
class FetchItem:
    block_id: str
    start_token_id: int
    end_token_id: int


@dataclass
class DumpItem:
    block_id: str
    start_token_id: int
    end_token_id: int
    cache_out_loc: torch.Tensor


@dataclass
class ReqStatus:
    block_hashes: list[str] = field(default_factory=list)
    fetch_index: int = 0
    dump_index: int = 0
    end_index: int = 0

@dataclass
class ReqTransferMetadata:
    request_id: str
    fetch_items: list[FetchItem] = field(default_factory=list)
    dump_items: list[DumpItem] = field(default_factory=list)


@dataclass
class UCTransferMetadata:
    request_metadata_list: list[ReqTransferMetadata] = field(default_factory=list)


class UnifiedCacheConnector():
    def __init__(self, uc_connector_name: str, unifiedCacheConfig: UnifiedCacheConfig, environmentConfig: EnvironmentConfig):
        self.environmentConfig = environmentConfig
        self.tp_rank = unifiedCacheConfig.device
        self.total_tp_size = environmentConfig.total_tp_size

        unifiedCacheConfig_dict = asdict(unifiedCacheConfig)
        self.connector = UcmConnectorFactory.create_connector(uc_connector_name, unifiedCacheConfig_dict)

        self.is_mla = environmentConfig.is_mla
        self.cache_nums = 1 if self.is_mla else 2

        self.num_layers = environmentConfig.layer_num
        self.tp_group = environmentConfig.tp_group
        
        self.req_status_dict: dict[str, ReqStatus] = {}
        self.dump_tasks: dict[str, dict[str, list[Task]]] 
        self.block_dump_status: dict[str, dict[str, list[int]]]
        self.dump_tasks = defaultdict(lambda: defaultdict(list))
        self.block_dump_status = defaultdict(lambda: defaultdict(list[int]))

        self._transfer_metadata: UCTransferMetadata | None = None
        self.current_layer: int = 0

        self.block_size = 128 # TODO: make it configurable
        self.layerwise_load_tasks: dict[str, dict[int, Task]] = {}
        self.req_to_slots: dict[str, torch.Tensor] = {}
        self.req_to_token_pool = environmentConfig.req_to_token_pool
        self.token_to_kv_pool = environmentConfig.token_to_kv_pool
        if self.is_mla:
            kv_buffer = self.token_to_kv_pool.kv_buffer  # shape: [num_layers, ...]
            self.kvcache = [(kv_buffer[i], kv_buffer[i]) for i in range(len(kv_buffer))]
            self.cuda_block_pool = CUDABlockPool(
                is_mla=self.is_mla,
                block_size=self.block_size,
                kv_cache_dim=self.token_to_kv_pool.kv_cache_dim,
                dtype=self.token_to_kv_pool.store_dtype,
            )
        else:
            k_buffer = self.token_to_kv_pool.k_buffer   # shape: [num_layers, ...]
            v_buffer = self.token_to_kv_pool.v_buffer   # shape: [num_layers, ...]
            self.kvcache = [(k_buffer[i], v_buffer[i]) for i in range(len(k_buffer))]
            self.cuda_block_pool = CUDABlockPool(
                is_mla=self.is_mla,
                block_size=self.block_size,
                head_num=self.token_to_kv_pool.head_num,
                head_dim=self.token_to_kv_pool.head_dim,
                dtype=self.token_to_kv_pool.store_dtype,
            )
        self.task_to_cuda_blocks: dict[Task, List[int]] = {}

        self.prev_dump_block_ids: dict[str, list[str]] = {}
        self.prev_dump_tensors: dict[str, list[torch.Tensor]] = {}
        self.prev_dump_offsets: dict[str, list[int]] = {}
        self.prev_dump_blocks: dict[str, list[int]] = {}

    def _data_offset_mha(self, kv_layer, layer_id, block_size=128):
        # Non-MLA scene: one layer shape is  (num_tokens num_kv_heads, head_size)
        # kvcache = [self.token_to_kv_pool.k_buffer, self.token_to_kv_pool.v_buffer]
        # Element size
        elem_size = kv_layer[0].element_size()
        logger.debug(
            f"total_tp_size = {self.total_tp_size},\n" f"element size = {elem_size}."
        )
        # One block size
        k_min_data_block_size = (
            kv_layer[0][0].numel() * block_size
        ) * elem_size
        v_min_data_block_size = (
            kv_layer[1][0].numel() * block_size
        ) * elem_size
        # When tp > 1 layer_size = (k_min_data_block_size + v_min_data_block_size) * tp_size

        layer_size = (k_min_data_block_size + v_min_data_block_size) * self.total_tp_size 
        offset = int(layer_size * layer_id + layer_size / self.total_tp_size * self.tp_rank)
        # Offset of k = layer_size * layer_id + layer_size / tp_size * current rank
        # Offset of v = Offset of k + k_min_data_block_size
        return offset, offset + k_min_data_block_size

    def _data_offset_mla(self, kv_layer, layer_id, block_size=128):
        # MLA scene: one layer shape is (num_tokens , 1 , head_size)
        # kvcache = [self.token_to_kv_pool.kv_buffer]
        # Element size
        elem_size = kv_layer[0].element_size()
        logger.debug(
            f"total_tp_size = {self.total_tp_size},\n" f"element size = {elem_size}."
        )
        # One block size
        kv_min_data_block_size = (
            kv_layer[0][0].numel() * block_size
        ) * elem_size
        # layer_size = k_min_data_block_size
        layer_size = kv_min_data_block_size
        offset = int(layer_size * layer_id)
        # Offset of kv = layer_size * layer_id 
        return offset
    
    def _build_transfer_data_mla(
        self, layer_id, token_slots: torch.Tensor, block_size=128
    ) -> tuple[List[torch.Tensor], List[int], List[int]]:
        '''
        Build transfer data and offsets for MHA (non-MLA) kv cache.
        '''
        kv_tensors = []
        kv_offsets = []
        kv_cuda_blocks = []
        kv_layer = self.kvcache[layer_id]

        for blk_id in token_slots.view(-1, block_size):
            cuda_block_id = self.cuda_block_pool.alloc(kv_layer[0][blk_id])
            cuda_block = self.cuda_block_pool.get_block(cuda_block_id)
            offset = self._data_offset_mla(kv_layer, layer_id, block_size)
            kv_tensors.append(cuda_block)
            kv_offsets.append(offset)
            kv_cuda_blocks.append(cuda_block_id)
        return kv_tensors, kv_offsets, kv_cuda_blocks
    
    def _build_transfer_data_mha(
        self, layer_id, token_slots: torch.Tensor, block_size=128
    ) -> tuple[List[torch.Tensor], List[int], List[int]]:
        '''
        Build transfer data and offsets for MLA kv cache.
        '''
        k_tensors = []
        k_offsets = []
        k_cuda_blocks = []
        v_tensors = []
        v_offsets = []
        v_cuda_blocks = []
        kv_layer = self.kvcache[layer_id]

        for blk_id in token_slots.view(-1, block_size):
            cuda_k_block_id = self.cuda_block_pool.alloc(kv_layer[0][blk_id])
            cuda_k_block = self.cuda_block_pool.get_block(cuda_k_block_id)
            cuda_v_block_id = self.cuda_block_pool.alloc(kv_layer[1][blk_id])
            cuda_v_block = self.cuda_block_pool.get_block(cuda_v_block_id)
            offset_k, offset_v = self._data_offset_mha(kv_layer, layer_id, block_size)
            k_tensors.append(cuda_k_block)
            k_offsets.append(offset_k)
            k_cuda_blocks.append(cuda_k_block_id)
            v_tensors.append(cuda_v_block)
            v_offsets.append(offset_v)
            v_cuda_blocks.append(cuda_v_block_id)
        return k_tensors + v_tensors, k_offsets + v_offsets, k_cuda_blocks + v_cuda_blocks

    def _free_task_cuda_blocks(self, task: Task):
        cuda_blocks = self.task_to_cuda_blocks.pop(task.task_id, [])
        for cuda_block_id in cuda_blocks:
            self.cuda_block_pool.free(cuda_block_id)

    def start_load_kv(self, token_slots: torch.Tensor, request_id: str):
        req_status = self.req_status_dict.get(request_id, None)
        if not req_status:
            return
        block_hashes = req_status.block_hashes
        fetch_begin = req_status.fetch_index // self.block_size
        fetch_end = req_status.dump_index // self.block_size
        fetch_block_hashes = block_hashes[fetch_begin:fetch_end]

        for layer_id in range(len(self.kvcache)):
            if self.is_mla:
                tensors, offsets, cuda_blocks = self._build_transfer_data_mla(
                    layer_id, token_slots
                )
            else:
                tensors, offsets, cuda_blocks = self._build_transfer_data_mha(
                    layer_id, token_slots
                )
            task = self.connector.load(fetch_block_hashes * (1 if self.is_mla else 2), offsets, tensors)
            self.task_to_cuda_blocks[task.task_id] = cuda_blocks
            self.layerwise_load_tasks.setdefault(request_id, {})[layer_id] = task
        self.req_to_slots[request_id] = token_slots


    def wait_for_layer_load(self, layer_id: int):
        # self.layerwise_load_tasks: dict[str, dict[int, Task]] = {}
        # request_id layer_id task
        
        if not self.layerwise_load_tasks:
            return

        for request_id, layer_task_dict in list(self.layerwise_load_tasks.items()):
            task = layer_task_dict.pop(layer_id, None)
            if task is None:
                continue
            token_slots = self.req_to_slots[request_id]
            ret = self.connector.wait(task)

            cuda_blocks = self.task_to_cuda_blocks.get(task.task_id, [])
            kv_layer = self.kvcache[layer_id]

            block_ids = token_slots.view(-1, self.block_size) 
            num_blocks = block_ids.shape[0]
            device = kv_layer[0].device
            if self.is_mla:
                for cb, blk_id in zip(cuda_blocks, block_ids):
                    block = self.cuda_block_pool.get_block(cb).to(device)
                    kv_layer[0][blk_id] = block
            else:
                mid = num_blocks
                first_half = cuda_blocks[:mid]      # K blocks
                second_half = cuda_blocks[mid:]     # V blocks

                assert len(first_half) == len(second_half) == num_blocks
                for k_cb, v_cb, blk_id in zip(first_half, second_half, block_ids):
                    k_block = self.cuda_block_pool.get_block(k_cb).to(device)
                    v_block = self.cuda_block_pool.get_block(v_cb).to(device)
                    kv_layer[0][blk_id] = k_block
                    kv_layer[1][blk_id] = v_block
            
            self._free_task_cuda_blocks(task)

            if ret != 0:
                logger.error(
                    f"Wait load task failed for request {request_id}, layer {layer_id}."
                )

            if not layer_task_dict:
                self.layerwise_load_tasks.pop(request_id, None)

    def _layer_name_to_id(layer_name: str) -> int:
        """
        Extract the layer index from the module name.
        Examples:
        - "encoder.layers.0" -> 0
        - "encoder.layers.1.self_attn" -> 1
        - "2.self_attn" -> 2
        - "model.encoder.layers.0.sub.1" -> ValueError
        """
        subnames = layer_name.split(".")
        int_vals: list[int] = []
        for subname in subnames:
            try:
                int_vals.append(int(subname))
            except ValueError:
                continue
        assert len(int_vals) == 1, (
            f"layer name {layer_name} should" " only contain one integer"
        )
        return int_vals[0]
    def get_num_new_matched_tokens(
        self,
        request_id: str,
        token_ids: List[int],
        num_computed_tokens: int,
    ) -> int:
        # Preempted request handling
        # TODO

        assert num_computed_tokens % self.block_size == 0
        block_hashes = hash_request_tokens(md5, self.block_size, token_ids)
        if not block_hashes:
            return 0
        start_position = num_computed_tokens // self.block_size
        remain_hashes = block_hashes[start_position:]
        if not remain_hashes:
            return 0
        lookup_results = self.connector.lookup(remain_hashes)
        num_lookup_hits = 0
        for i, hit in enumerate(lookup_results):
            if hit:
                num_lookup_hits += 1
            else:
                break
        
        fetch_index = start_position * self.block_size
        dump_index = (start_position + num_lookup_hits) * self.block_size
        end_index = len(block_hashes) * self.block_size
        dump_hashes = block_hashes[dump_index // self.block_size:]
        
        create_ret = self.update_state_after_alloc(dump_hashes)

        if not create_ret:
            end_index = dump_index

        self.req_status_dict[request_id] = ReqStatus(
            block_hashes=block_hashes,
            fetch_index=fetch_index,
            dump_index=dump_index,
            end_index=end_index,
        )

        return num_lookup_hits * self.block_size

    def update_state_after_alloc(self, dump_hashes: list[str]) -> bool:
        if not dump_hashes:
            return True

        create_results = self.connector.create(dump_hashes)

        if any(ret != 0 for ret in create_results):
            logger.warning(f"\ncreate_results on storage: {create_results}\n")
            return False

        return True

    def submit_dump_tasks(self, layer_id: int):
        if self.is_mla and self.tp_rank != 0:
            return

        uc_transfer_metadata = self._transfer_metadata

        torch.cuda.current_stream().synchronize()

        for req_meta in uc_transfer_metadata.request_metadata_list:
            request_id = req_meta.request_id

            if request_id in self.prev_dump_blocks.keys():
                last_block_ids = self.prev_dump_block_ids[request_id]
                last_tensors = self.prev_dump_tensors[request_id]
                last_offsets = self.prev_dump_offsets[request_id]
                last_blocks = self.prev_dump_blocks[request_id]

                task = self.connector.dump(last_block_ids * (1 if self.is_mla else 2), last_offsets, last_tensors)
                self.task_to_cuda_blocks[task.task_id] = last_blocks

                for block_id in last_block_ids:
                    self.dump_tasks[req_meta.request_id][block_id].append(task)

            dump_items = req_meta.dump_items
            if len(dump_items) == 0:
                continue

            block_ids: list[str] = []
            cache_out_loc_list: list[torch.Tensor] = []
            for item in dump_items:
                block_ids.append(item.block_id)
                cache_out_loc_list.append(item.cache_out_loc)
            cache_out_locs = torch.cat(cache_out_loc_list, dim=0)

            if self.is_mla:
                tensors, offsets, blocks = self._build_transfer_data_mla(layer_id, cache_out_locs)
            else:
                tensors, offsets, blocks = self._build_transfer_data_mha(layer_id, cache_out_locs)
            
            if layer_id == self.num_layers - 1:
                print("come to last_layer")
                
                torch.cuda.current_stream().synchronize()

                task = self.connector.dump(block_ids * (1 if self.is_mla else 2), offsets, tensors)
                self.task_to_cuda_blocks[task.task_id] = blocks

                for block_id in block_ids:
                    self.dump_tasks[req_meta.request_id][block_id].append(task)

            self.prev_dump_block_ids[request_id] = block_ids
            self.prev_dump_tensors[request_id] = tensors
            self.prev_dump_offsets[request_id] = offsets
            self.prev_dump_blocks[request_id] = blocks

    def _find_block_index(self, req_status: ReqStatus, block_id: str) -> int:
        for i, h in enumerate(req_status.block_hashes):
            if h == block_id:
                return i
        return -1

    def _tp_reduce_blocks_all(
        self,
        local_success_by_req: dict[str, list[str]],
        local_fail_by_req: dict[str, list[str]],
    ) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
        if self.tp_group is None:
            if self.total_tp_size != 1:
                logger.error(
                    f"TP size: {self.total_tp_size}, but tp_group has not been initialized."
                )

            return local_success_by_req, local_fail_by_req

        request_ids = sorted(local_success_by_req.keys())

        success_masks = []
        fail_masks = []
        block_nums = []

        for request_id in request_ids:
            req_status = self.req_status_dict[request_id]
            block_ids = list(req_status.block_hashes)
            id2index = {block_id: i for i, block_id in enumerate(block_ids)}

            n = len(block_ids)
            block_nums.append(n)

            success_mask = torch.zeros(n, dtype=torch.int32, device="cuda")
            fail_mask = torch.zeros(n, dtype=torch.int32, device="cuda")

            for block_id in local_success_by_req.get(request_id, []):
                index = id2index.get(block_id, None)
                if index is not None:
                    success_mask[index] = 1

            for block_id in local_fail_by_req.get(request_id, []):
                index = id2index.get(block_id, None)
                if index is not None:
                    fail_mask[index] = 1

            success_masks.append(success_mask)
            fail_masks.append(fail_mask)

        success_mask_all = torch.cat(success_masks, dim=0)
        fail_mask_all = torch.cat(fail_masks, dim=0)

        dist.all_reduce(success_mask_all, op=dist.ReduceOp.MIN, group=self.tp_group)
        dist.all_reduce(fail_mask_all, op=dist.ReduceOp.MAX, group=self.tp_group)

        global_success_by_req: dict[str, list[str]] = {}
        global_fail_by_req: dict[str, list[str]] = {}

        offset = 0
        for request_id, n in zip(request_ids, block_nums):
            req_status = self.req_status_dict[request_id]
            block_ids = list(req_status.block_hashes)

            success_chunk = success_mask_all[offset: offset + n]
            fail_chunk = fail_mask_all[offset: offset + n]

            success_ids = []
            fail_ids = []

            for i, block_id in enumerate(block_ids):
                if success_chunk[i].item() == 1:
                    success_ids.append(block_id)
                if fail_chunk[i].item() == 1:
                    fail_ids.append(block_id)

            global_success_by_req[request_id] = success_ids
            global_fail_by_req[request_id] = fail_ids

            offset += n

        return global_success_by_req, global_fail_by_req

    def _update_dump_tasks(
        self,
        request_id: str,
        success_block_ids: list[str], 
        fail_block_ids: list[str],
    ):
        if fail_block_ids:
            if self.tp_rank == 0:
                self.connector.commit(fail_block_ids, False)
            for block_id in fail_block_ids:
                self.dump_tasks[request_id].pop(block_id, None)
                self.block_dump_status[request_id].pop(block_id, None)

        if success_block_ids:
            if self.tp_rank == 0:
                self.connector.commit(success_block_ids, True)
            for block_id in success_block_ids:
                self.dump_tasks[request_id].pop(block_id, None)
                self.block_dump_status[request_id].pop(block_id, None)

        if request_id in self.dump_tasks and not self.dump_tasks[request_id]:
            self.dump_tasks.pop(request_id, None)
            self.block_dump_status.pop(request_id, None)

    def handle_dump_tasks(self):
        if self.is_mla and self.tp_rank != 0:
            return

        # Note (pyxyzc): we cannot use _transfer_metadata here, cause the moment 
        # when the dump task fails and returns is not necessarily the same moment 
        # when handle_dump_tasks is invoked during the step in which the task was issued.

        request_ids = list(self.dump_tasks.keys())
        if not request_ids:
            return

        local_success_by_req: dict[str, list[str]] = {}
        local_fail_by_req: dict[str, list[str]] = {}

        for request_id in request_ids:
            block_dump_tasks = self.dump_tasks.get(request_id, {})
            if not block_dump_tasks:
                continue

            req_status = self.req_status_dict.get(request_id)
            if req_status is None:
                logger.error(
                    f"Request entered into unifiedcache connector without req status information: {request_id}."
                )
                continue

            success_tasks: list[int] = []

            local_success_block_ids: list[str] = []
            local_fail_block_ids: list[str] = []

            block_fail_index = -1
            block_ids = list(block_dump_tasks.keys())

            for block_id in block_ids:
                tasks = block_dump_tasks.get(block_id, [])
                block_index = self._find_block_index(req_status, block_id)

                if block_fail_index != -1 and block_index > block_fail_index:
                    local_fail_block_ids.append(block_id)
                    continue

                success_flag = True

                for task in tasks:
                    if task.task_id in success_tasks:
                        self.block_dump_status[request_id][block_id].append(task.task_id)
                        continue

                    ret, finished = self.connector.check(task)
                    if ret != 0:
                        logger.error(
                            f"Task {task} failed, check return {ret} for request {request_id}, block {block_id}."
                        )
                        success_flag = False
                        break
                    elif not finished:
                        continue

                    wret = self.connector.wait(task)
                    if wret != 0:
                        logger.error(
                            f"Task {task} failed, wait return {wret} for request {request_id}, block {block_id}."
                        )
                        success_flag = False
                        break
                    else:
                        success_tasks.append(task.task_id)
                        self._free_task_cuda_blocks(task)

                        self.block_dump_status[request_id][block_id].append(task.task_id)

                if not success_flag:
                    local_fail_block_ids.append(block_id)
                    req_status.end_index = block_index * self.block_size

                    if block_fail_index == -1:
                        block_fail_index = block_index

            expected_success_nums = self.num_layers
            for block_id, success_tasks in self.block_dump_status[request_id].items():
                success_nums = len(success_tasks)
                if success_nums >= expected_success_nums:
                    local_success_block_ids.append(block_id)

            local_success_by_req[request_id] = local_success_block_ids
            local_fail_by_req[request_id] = local_fail_block_ids

        global_success_by_req, global_fail_by_req = self._tp_reduce_blocks_all(
            local_success_by_req,
            local_fail_by_req,
        )

        for request_id in request_ids:
            final_success = global_success_by_req.get(request_id, [])
            final_fail = global_fail_by_req.get(request_id, [])
            self._update_dump_tasks(request_id, final_success, final_fail)


        self.prev_dump_block_ids.clear()
        self.prev_dump_tensors.clear()
        self.prev_dump_offsets.clear()
        self.prev_dump_blocks.clear()

    def _convert_len(self, len: int):
        assert len >= 0
        return (len // self.block_size) * self.block_size

    def build_connector_metadata(self, schedule_batch: ScheduleBatch) -> None:
        meta = UCTransferMetadata()

        running_offset = 0

        for i, req in enumerate(schedule_batch.reqs):
            req_id = req.rid
            req_status = self.req_status_dict.get(req_id)
            if req_status == None:
                continue

            assert req_status.fetch_index % self.block_size == 0, \
            f"fetch_index ({req_status.fetch_index}) must be block-aligned ({self.block_size})"
            assert req_status.dump_index % self.block_size == 0, \
            f"dump_index ({req_status.dump_index}) must be block-aligned ({self.block_size})"
            assert req_status.end_index % self.block_size == 0, \
            f"dump_index ({req_status.end_index}) must be block-aligned ({self.block_size})"

            prefix_len = schedule_batch.prefix_lens[i]
            extend_len = schedule_batch.extend_lens[i]
            prefix_begin = 0
            prefix_end   = prefix_begin + prefix_len
            extend_begin = prefix_end
            extend_end   = extend_begin + extend_len

            if extend_end < req_status.dump_index:
                continue
            
            fetch_items: list[FetchItem] = []
            dump_items: list[DumpItem] = []
            
            dump_begin = max(req_status.dump_index, extend_begin)
            dump_end = min(req_status.end_index, extend_end)

            if dump_end > dump_begin:
                block_nums = (dump_end - dump_begin) // self.block_size
                first_block = dump_begin // self.block_size
                for i in range(block_nums):
                    blk = first_block + i

                    cache_start = running_offset + blk * self.block_size - dump_begin
                    cache_end = cache_start + self.block_size

                    dump_items.append(
                        DumpItem(
                            block_id=req_status.block_hashes[blk],
                            start_token_id=blk * self.block_size,
                            end_token_id=(blk + 1) * self.block_size,
                            cache_out_loc=schedule_batch.out_cache_loc[cache_start:cache_end],
                        )
                    )

            req_transfer_meta = ReqTransferMetadata(
                request_id=req.rid,
                fetch_items=fetch_items,
                dump_items=dump_items,
            )
            meta.request_metadata_list.append(req_transfer_meta)

            running_offset += extend_len

        self._transfer_metadata = meta
