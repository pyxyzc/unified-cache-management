from dataclasses import dataclass, asdict, field
from enum import Enum
from typing import Optional

import torch
from sglang.srt.managers.schedule_batch import ScheduleBatch

from ucm.store.factory import UcmConnectorFactory
from ucm.store.ucmstore import Task
from dataclasses import dataclass, asdict
import torch
from typing import Optional


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


class BlockStatus(Enum):
    NONE = "none"
    FETCH = "fetch"
    DUMP = "dump"


@dataclass
class ReqStatus:
    block_hashes: list[str] = field(default_factory=list)
    block_status_list: list[BlockStatus] = field(default_factory=list)
    fetch_index: int = 0
    dump_index: int = 0
    prefix_begin_index: int = 0
    extend_begin_index: int = 0

@dataclass
class ReqTransferMetadata:
    request_id: str
    fetch_items: list[FetchItem] = field(default_factory=list)
    dump_items: list[DumpItem] = field(default_factory=list)


@dataclass
class UCTransferMetadata:
    requests: list[ReqTransferMetadata] = field(default_factory=list)


class UnifiedCacheConnector():
    def __init__(self, uc_connector_name: str, unifiedCacheConfig: UnifiedCacheConfig, environmentConfig: EnvironmentConfig):
        self.environmentConfig = environmentConfig
        self.tp_rank = unifiedCacheConfig.device
        unifiedCacheConfig_dict = asdict(unifiedCacheConfig)
        self.connector = UcmConnectorFactory.create_connector(uc_connector_name, unifiedCacheConfig_dict)

        self.block_size = unifiedCacheConfig.kv_block_size
        self.req_status_dict: dict[str, ReqStatus] = {}
        self.dump_tasks: dict[str, dict[str, list[Task]]] = {}

    def start_load_kv(self):
        pass


    def wait_for_layer_load(self):
        pass


    def save_kv_layer(self):
        pass


    def wait_for_save(self):
        pass

    def _convert_len(self, len: int):
        assert len >= 0
        return (len // self.block_size) * self.block_size

    def build_connector_metadata(self, schedule_batch: ScheduleBatch) -> UCTransferMetadata:
        meta = UCTransferMetadata()

        for i, req in enumerate(schedule_batch.reqs):
            req_id = req.rid
            req_status = self.req_status_dict[req_id]
            assert req_status.fetch_index % self.block_size == 0, \
            f"fetch_index ({req_status.fetch_index}) must be block-aligned ({self.block_size})"
            assert req_status.dump_index % self.block_size == 0, \
            f"dump_index ({req_status.dump_index}) must be block-aligned ({self.block_size})"

            prefix_len = schedule_batch.prefix_lens[i]
            extend_len = schedule_batch.extend_lens[i]
            prefix_begin = req_status.prefix_begin_index
            prefix_end   = prefix_begin + prefix_len
            extend_begin = req_status.extend_begin_index
            extend_end   = extend_begin + extend_len

            fetch_items = []
            dump_items = []

            fetch_begin = max(prefix_begin, req_status.fetch_index)
            fetch_end = min(prefix_end, req_status.dump_index)

            if fetch_end > fetch_begin:
                first_fetch_block = fetch_begin // self.block_size
                last_fetch_block_exclusive = (fetch_end + self.block_size - 1) // self.block_size
                for blk in range(first_fetch_block, last_fetch_block_exclusive):
                    fetch_items.append(
                        FetchItem(
                            block_id=req_status.block_hashes[blk],
                            start_token_id=blk * self.block_size,
                            end_token_id=(blk + 1) * self.block_size,
                        )
                    )

            assert extend_begin >= req_status.dump_index
            dump_len = self._convert_len(extend_len)
            dump_begin = extend_begin
            dump_end = extend_begin + dump_len

            if dump_end > dump_begin:
                first_dump_block = dump_begin // self.block_size
                last_dump_block_exclusive = dump_end // self.block_size
                for blk in range(first_dump_block, last_dump_block_exclusive):
                    dump_items.append(
                        DumpItem(
                            block_id=req_status.block_hashes[blk],
                            start_token_id=blk * self.block_size,
                            end_token_id=(blk + 1) * self.block_size,
                        )
                    )

            req_transfer_meta = ReqTransferMetadata(
                request_id=req.rid,
                fetch_items=fetch_items,
                dump_items=dump_items,
            )
            meta.requests.append(req_transfer_meta)

            req_status.prefix_begin_index = prefix_end
            req_status.extend_begin_index = extend_end

        return meta
