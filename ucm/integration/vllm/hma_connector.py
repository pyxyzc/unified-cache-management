import copy
import math
import os
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Optional, Sequence, Tuple

import numpy as np
import torch
from vllm.distributed.kv_transfer.kv_connector.v1.base import (
    KVConnectorMetadata,
    KVConnectorRole,
    SupportsHMA,
)
from vllm.model_executor.models.utils import extract_layer_index
from vllm.v1.core.sched.output import SchedulerOutput

from ucm.integration.vllm.device import create_device
from ucm.integration.vllm.ucm_connector import (
    UCMDirectConnector,
    _check_shm_capacity,
    _use_ucm_connector_cpu_affinity,
)
from ucm.logger import init_logger
from ucm.shared.metrics import ucmmetrics
from ucm.sparse.utils import round_up
from ucm.store.factory_v1 import UcmConnectorFactoryV1
from ucm.store.ucmstore_v1 import Task, UcmKVStoreBaseV1

if TYPE_CHECKING:
    from vllm.config import VllmConfig
    from vllm.forward_context import ForwardContext
    from vllm.v1.core.kv_cache_manager import KVCacheBlocks
    from vllm.v1.kv_cache_interface import KVCacheConfig
    from vllm.v1.request import Request

logger = init_logger(__name__)


def _fawa_trace(event: str, **fields: object) -> None:
    """Emit opt-in, structured breadcrumbs for the FAWA/DSV4 path.

    FAWA is exercised from both the scheduler and every worker.  Keeping the
    trace behind an environment switch makes it useful for bring-up without
    adding per-request log volume to normal serving runs.
    """

    if os.getenv("UCM_FAWA_TRACE", "").strip().lower() not in (
        "1",
        "true",
        "yes",
        "on",
    ):
        return
    details = ", ".join(f"{key}={value!r}" for key, value in fields.items())
    suffix = f": {details}" if details else ""
    logger.info(f"[FAWA_TRACE] {event}{suffix}")


def _preview_bytes(values: Sequence[bytes], limit: int = 3) -> list[str]:
    """Return short hash previews so traces do not dump complete cache keys."""

    return [value[:8].hex() for value in values[:limit]]


def _preview_ints(values: Sequence[int], limit: int = 8) -> list[int]:
    """Return a bounded preview of physical vLLM block ids."""

    return [int(value) for value in values[:limit]]


def _describe_cache_value(value: object) -> object:
    """Summarize a registered cache tensor without materializing its contents."""

    if isinstance(value, torch.Tensor):
        return {
            "shape": tuple(value.shape),
            "stride": tuple(value.stride()),
            "dtype": str(value.dtype),
            "device": str(value.device),
        }
    if isinstance(value, Tuple):
        return {
            "type": "tuple",
            "items": [_describe_cache_value(item) for item in value],
        }
    return {"type": type(value).__name__}


@dataclass(frozen=True)
class KVCacheGroupMeta:
    """Logical storage shape for one vLLM KV-cache group."""

    group_id: int
    token_block_size: int
    tail_blocks: int
    tail_tokens: int


class KVCacheGroupLayout:
    """Flat pointer layout for one vLLM KV cache group.

    The cache views belonging to one KV group are not necessarily contiguous by
    layer id, so this layout flattens all registered tensors in a deterministic
    order and records enough stride metadata to address arbitrary block rows.
    """

    def __init__(
        self,
        kvcaches: dict[str, torch.Tensor],
        *,
        is_ascend_layout: bool = False,
        expected_block_size: Optional[int] = None,
    ) -> None:
        self.kvcaches = dict(sorted(kvcaches.items(), key=self._sort_key))
        self.is_ascend_layout = is_ascend_layout
        self.expected_block_size = expected_block_size
        self.base_ptrs: np.ndarray
        self.block_strides: np.ndarray
        self.tensor_token_strides: np.ndarray
        self.tensor_sizes_per_token: np.ndarray
        self.tensor_block_sizes: np.ndarray
        self._build_layout()

    @staticmethod
    def _sort_key(item: tuple[str, torch.Tensor]) -> tuple[int, str]:
        name, _ = item
        return (extract_layer_index(name), name)

    def _is_combined_kv_4d(
        self,
        shape: Sequence[int],
        layer_name: str,
    ) -> bool:
        """Classify a 4-D cache without confusing block_size=2 with K/V."""

        if self.is_ascend_layout:
            if (
                self.expected_block_size is not None
                and shape[1] != self.expected_block_size
            ):
                raise ValueError(
                    f"Ascend KV cache tensor block size mismatch for {layer_name}: "
                    f"shape={tuple(shape)}, expected={self.expected_block_size}."
                )
            return False

        is_combined_kv = shape[1] == 2
        token_dim = 2 if is_combined_kv else 1
        if (
            self.expected_block_size is not None
            and shape[token_dim] != self.expected_block_size
        ):
            raise ValueError(
                f"GPU KV cache tensor block size mismatch for {layer_name}: "
                f"shape={tuple(shape)}, expected={self.expected_block_size}."
            )
        return is_combined_kv

    def _build_layout(self) -> None:
        """Flatten registered KV tensors into store-compatible pointer rows."""

        ptrs: list[int] = []
        strides: list[int] = []
        tensor_token_strides: list[int] = []
        tensor_sizes_per_token: list[int] = []
        tensor_block_sizes: list[int] = []
        view_meta: list[tuple[str, tuple[int, ...], tuple[int, ...], str, int]] = []

        def handle_tensor(
            t: torch.Tensor,
            size_dims: Sequence[int],
            layer_name: str,
        ) -> None:
            ptrs.append(t[0].data_ptr())
            strides.append(t.stride(0) * t.element_size())
            tensor_size = math.prod([t.shape[i] for i in size_dims]) * t.element_size()
            token_dim = 1
            tensor_block_size = int(t.shape[token_dim])
            tensor_token_strides.append(t.stride(token_dim) * t.element_size())
            tensor_sizes_per_token.append(tensor_size // tensor_block_size)
            tensor_block_sizes.append(tensor_block_size)
            view_meta.append(
                (
                    layer_name,
                    tuple(t.shape),
                    tuple(t.stride()),
                    str(t.dtype),
                    tensor_block_size,
                )
            )

        def handle_kv_layer_tensor(tensor: torch.Tensor, layer_name: str) -> None:
            if tensor.dim() == 5:
                # [2, num_blocks, block_size, num_head, head_dim]
                handle_tensor(tensor[0], (-3, -2, -1), layer_name)
                handle_tensor(tensor[1], (-3, -2, -1), layer_name)
            elif tensor.dim() == 4:
                if self._is_combined_kv_4d(tensor.shape, layer_name):
                    # GPU kernels may register [num_blocks, 2, block_size, ...];
                    # split the K/V axis before reading the token dimension.
                    handle_tensor(tensor[:, 0], (-2, -1), layer_name)
                    handle_tensor(tensor[:, 1], (-2, -1), layer_name)
                else:
                    # Ascend registers split KV/state tensors as
                    # [num_blocks, block_size, num_head, head_dim].
                    handle_tensor(tensor, (-3, -2, -1), layer_name)
            elif tensor.dim() == 3:
                # [num_blocks, block_size, head_dim]. Some DeepSeek V4 caches
                # use block_size=2 here and share a group with larger pages.
                handle_tensor(tensor, (-2, -1), layer_name)
            else:
                raise ValueError(
                    f"Unsupported KV cache tensor shape for "
                    f"{layer_name}: {tensor.shape}"
                )

        for layer_name, kv_layer in self.kvcaches.items():
            if isinstance(kv_layer, torch.Tensor):
                handle_kv_layer_tensor(kv_layer, layer_name)
            elif isinstance(kv_layer, Tuple):
                for tensor in kv_layer:
                    handle_kv_layer_tensor(tensor, layer_name)
            else:
                raise TypeError(
                    f"Unsupported KV cache type for " f"{layer_name}: {type(kv_layer)}"
                )

        if not ptrs:
            raise ValueError("KV cache group layout is empty.")

        self.base_ptrs = np.asarray(ptrs, dtype=np.uint64)
        self.block_strides = np.asarray(strides, dtype=np.uint64)
        self.tensor_token_strides = np.asarray(tensor_token_strides, dtype=np.uint64)
        self.tensor_sizes_per_token = np.asarray(
            tensor_sizes_per_token, dtype=np.uint64
        )
        self.tensor_block_sizes = np.asarray(tensor_block_sizes, dtype=np.uint64)
        self.view_meta = [
            {
                "name": name,
                "shape": shape,
                "stride": stride,
                "dtype": dtype,
                "tensor_block_size": tensor_block_size,
            }
            for name, shape, stride, dtype, tensor_block_size in view_meta
        ]
        logger.info(
            f"KV cache group layout: views={len(self.kvcaches)}, "
            f"ptrs={len(ptrs)}, "
            f"tensor_block_sizes={sorted(set(tensor_block_sizes))}"
        )
        _fawa_trace(
            "layout.built",
            is_ascend_layout=self.is_ascend_layout,
            expected_block_size=self.expected_block_size,
            cache_layer_count=len(self.kvcaches),
            view_count=len(self.view_meta),
            base_ptrs_shape=self.base_ptrs.shape,
            block_strides_shape=self.block_strides.shape,
            tensor_block_sizes=self.tensor_block_sizes.tolist(),
            views=self.view_meta[:4],
        )

    def extract_addrs_with_offsets(
        self,
        block_ids: np.ndarray,
        group_token_block_size: int,
        offsets: np.ndarray,
    ) -> np.ndarray:
        """Return per-view addresses for logical blocks with token offsets."""

        physical_token_offsets = (
            offsets[:, None]
            * self.tensor_block_sizes[None, :]
            // group_token_block_size
        )

        return (
            block_ids[:, None] * self.block_strides[None, :]
            + physical_token_offsets * self.tensor_token_strides[None, :]
            + self.base_ptrs[None, :]
        ).astype(np.uint64, copy=False)

    def extract_addrs(
        self,
        block_ids: np.ndarray,
    ) -> np.ndarray:
        """Return per-view base addresses for complete tensor blocks."""

        return (
            block_ids[:, None] * self.block_strides[None, :] + self.base_ptrs[None, :]
        ).astype(np.uint64, copy=False)

    def segment_tensor_size_list(
        self,
        logical_tokens: int,
        group_token_block_size: int,
    ) -> list[int]:
        """Return byte sizes for one logical segment across all tensor views."""

        tensor_tokens = (
            self.tensor_block_sizes * logical_tokens // group_token_block_size
        )
        return (self.tensor_sizes_per_token * tensor_tokens).tolist()

    @property
    def tensor_block_size(self) -> int:
        if len(set(self.tensor_block_sizes.tolist())) != 1:
            raise ValueError(
                "KV cache group layout has mixed view tensor block sizes: "
                f"{self.tensor_block_sizes.tolist()}"
            )
        return int(self.tensor_block_sizes[0])


@dataclass
class FAWARequestMeta:
    """Scheduler-side state accumulated for one request."""

    ucm_block_ids: list[bytes] = field(default_factory=list)
    hbm_hit_block_num: int = 0
    total_hit_block_num: int = 0
    num_token_ids: int = 0
    vllm_block_ids: tuple[list[int], ...] = field(default_factory=tuple)
    token_processed: int = 0


@dataclass
class FAWARequestDispatchMeta:
    """Per-step load and dump plan sent from scheduler to workers."""

    load_keys: list[bytes] = field(default_factory=list)
    load_hash_start: int = 0
    load_hash_end: int = 0
    load_vllm_block_ids: tuple[list[int], ...] = field(default_factory=tuple)
    dump_keys: list[bytes] = field(default_factory=list)
    dump_hash_start: int = 0
    dump_hash_end: int = 0
    dump_vllm_block_ids: tuple[list[int], ...] = field(default_factory=tuple)


@dataclass
class UCMFAWAConnectorMetadata(KVConnectorMetadata):
    """Connector metadata carrying FAWA dispatch plans for this step."""

    request_meta: dict[str, FAWARequestDispatchMeta] = field(default_factory=dict)
    preempted_req_ids: set[str] = field(default_factory=set)


@dataclass
class FAWALoadTask:
    """Outstanding FAWA load task plus scheduler-visible failure anchors."""

    request_id: str
    label: str
    store: UcmKVStoreBaseV1
    task: Task
    key_count: int


@dataclass
class FAWADumpTask:
    """Outstanding FAWA dump task submitted to one backing store."""

    label: str
    store: UcmKVStoreBaseV1
    task: Task
    key_count: int
    event_handle: int


class UCMFAWAConnector(UCMDirectConnector, SupportsHMA):
    """UCM connector for mixed full-attention and window KV cache groups.

    Full-attention groups are stored once per reusable prefix block and are
    loaded for every external prefix hit. WA groups store the tail blocks
    needed at each prefix boundary, and only the final matched boundary is
    loaded.
    """

    DEFAULT_HASH_BLOCK_SIZE = 256
    ASCEND_SUPPORTED_VLLM_BLOCK_SIZES = frozenset({32, 64, 128})
    ASCEND_C4_COMPRESS_RATIO = 4

    def __init__(
        self,
        vllm_config: "VllmConfig",
        role: KVConnectorRole,
        kv_cache_config: "KVCacheConfig",
    ):
        self._defer_scheduler_store = True
        super().__init__(vllm_config, role, kv_cache_config)
        model_config = getattr(vllm_config, "model_config", None)
        _fawa_trace(
            "connector.init.start",
            role=str(role),
            model=getattr(model_config, "model", None),
            model_type=getattr(
                getattr(model_config, "hf_config", None), "model_type", None
            ),
            device_id=self.device_id,
            tp_rank=self.tp_rank,
            tp_size=self.tp_size,
            cache_block_size=self.block_size,
            is_mla=self.is_mla,
            unique_id_shared=bool(
                self.connector_configs[0]
                .get("ucm_connector_config", {})
                .get("share_buffer_enable", self.is_mla)
            ),
        )
        self.hash_block_size = self.DEFAULT_HASH_BLOCK_SIZE
        self.group_layouts: dict[int, KVCacheGroupLayout] = {}
        if self._kv_cache_config is None:
            raise RuntimeError("FAWA connector requires kv_cache_config.")

        self.is_ascend_layout = False
        self.ascend_base_block_size: Optional[int] = None
        self.fa_group_ids, self.window_group_ids = [], []
        self.group_metas: dict[int, KVCacheGroupMeta] = {}
        self.file_size = {}

        # The maximum token block size across all groups, used for aligning the number of computed tokens in the scheduler.
        self.max_token_block_size = 0
        self._init_group_metas()
        self.fa_store: Optional[UcmKVStoreBaseV1] = None
        self.wa_store: Optional[UcmKVStoreBaseV1] = None
        self.requests_meta: dict[str, FAWARequestMeta] = {}
        self.tp_dump_tasks: dict[tuple, list[FAWADumpTask]] = {}
        self.wa_dump_block_wise = self.launch_config.get("wa_dump_block_wise", True)

        # If the number of external hit blocks is small, it's possible that the load overhead is larger than the compute of a few blocks.
        # In that case, we can skip loading and directly compute the missed blocks, which can be faster.
        # This threshold can be tuned based on the performance characteristics of the system.
        self.load_tokens_threshold = self.launch_config.get(
            "load_tokens_threshold", 2048
        )

        if role == KVConnectorRole.SCHEDULER:
            self.fa_store = self._create_fa_store(None)
            self.wa_store = self._create_wa_store(None)

        group_meta_summary = tuple(
            {
                "group_id": meta.group_id,
                "token_block_size": meta.token_block_size,
                "tail_blocks": meta.tail_blocks,
                "tail_tokens": meta.tail_tokens,
            }
            for _, meta in sorted(self.group_metas.items())
        )
        logger.info(
            f"FAWA KV group config: fa_groups={self.fa_group_ids}, "
            f"window_groups={self.window_group_ids}, "
            f"hash_block_size={self.hash_block_size}, "
            f"ascend_base_block_size={self.ascend_base_block_size}, "
            f"is_ascend_layout={self.is_ascend_layout}, "
            f"group_metas={group_meta_summary}"
        )
        _fawa_trace(
            "connector.init.done",
            role=str(role),
            hash_block_size=self.hash_block_size,
            max_token_block_size=self.max_token_block_size,
            fa_group_ids=self.fa_group_ids,
            window_group_ids=self.window_group_ids,
            group_metas=group_meta_summary,
            file_size=self.file_size,
            scheduler_stores_created=role == KVConnectorRole.SCHEDULER,
            persist_token_threshold=self.persist_token_threshold,
            load_tokens_threshold=self.load_tokens_threshold,
            wa_dump_block_wise=self.wa_dump_block_wise,
        )
        logger.info("Init UCM FAWA connector.")

    def get_block_size(self) -> int:
        return self.hash_block_size

    @classmethod
    def can_handle_kv_cache_config(
        cls, kv_cache_config: Optional["KVCacheConfig"]
    ) -> bool:
        """Return whether this connector supports the given hybrid KV layout."""

        if kv_cache_config is None:
            return False

        kv_cache_groups = kv_cache_config.kv_cache_groups
        spec_names = set()
        for group_spec in kv_cache_groups:
            nested_specs = getattr(group_spec.kv_cache_spec, "kv_cache_specs", None)
            spec = (
                next(iter(nested_specs.values()))
                if nested_specs
                else group_spec.kv_cache_spec
            )
            spec_names.add(type(spec).__name__)
        # GPU FAWA is currently selected for DeepSeek-V4 style MLA+SWA layouts.
        DS_V4_REQUIRED_SPECS = frozenset({"SlidingWindowMLASpec"})
        gpu_support = DS_V4_REQUIRED_SPECS.issubset(spec_names)
        if gpu_support:
            _fawa_trace(
                "can_handle_kv_cache_config",
                path="gpu",
                group_count=len(kv_cache_groups),
                spec_names=sorted(spec_names),
                required_specs=sorted(DS_V4_REQUIRED_SPECS),
                selected=True,
            )
            return True
        ascend_support = cls.can_handle_ascend_kv_cache_config(kv_cache_config)
        _fawa_trace(
            "can_handle_kv_cache_config",
            path="ascend" if ascend_support else "unsupported",
            group_count=len(kv_cache_groups),
            spec_names=sorted(spec_names),
            required_specs=sorted(DS_V4_REQUIRED_SPECS),
            selected=ascend_support,
        )
        return ascend_support

    @classmethod
    def can_handle_ascend_kv_cache_config(
        cls, kv_cache_config: Optional["KVCacheConfig"]
    ) -> bool:
        """Return whether the KV config matches the supported Ascend FAWA layout."""

        if kv_cache_config is None:
            return False
        kv_cache_groups = kv_cache_config.kv_cache_groups
        spec_names = set()
        for group_spec in kv_cache_groups:
            nested_specs = getattr(group_spec.kv_cache_spec, "kv_cache_specs", None)
            spec = (
                next(iter(nested_specs.values()))
                if nested_specs
                else group_spec.kv_cache_spec
            )
            spec_names.add(type(spec).__name__)
        ASCEND_REQUIRED_SPECS = frozenset({"AscendSlidingWindowMLASpec"})
        npu_support = ASCEND_REQUIRED_SPECS.issubset(spec_names)
        _fawa_trace(
            "can_handle_ascend_kv_cache_config",
            group_count=len(kv_cache_groups),
            spec_names=sorted(spec_names),
            required_specs=sorted(ASCEND_REQUIRED_SPECS),
            selected=npu_support,
        )
        return npu_support

    @classmethod
    def _get_ascend_base_block_size(cls, kv_cache_config: "KVCacheConfig") -> int:
        """Read the user-scale block size from the Ascend C4 FA group.

        The scheduler mutates ``vllm_config.cache_config.block_size`` to the
        smallest hybrid-group block size, while workers retain the configured
        value. The C4 full-attention group is present in both roles and keeps
        the original 32/64/128 block size, so it is the stable source here.
        """

        c4_block_sizes = set()
        for group in kv_cache_config.kv_cache_groups:
            kv_cache_spec = group.kv_cache_spec
            nested_specs = getattr(kv_cache_spec, "kv_cache_specs", None)
            spec = next(iter(nested_specs.values())) if nested_specs else kv_cache_spec
            if (
                getattr(spec, "sliding_window", None) is None
                and getattr(spec, "compress_ratio", 1) == cls.ASCEND_C4_COMPRESS_RATIO
            ):
                c4_block_sizes.add(kv_cache_spec.block_size)

        if len(c4_block_sizes) != 1:
            raise ValueError(
                "Expected exactly one Ascend C4 full-attention block size, "
                f"got {sorted(c4_block_sizes)}."
            )
        c4_block_size_candidates = sorted(c4_block_sizes)
        block_size = c4_block_sizes.pop()

        if block_size not in cls.ASCEND_SUPPORTED_VLLM_BLOCK_SIZES:
            supported = sorted(cls.ASCEND_SUPPORTED_VLLM_BLOCK_SIZES)
            raise ValueError(
                f"Unsupported DeepSeek V4 Ascend block size {block_size}; "
                f"expected one of {supported}."
            )
        _fawa_trace(
            "ascend.base_block_size",
            c4_block_sizes=c4_block_size_candidates,
            selected_block_size=block_size,
            hash_block_size=block_size * cls.ASCEND_C4_COMPRESS_RATIO,
        )
        return block_size

    def _init_group_metas(self) -> None:
        """Classify FA/WA groups and compute their logical segment sizes."""

        if self.can_handle_ascend_kv_cache_config(self._kv_cache_config):
            self.is_ascend_layout = True
            self.ascend_base_block_size = self._get_ascend_base_block_size(
                self._kv_cache_config
            )
            self.hash_block_size = (
                self.ascend_base_block_size * self.ASCEND_C4_COMPRESS_RATIO
            )

        groups = self._kv_cache_config.kv_cache_groups
        self.fa_group_ids, self.window_group_ids = [], []
        layer_compress_ratios = getattr(
            self._vllm_config.model_config.hf_config,
            "compress_ratios",
            None,
        )
        if layer_compress_ratios is None:
            raise ValueError("current only support DSV4")
        _fawa_trace(
            "group_meta.init",
            group_count=len(groups),
            is_ascend_layout=self.is_ascend_layout,
            ascend_base_block_size=self.ascend_base_block_size,
            hash_block_size=self.hash_block_size,
            compress_ratio_count=len(layer_compress_ratios),
            compress_ratio_preview=list(layer_compress_ratios[:8]),
        )
        for group_id, group in enumerate(groups):
            kv_cache_spec = group.kv_cache_spec
            # Use the representative spec when vLLM wraps multiple layer specs.
            nested_specs = getattr(kv_cache_spec, "kv_cache_specs", None)
            spec = next(iter(nested_specs.values())) if nested_specs else kv_cache_spec
            window_size = getattr(spec, "sliding_window", None)
            compress_ratio = getattr(spec, "compress_ratio", 1)
            token_block_size = kv_cache_spec.block_size
            if self.is_ascend_layout:
                # Ascend compressed groups expose a logical block span scaled by
                # the compression ratio.
                token_block_size = kv_cache_spec.block_size * compress_ratio

            if window_size is None:
                # FA groups store one canonical hash block per row.
                tail_tokens = self.hash_block_size
                self.fa_group_ids.append(group_id)
            else:
                tensor_name = group.layer_names[0]
                if tensor_name.split(".")[-1] in ["swa_cache"]:
                    # SWA caches keep the full sliding-window tail.
                    tail_tokens = window_size
                else:
                    # Compressor state caches keep only the uncompressed tail.
                    layer_index = extract_layer_index(tensor_name)
                    tail_tokens = window_size - layer_compress_ratios[layer_index]

                tail_blocks = tail_tokens // token_block_size
                self.window_group_ids.append(group_id)

            tail_blocks = max(tail_tokens // token_block_size, 1)
            self.max_token_block_size = max(self.max_token_block_size, token_block_size)
            self.group_metas[group_id] = KVCacheGroupMeta(
                group_id=group_id,
                token_block_size=token_block_size,
                tail_blocks=tail_blocks,
                tail_tokens=tail_tokens,
            )
            _fawa_trace(
                "group_meta.classified",
                group_id=group_id,
                layer_count=len(group.layer_names),
                layer_name=group.layer_names[0] if group.layer_names else None,
                spec_type=type(spec).__name__,
                kind="FA" if window_size is None else "WA",
                window_size=window_size,
                compress_ratio=compress_ratio,
                vllm_block_size=kv_cache_spec.block_size,
                logical_token_block_size=token_block_size,
                tail_tokens=tail_tokens,
                tail_blocks=tail_blocks,
            )
        logger.info_once(
            f"max token_block_size of all groups: {self.max_token_block_size}"
        )
        if self.max_token_block_size % self.hash_block_size != 0:
            raise ValueError(
                f"Maximum token block size {self.max_token_block_size} must be "
                f"divisible by hash block size {self.hash_block_size}."
            )
        # get file size for block gc
        if len(layer_compress_ratios) < 61:
            # for dsv4 flash
            num_c4a_layers = 21
            num_c128a_layers = 20
            # TODO only support for dp tp
            num_total_layers = 43
        else:
            # for dsv4 pro
            num_c4a_layers = 30
            num_c128a_layers = 31
            num_total_layers = 61

        if (
            self._vllm_config.speculative_config is not None
            and self._vllm_config.speculative_config.num_speculative_tokens > 0
        ):
            num_total_layers += 1

        # TODO we should get file size in worker thread
        if self.is_ascend_layout:
            if self.ascend_base_block_size is None:
                raise RuntimeError("Ascend base block size was not initialized.")
            # One C4 row consumes a complete physical block. One C128 row
            # consumes block_size / 32 physical tokens, so both contributions
            # scale linearly with the configured vLLM block size.
            c4a_bytes_per_block_token = 1024 + 128 + 2
            c128a_bytes_per_block_token = 32
            self.file_size["FA"] = self.ascend_base_block_size * (
                c4a_bytes_per_block_token * num_c4a_layers
                + c128a_bytes_per_block_token * num_c128a_layers
            )
            self.file_size["WA"] = (
                131072 * num_total_layers + (32768 + 8192) * num_c4a_layers
            )
        else:
            self.file_size["FA"] = (
                37376 + 8448
            ) * num_c4a_layers + 1168 * num_c128a_layers
            self.file_size["WA"] = (37376 * 2) * num_total_layers + (
                8192 + 32768
            ) * num_c4a_layers
        self.file_size["FA"] = round_up(self.file_size["FA"], 4096)
        self.file_size["WA"] = round_up(self.file_size["WA"], 4096)
        _fawa_trace(
            "group_meta.file_sizes",
            model_variant="dsv4_flash" if len(layer_compress_ratios) < 61 else "dsv4_pro",
            layer_compress_ratio_count=len(layer_compress_ratios),
            num_total_layers=num_total_layers,
            speculative_tokens=(
                getattr(self._vllm_config.speculative_config, "num_speculative_tokens", 0)
                if self._vllm_config.speculative_config is not None
                else 0
            ),
            file_size=self.file_size,
            max_token_block_size=self.max_token_block_size,
        )

    def _create_fa_store(
        self,
        group_layouts: Optional[dict[int, KVCacheGroupLayout]],
        cpu_affinity_cores: Optional[list[int]] = None,
    ) -> UcmKVStoreBaseV1:
        """Create the backing store used for full-attention rows."""

        tensor_size_list = None
        if self._role == KVConnectorRole.WORKER:
            if group_layouts is None:
                raise RuntimeError("Worker FA store needs layouts.")
            tensor_size_list = self._store_tensor_size_list(
                group_layouts,
                self.fa_group_ids,
            )
        return self._create_store(
            label="FA",
            store_suffix="fa",
            tensor_size_list=tensor_size_list,
            cpu_affinity_cores=cpu_affinity_cores,
        )

    def _create_wa_store(
        self,
        group_layouts: Optional[dict[int, KVCacheGroupLayout]],
        cpu_affinity_cores: Optional[list[int]] = None,
    ) -> UcmKVStoreBaseV1:
        """Create the backing store used for window-tail rows."""

        tensor_size_list = None
        if self._role == KVConnectorRole.WORKER:
            if group_layouts is None:
                raise RuntimeError("Worker WA store needs layouts.")
            tensor_size_list = self._store_tensor_size_list(
                group_layouts,
                self.window_group_ids,
            )
        return self._create_store(
            label="WA",
            store_suffix="wa",
            tensor_size_list=tensor_size_list,
            cpu_affinity_cores=cpu_affinity_cores,
        )

    def _base_store_config(
        self,
        store_suffix: str,
    ) -> tuple[str, Optional[str], dict[str, object]]:
        """Build a namespaced UCM store config for either FA or WA data."""

        if len(self.connector_configs) != 1:
            raise RuntimeError(
                f"Expected exactly one connector config, "
                f"but got {len(self.connector_configs)}: "
                f"{self.connector_configs}"
            )

        name = self.connector_configs[0]["ucm_connector_name"]
        module_path = self.connector_configs[0].get("ucm_connector_module_path", None)
        config = copy.deepcopy(self.connector_configs[0]["ucm_connector_config"])
        config.setdefault("store_pipeline", "Cache|Empty")
        # MLA ranks share one logical store buffer; non-MLA stores are per rank.
        config.setdefault("share_buffer_enable", self.is_mla)
        if isinstance(config.get("storage_backends"), str):
            config["storage_backends"] = [
                path for path in config["storage_backends"].split(":")
            ]
        config["unique_id"] = f"{self.unique_id}_fawa_{store_suffix}"
        self._namespace_storage_backends(config, store_suffix)
        dp_rank = self._vllm_config.parallel_config.data_parallel_rank
        config["posix_gc_enable"] = (
            self._role != KVConnectorRole.WORKER and dp_rank == 0
        )
        if config.get("posix_capacity_gb", None) is not None:
            config["posix_capacity_gb"] = int(config["posix_capacity_gb"]) // 2
        return name, module_path, config

    def _set_default_shm_buffer_capacity(self, config: dict[str, object]) -> None:
        if not bool(config.get("share_buffer_enable", False)):
            return

        # HMA creates two shared-buffer stores, FA and WA, so split the
        # shared-buffer capacity evenly between them, whether user-set or the
        # 128GB direct-connector default.
        if config.get("cache_buffer_capacity_gb") is None:
            config["cache_buffer_capacity_gb"] = 128
        capacity = int(config["cache_buffer_capacity_gb"])
        config["cache_buffer_capacity_gb"] = max(capacity // 2, 1)
        logger.info(
            f"Set FAWA cache_buffer_capacity_gb to "
            f"{config['cache_buffer_capacity_gb']}GB by splitting the "
            f"{capacity}GB shared-buffer capacity across FA/WA stores."
        )
        # The shared buffer is allocated via shm_open in /dev/shm; fail early
        # (before store creation) if the tmpfs cannot hold the FA+WA total.
        _check_shm_capacity(capacity)

    @staticmethod
    def _namespace_storage_backends(
        config: dict[str, object],
        store_suffix: str,
    ) -> None:
        """Place FA and WA store files in separate backend subdirectories."""

        backends = config.get("storage_backends")
        if not isinstance(backends, list):
            return
        namespaced_backends: list[str] = []
        for backend in backends:
            backend_path = os.path.join(str(backend), f"fawa_{store_suffix}")
            os.makedirs(backend_path, exist_ok=True)
            namespaced_backends.append(backend_path)
        config["storage_backends"] = namespaced_backends

    def _create_store(
        self,
        label: str,
        store_suffix: str,
        tensor_size_list: Optional[list[int]],
        cpu_affinity_cores: Optional[list[int]] = None,
    ) -> UcmKVStoreBaseV1:
        """Instantiate one UCM store with worker tensor layout metadata."""

        name, module_path, config = self._base_store_config(store_suffix)
        self._set_default_shm_buffer_capacity(config)
        if label == "FA":
            config.setdefault("cache_io_aggregation", True)
        else:
            config["cache_io_aggregation"] = False
        if self._role == KVConnectorRole.WORKER:
            if tensor_size_list is None:
                raise RuntimeError(f"Worker FAWA {label} store needs tensor sizes.")
            config["device_id"] = self.device_id
            config["tensor_size_list"] = tensor_size_list
            # io_direct requires shard and block sizes to be 4KB aligned.
            aligned_size = 4096
            padded_size = round_up(sum(tensor_size_list), aligned_size)
            config["shard_size"] = padded_size
            config["block_size"] = padded_size
            if self.file_size[label] != padded_size:
                logger.info_once(
                    f"GC file size of {label} does not match real file size. "
                    f"Worker: {padded_size}, Scheduler: {self.file_size[label]}"
                )
            # MLA stores aggregate TP shards under one logical rank group.
            config["local_rank_size"] = self.tp_size if self.is_mla else 1
            if cpu_affinity_cores:
                config["cpu_affinity_cores"] = list(cpu_affinity_cores)
        else:
            config["block_size"] = self.file_size[label]
        _fawa_trace(
            "store.create",
            label=label,
            role=str(self._role),
            connector_name=name,
            store_suffix=store_suffix,
            device_id=config.get("device_id"),
            tensor_count=len(tensor_size_list) if tensor_size_list is not None else None,
            tensor_bytes=(
                sum(tensor_size_list) if tensor_size_list is not None else None
            ),
            shard_size=config.get("shard_size"),
            block_size=config.get("block_size"),
            gc_file_size=self.file_size[label],
            local_rank_size=config.get("local_rank_size"),
            share_buffer_enable=config.get("share_buffer_enable"),
            storage_backends=config.get("storage_backends"),
        )
        logger.info(
            f"create FAWA {label} {name} with config: "
            f"{self._summarize_store_config(config)}"
        )
        return UcmConnectorFactoryV1.create_connector(name, config, module_path)

    @staticmethod
    def _summarize_store_config(config: dict[str, object]) -> dict[str, object]:
        """Return a log-friendly store config without dumping large size lists."""

        summary = dict(config)
        tensor_size_list = summary.pop("tensor_size_list", None)
        if tensor_size_list is not None:
            tensor_sizes = [int(size) for size in tensor_size_list]
            summary["tensor_count"] = len(tensor_sizes)
            summary["tensor_bytes"] = sum(tensor_sizes)
        return summary

    def register_kv_caches(self, kv_caches: dict[str, torch.Tensor]):
        self.kv_caches = kv_caches
        self.device = create_device()

        enable_affinity = _use_ucm_connector_cpu_affinity()
        worker_cores, store_cores = (
            self.device.split_cores(self.device_id) if enable_affinity else (None, None)
        )
        _fawa_trace(
            "register_kv_caches.start",
            role=str(self._role),
            device_id=self.device_id,
            device=str(self.device),
            cache_entry_count=len(kv_caches),
            group_count=len(self._kv_cache_config.kv_cache_groups),
            is_ascend_layout=self.is_ascend_layout,
            cpu_affinity_enabled=enable_affinity,
            worker_cores=worker_cores,
            store_cores=store_cores,
        )

        for group_id, group_spec in enumerate(self._kv_cache_config.kv_cache_groups):
            group_caches: dict[str, torch.Tensor] = {}
            for layer_name in group_spec.layer_names:
                if isinstance(kv_caches[layer_name], torch.Tensor):
                    group_caches[layer_name] = kv_caches[layer_name]
                else:
                    group_caches[layer_name] = tuple(kv_caches[layer_name])
            layout = KVCacheGroupLayout(
                group_caches,
                is_ascend_layout=self.is_ascend_layout,
                expected_block_size=group_spec.kv_cache_spec.block_size,
            )
            self.group_layouts[group_id] = layout
            first_layer_names = group_spec.layer_names[:3]
            cache_descriptions = {
                layer_name: _describe_cache_value(group_caches[layer_name])
                for layer_name in first_layer_names
            }
            _fawa_trace(
                "register_kv_caches.group",
                group_id=group_id,
                layer_count=len(group_spec.layer_names),
                layer_preview=first_layer_names,
                cache_preview=cache_descriptions,
                layout_view_count=len(layout.view_meta),
                layout_tensor_block_size=layout.tensor_block_sizes.tolist(),
                layout_view_preview=layout.view_meta[:3],
            )

        self.store = self._create_fa_store(self.group_layouts, store_cores)
        self.fa_store = self.store
        self.wa_store = self._create_wa_store(self.group_layouts, store_cores)
        _fawa_trace(
            "register_kv_caches.done",
            group_layout_ids=sorted(self.group_layouts),
            fa_store_ready=self.fa_store is not None,
            wa_store_ready=self.wa_store is not None,
        )

        if worker_cores:
            try:
                os.sched_setaffinity(0, worker_cores)
                logger.info(f"[VLLM CPU Affinity] Worker bound to cores {worker_cores}")
            except Exception as e:
                logger.warning(f"Failed to bind worker: {e}")

    def _store_tensor_size_list(
        self,
        group_layouts: dict[int, KVCacheGroupLayout],
        group_ids: tuple[int, ...],
    ) -> list[int]:
        """Build the per-tensor byte-size vector expected by UCM stores."""

        tensor_size_list: list[int] = []
        for group_id in group_ids:
            layout = group_layouts.get(group_id)
            if layout is None:
                continue
            meta = self.group_metas[group_id]

            if not meta.tail_tokens:
                continue

            segment_tokens = meta.tail_tokens // meta.tail_blocks

            for _ in range(meta.tail_blocks):
                segment_sizes = layout.segment_tensor_size_list(
                    segment_tokens,
                    meta.token_block_size,
                )
                tensor_size_list.extend(segment_sizes)
        if not tensor_size_list:
            group_label = (
                "FA"
                if group_ids == self.fa_group_ids
                else "WA" if group_ids == self.window_group_ids else str(group_ids)
            )
            raise RuntimeError(f"Worker FAWA {group_label} layout is empty.")
        _fawa_trace(
            "store.tensor_sizes",
            group_ids=group_ids,
            tensor_count=len(tensor_size_list),
            total_bytes=sum(tensor_size_list),
            first_sizes=tensor_size_list[:8],
        )
        return tensor_size_list

    def _lookup_external_hit_blocks(self, external_keys: list[bytes]) -> int:
        """Find the longest reusable prefix present in both FA and WA stores."""

        _fawa_trace(
            "external_lookup.start",
            external_key_count=len(external_keys),
            key_preview=_preview_bytes(external_keys),
        )
        if self.fa_store is None:
            raise RuntimeError("FA store is not initialized.")
        if self.wa_store is None:
            raise RuntimeError("WA store is not initialized.")
        fa_hit_blocks = (
            self._rank_consistency.lookup_on_prefix(self.fa_store, external_keys) + 1
        )
        _fawa_trace(
            "external_lookup.fa",
            fa_hit_blocks=fa_hit_blocks,
            searched_key_count=len(external_keys),
        )
        if fa_hit_blocks <= 0:
            _fawa_trace("external_lookup.done", hit_blocks=0, reason="FA miss")
            return 0

        # WA rows represent window boundary state, so they are not required to
        # form a prefix. Search only inside the FA-contiguous hit range and use
        # the latest boundary that exists.
        wa_keys = external_keys[:fa_hit_blocks]
        reverse_idx = self._rank_consistency.lookup_on_reverse(self.wa_store, wa_keys)
        _fawa_trace(
            "external_lookup.wa",
            wa_candidate_key_count=len(wa_keys),
            reverse_idx=reverse_idx,
            key_preview=_preview_bytes(wa_keys),
        )
        if reverse_idx < 0:
            _fawa_trace("external_lookup.done", hit_blocks=0, reason="WA miss")
            return 0
        hit_blocks = reverse_idx + 1
        _fawa_trace("external_lookup.done", hit_blocks=hit_blocks)
        return hit_blocks

    def _prefetch_hit_key_hotness(
        self,
        fa_hbm_hit_keys: list[bytes],
        all_hit_keys: list[bytes],
    ) -> None:
        """Best-effort GC hotness update for FAWA's two backing stores.

        External FA keys are already touched by ``lookup_on_prefix``, so the
        FA store only needs the local-HBM prefix that lookup did not inspect.
        WA uses ``lookup_on_reverse``, which touches only the selected boundary;
        update every reusable boundary key so sparse WA snapshots age together
        with the corresponding FA prefix. Missing sparse WA files are harmless:
        store ``prefetch`` is a hint and Posix simply ignores a failed ``utime``.
        """

        if self.fa_store is None:
            raise RuntimeError("FA store is not initialized.")
        if self.wa_store is None:
            raise RuntimeError("WA store is not initialized.")

        updates = (
            ("FA", self.fa_store, fa_hbm_hit_keys),
            ("WA", self.wa_store, all_hit_keys),
        )
        for label, store, keys in updates:
            if not keys:
                continue
            _fawa_trace(
                "hotness.prefetch",
                label=label,
                key_count=len(keys),
                key_preview=_preview_bytes(keys),
            )
            try:
                store.prefetch(keys)
            except Exception as e:
                # Prefetch is only a GC hotness hint. A failure must not turn a
                # valid cache hit into a scheduler-side miss.
                logger.warning(
                    f"FAWA {label} hotness update failed. " f"{type(e).__name__}: {e}"
                )

    def get_num_new_matched_tokens(
        self,
        request: "Request",
        num_computed_tokens: int,
    ) -> tuple[int, bool]:
        wa_hbm_hit_block_num = num_computed_tokens // self.hash_block_size
        wa_computed_tokens = wa_hbm_hit_block_num * self.hash_block_size
        _fawa_trace(
            "match.start",
            request_id=request.request_id,
            request_tokens=request.num_tokens,
            all_token_id_count=len(request.all_token_ids),
            num_computed_tokens=num_computed_tokens,
            hash_block_size=self.hash_block_size,
            hbm_hit_block_num=wa_hbm_hit_block_num,
            hbm_computed_tokens=wa_computed_tokens,
            persist_token_threshold=self.persist_token_threshold,
            load_tokens_threshold=self.load_tokens_threshold,
        )

        if request.num_tokens <= self.persist_token_threshold:
            _fawa_trace(
                "match.decision",
                request_id=request.request_id,
                external_hit_tokens=0,
                need_load=False,
                reason="below persist_token_threshold",
            )
            return 0, False
        skip_external_load = request.num_tokens <= (
            wa_computed_tokens + self.load_tokens_threshold
        )
        if skip_external_load and wa_hbm_hit_block_num <= 0:
            _fawa_trace(
                "match.decision",
                request_id=request.request_id,
                external_hit_tokens=0,
                need_load=False,
                reason="load threshold reached with no HBM prefix",
            )
            return 0, False

        canonical_hashes = self.generate_hash(
            self.hash_block_size, request.all_token_ids, self._seed
        )
        fa_hbm_hit_keys = canonical_hashes[:wa_hbm_hit_block_num]
        _fawa_trace(
            "match.hashes",
            request_id=request.request_id,
            canonical_hash_count=len(canonical_hashes),
            canonical_hash_preview=_preview_bytes(canonical_hashes),
            fa_hbm_hit_key_count=len(fa_hbm_hit_keys),
            fa_hbm_hit_key_preview=_preview_bytes(fa_hbm_hit_keys),
            skip_external_load=skip_external_load,
        )

        # Even when no external load is worthwhile, the local-HBM prefix is a
        # cache hit and should remain hot in both FAWA backing stores.
        if skip_external_load:
            self._prefetch_hit_key_hotness(fa_hbm_hit_keys, fa_hbm_hit_keys)
            _fawa_trace(
                "match.decision",
                request_id=request.request_id,
                external_hit_tokens=0,
                need_load=False,
                reason="external load skipped by threshold",
            )
            return 0, False

        external_keys = canonical_hashes[wa_hbm_hit_block_num:]
        if not external_keys:
            self._prefetch_hit_key_hotness(fa_hbm_hit_keys, fa_hbm_hit_keys)
            _fawa_trace(
                "match.decision",
                request_id=request.request_id,
                external_hit_tokens=0,
                need_load=False,
                reason="no external keys",
            )
            return 0, False

        external_hit_blocks = 0
        if external_keys:
            try:
                external_hit_blocks = self._lookup_external_hit_blocks(external_keys)
            except Exception as e:
                logger.error(
                    f"request {request.request_id} FAWA lookup error. "
                    f"{type(e).__name__}: {e}"
                )
                self._record_counter("connector_lookup_errors_total")

        total_hit_block_num = wa_hbm_hit_block_num + external_hit_blocks
        self._prefetch_hit_key_hotness(
            fa_hbm_hit_keys,
            canonical_hashes[:total_hit_block_num],
        )
        num_total_hit_tokens = (
            external_hit_blocks * self.hash_block_size + wa_computed_tokens
        )
        external_hit_tokens = num_total_hit_tokens - num_computed_tokens

        if num_total_hit_tokens == request.num_tokens:
            external_hit_tokens -= 1

        threshold_forced_compute = False
        if external_hit_blocks * self.hash_block_size <= self.load_tokens_threshold:
            threshold_forced_compute = True
            external_hit_tokens = 0
            num_total_hit_tokens = num_computed_tokens
            # let wa_hbm_hit_block_num equal to total_hit_block_num,so no need to load external blocks
            wa_hbm_hit_block_num += external_hit_blocks

        # TODO :for HMA, vllm should offer all kv group's prefix block hits，so that more FA blocks can be reused
        self.requests_meta[request.request_id] = FAWARequestMeta(
            ucm_block_ids=canonical_hashes,
            hbm_hit_block_num=wa_hbm_hit_block_num,
            total_hit_block_num=total_hit_block_num,
            num_token_ids=request.num_tokens,
            token_processed=num_total_hit_tokens,
        )
        _fawa_trace(
            "match.result",
            request_id=request.request_id,
            hbm_hit_block_num=wa_hbm_hit_block_num,
            external_hit_blocks=external_hit_blocks,
            total_hit_block_num=total_hit_block_num,
            num_computed_tokens=num_computed_tokens,
            num_total_hit_tokens=num_total_hit_tokens,
            external_hit_tokens=external_hit_tokens,
            threshold_forced_compute=threshold_forced_compute,
            request_meta_token_processed=num_total_hit_tokens,
            request_meta_stored=True,
        )
        logger.info_once(
            f"FAWA request_id: {request.request_id}, "
            f"total tokens: {request.num_tokens}, "
            f"hit hbm tokens: {num_computed_tokens}, "
            f"hit external tokens: {external_hit_tokens}, "
            f"load blocks: {total_hit_block_num - wa_hbm_hit_block_num}, "
            f"dump blocks: {len(canonical_hashes) - total_hit_block_num}, "
        )
        return external_hit_tokens, False

    def update_state_after_alloc(
        self,
        request: "Request",
        blocks: "KVCacheBlocks",
        num_external_tokens: int,
    ) -> None:
        pass

    def _slice_group_block_ids(
        self,
        group_id: int,
        group_block_ids: list[int],
        window_boundary_token_idx: np.ndarray,
        fetch_wa_block_wise: bool,
    ) -> list[int]:
        """Select the physical group blocks needed for FA or WA store rows."""

        is_window_group = group_id in self.window_group_ids
        group_meta = self.group_metas[group_id]
        if is_window_group:
            if not group_meta.tail_tokens:
                return []
            if fetch_wa_block_wise:
                # Block-wise WA stores one tail row for each canonical boundary.
                boundary_block_indices = (
                    window_boundary_token_idx // group_meta.token_block_size
                )
                offsets = np.arange(group_meta.tail_blocks - 1, -1, -1, dtype=np.int64)
                boundary_block_indices = (
                    boundary_block_indices[:, None] - offsets[None, :]
                )
                return np.array(group_block_ids)[
                    boundary_block_indices.flatten()
                ].tolist()
            else:
                # Chunk-wise WA stores only the tail for the final boundary.
                boundary_block_idx = (
                    window_boundary_token_idx[-1] // group_meta.token_block_size
                ) + 1
                return group_block_ids[
                    boundary_block_idx - group_meta.tail_blocks : boundary_block_idx
                ]
        # FA rows map each canonical hash block to its containing group block.
        return np.array(group_block_ids)[
            window_boundary_token_idx // group_meta.token_block_size
        ].tolist()

    def _generate_dispatch_meta(
        self,
        req_meta: FAWARequestMeta,
        new_tokens: int,
        new_vllm_block_ids: tuple[list[int], ...],
        need_load: bool = True,
    ) -> FAWARequestDispatchMeta:
        """Build one request's worker-side load and dump plan.

        Canonical hash blocks are split into:

        - `[0, hbm_hit_block_num)`: already resident in local HBM.
        - `[hbm_hit_block_num, total_hit_block_num)`: external hit to load.
        - `[token_processed, token_processed + new_tokens)`: newly computed
          tokens whose complete canonical blocks should be dumped.

        `new_vllm_block_ids` is appended to the accumulated per-group block
        rows before slicing FA/WA rows for this step.
        """

        _fawa_trace(
            "dispatch.start",
            hbm_hit_block_num=req_meta.hbm_hit_block_num,
            total_hit_block_num=req_meta.total_hit_block_num,
            token_processed=req_meta.token_processed,
            request_token_count=req_meta.num_token_ids,
            new_tokens=new_tokens,
            need_load=need_load,
            new_vllm_block_counts=[len(block_ids) for block_ids in new_vllm_block_ids],
        )
        if not req_meta.vllm_block_ids:
            req_meta.vllm_block_ids = tuple([] for _ in self.group_metas)
        if len(new_vllm_block_ids) != len(req_meta.vllm_block_ids):
            raise RuntimeError(
                f"FAWA dispatch metadata expected {len(req_meta.vllm_block_ids)} "
                f"KV cache groups, got {len(new_vllm_block_ids)}."
            )
        for group_id, block_ids in enumerate(new_vllm_block_ids):
            req_meta.vllm_block_ids[group_id].extend(block_ids)

        all_group_block_ids = req_meta.vllm_block_ids
        _fawa_trace(
            "dispatch.block_state",
            accumulated_vllm_block_counts=[
                len(block_ids) for block_ids in all_group_block_ids
            ],
            accumulated_vllm_block_preview=[
                _preview_ints(block_ids) for block_ids in all_group_block_ids
            ],
        )
        load_block_keys: list[bytes] = []
        load_start, load_end = 0, 0
        load_vllm_block_ids: list[list[int]] = []
        if need_load and req_meta.total_hit_block_num > req_meta.hbm_hit_block_num:
            load_start = req_meta.hbm_hit_block_num
            load_end = req_meta.total_hit_block_num
            load_block_keys = req_meta.ucm_block_ids[load_start:load_end]
            window_boundary_token_idx = (
                np.arange(load_start + 1, load_end + 1) * self.hash_block_size - 1
            )
            for group_id, group_block_ids in enumerate(all_group_block_ids):
                load_vllm_block_ids.append(
                    self._slice_group_block_ids(
                        group_id,
                        group_block_ids,
                        window_boundary_token_idx,
                        fetch_wa_block_wise=False,  # always fetch the full WA tail on load to simplify logic
                    )
                )
            _fawa_trace(
                "dispatch.load_plan",
                hash_range=(load_start, load_end),
                key_count=len(load_block_keys),
                key_preview=_preview_bytes(load_block_keys),
                group_block_counts=[len(block_ids) for block_ids in load_vllm_block_ids],
                group_block_preview=[
                    _preview_ints(block_ids) for block_ids in load_vllm_block_ids
                ],
            )

        computed_end_token = min(
            req_meta.num_token_ids,
            req_meta.token_processed + new_tokens,
        )
        dump_start = max(
            req_meta.total_hit_block_num,
            req_meta.token_processed // self.hash_block_size,
        )
        dump_end = computed_end_token // self.hash_block_size
        dump_block_keys: list[bytes] = []
        dump_vllm_block_ids: list[list[int]] = []
        if dump_end > dump_start:
            dump_block_keys = req_meta.ucm_block_ids[dump_start:dump_end]
            window_boundary_token_idx = (
                np.arange(dump_start + 1, dump_end + 1) * self.hash_block_size - 1
            )
            for group_id, group_block_ids in enumerate(all_group_block_ids):
                dump_vllm_block_ids.append(
                    self._slice_group_block_ids(
                        group_id,
                        group_block_ids,
                        window_boundary_token_idx,
                        fetch_wa_block_wise=self.wa_dump_block_wise,
                    )
                )
        req_meta.token_processed = computed_end_token

        dispatch_meta = FAWARequestDispatchMeta(
            load_keys=load_block_keys,
            load_hash_start=load_start,
            load_hash_end=load_end,
            load_vllm_block_ids=tuple(load_vllm_block_ids),
            dump_keys=dump_block_keys,
            dump_hash_start=dump_start,
            dump_hash_end=dump_end,
            dump_vllm_block_ids=tuple(dump_vllm_block_ids),
        )
        _fawa_trace(
            "dispatch.dump_plan",
            computed_end_token=computed_end_token,
            dump_hash_range=(dump_start, dump_end),
            key_count=len(dump_block_keys),
            key_preview=_preview_bytes(dump_block_keys),
            group_block_counts=[len(block_ids) for block_ids in dump_vllm_block_ids],
            group_block_preview=[
                _preview_ints(block_ids) for block_ids in dump_vllm_block_ids
            ],
            next_token_processed=req_meta.token_processed,
        )
        return dispatch_meta

    def build_connector_meta(
        self, scheduler_output: SchedulerOutput
    ) -> UCMFAWAConnectorMetadata:
        requests_dispatch_meta: dict[str, FAWARequestDispatchMeta] = {}
        scheduled_cached_reqs = scheduler_output.scheduled_cached_reqs
        _fawa_trace(
            "connector_meta.start",
            new_request_ids=[
                request.req_id for request in scheduler_output.scheduled_new_reqs
            ],
            cached_request_ids=list(scheduled_cached_reqs.req_ids),
            finished_request_ids=list(scheduler_output.finished_req_ids),
            preempted_request_ids=list(scheduler_output.preempted_req_ids or ()),
            scheduled_token_counts=dict(scheduler_output.num_scheduled_tokens),
        )
        # New requests may need both external-prefix load and new-block dump.
        for request in scheduler_output.scheduled_new_reqs:
            request_id, vllm_block_ids = request.req_id, request.block_ids
            req_meta = self.requests_meta.get(request_id)
            if req_meta:
                requests_dispatch_meta[request_id] = self._generate_dispatch_meta(
                    req_meta,
                    scheduler_output.num_scheduled_tokens[request_id],
                    tuple(vllm_block_ids),
                )

        for i, request_id in enumerate(scheduled_cached_reqs.req_ids):
            req_meta = self.requests_meta.get(request_id)
            if req_meta:
                new_block_ids = scheduled_cached_reqs.new_block_ids[i]
                if new_block_ids is None:
                    new_block_ids = tuple([] for _ in self.group_metas)
                else:
                    new_block_ids = tuple(new_block_ids)
                if hasattr(scheduled_cached_reqs, "resumed_from_preemption"):
                    resumed_from_preemption = (
                        scheduled_cached_reqs.resumed_from_preemption[i]
                    )
                else:
                    resumed_from_preemption = (
                        request_id in scheduled_cached_reqs.resumed_req_ids
                    )
                if resumed_from_preemption:
                    req_meta.vllm_block_ids = tuple([] for _ in self.group_metas)
                requests_dispatch_meta[request_id] = self._generate_dispatch_meta(
                    req_meta,
                    scheduler_output.num_scheduled_tokens[request_id],
                    new_block_ids,
                    need_load=resumed_from_preemption,
                )

        for request_id in scheduler_output.finished_req_ids:
            self.requests_meta.pop(request_id, None)

        preempted_req_ids = set(scheduler_output.preempted_req_ids or ())
        metadata = UCMFAWAConnectorMetadata(requests_dispatch_meta, preempted_req_ids)
        _fawa_trace(
            "connector_meta.done",
            request_count=len(metadata.request_meta),
            request_ids=list(metadata.request_meta),
            preempted_request_ids=sorted(metadata.preempted_req_ids),
            plans={
                request_id: {
                    "load_keys": len(request.load_keys),
                    "load_hash_range": (request.load_hash_start, request.load_hash_end),
                    "dump_keys": len(request.dump_keys),
                    "dump_hash_range": (request.dump_hash_start, request.dump_hash_end),
                }
                for request_id, request in metadata.request_meta.items()
            },
        )
        return metadata

    def _submit_load_task(
        self,
        request_id: str,
        label: str,
        store: UcmKVStoreBaseV1,
        keys: list[bytes],
        ptrs: np.ndarray,
    ) -> FAWALoadTask:
        """Submit one store load and retain block ids for failure reporting."""

        _fawa_trace(
            "load.submit.start",
            request_id=request_id,
            label=label,
            key_count=len(keys),
            key_preview=_preview_bytes(keys),
            ptr_shape=ptrs.shape,
            ptr_dtype=str(ptrs.dtype),
        )
        shard_indices = [0] * len(keys)
        task = self._rank_consistency.submit_load(
            store,
            {request_id: keys},
            keys,
            shard_indices,
            ptrs,
        )
        load_task = FAWALoadTask(
            request_id=request_id,
            label=label,
            store=store,
            task=task,
            key_count=len(keys),
        )
        _fawa_trace(
            "load.submit.done",
            request_id=request_id,
            label=label,
            key_count=load_task.key_count,
            task_type=type(task).__name__,
        )
        return load_task

    def _handle_load_err(self, request_id: str):
        affected_block_ids = self._get_request_all_block_ids(request_id)
        self._record_load_error(
            "connector_load_wait_errors_total",
            affected_block_ids,
        )
        self._connector_worker_meta.mark_failed(request_id)

    def _wait_load_task(
        self,
        load_task: FAWALoadTask,
    ) -> bool:
        """Wait a load task and mark its anchor blocks invalid on failure."""

        _fawa_trace(
            "load.wait.start",
            request_id=load_task.request_id,
            label=load_task.label,
            key_count=load_task.key_count,
        )
        try:
            self._rank_consistency.wait_load(load_task.task)
        except Exception as e:
            logger.error(
                f"request {load_task.request_id} wait FAWA load "
                f"task label={load_task.label} error. {type(e).__name__}: {e}"
            )
            self._handle_load_err(load_task.request_id)
            return False
        _fawa_trace(
            "load.wait.done",
            request_id=load_task.request_id,
            label=load_task.label,
            key_count=load_task.key_count,
            success=True,
        )
        return True

    def get_block_ids_with_load_errors(self) -> set[int]:
        res = self._invalid_block_ids
        self._invalid_block_ids = set()
        return res

    def _submit_dump_task(
        self,
        label: str,
        store: UcmKVStoreBaseV1,
        keys: list[bytes],
        ptrs: np.ndarray,
        event_handle: int,
        block_ids_by_request: dict[str, set[bytes]],
    ) -> FAWADumpTask:
        """Submit one store dump for FA or WA rows."""

        _fawa_trace(
            "dump.submit.start",
            label=label,
            key_count=len(keys),
            key_preview=_preview_bytes(keys),
            ptr_shape=ptrs.shape,
            ptr_dtype=str(ptrs.dtype),
            event_handle=event_handle,
            request_ids=sorted(block_ids_by_request),
        )
        shard_indices = [0] * len(keys)
        task = self._rank_consistency.submit_dump(
            store,
            block_ids_by_request,
            keys,
            shard_indices,
            ptrs,
            event_handle,
        )
        dump_task = FAWADumpTask(
            label=label,
            store=store,
            task=task,
            key_count=len(keys),
            event_handle=event_handle,
        )
        _fawa_trace(
            "dump.submit.done",
            label=label,
            key_count=dump_task.key_count,
            task_type=type(task).__name__,
            event_handle=event_handle,
        )
        return dump_task

    def _extract_fa_ptr(self, store_keys, hash_start, hash_end, candidate_vllm_ids):
        """Build store pointer rows for full-attention cache segments."""

        _fawa_trace(
            "ptr.fa.start",
            key_count=len(store_keys),
            hash_range=(hash_start, hash_end),
            key_preview=_preview_bytes(store_keys),
            group_ids=self.fa_group_ids,
        )
        all_ptrs = []
        for group_id in self.fa_group_ids:
            layout = self.group_layouts.get(group_id)
            if layout is None:
                continue
            meta = self.group_metas[group_id]
            block_ids = np.asarray(candidate_vllm_ids[group_id], dtype=np.uint64)
            # GPU layouts usually use one tensor block per hash block. Ascend
            # layouts may pack several canonical hash blocks in one tensor
            # block, so the row starts at a token offset inside the block.
            if self.hash_block_size == meta.token_block_size:
                group_ptrs = layout.extract_addrs(block_ids)
                address_mode = "whole_group_block"
            else:
                token_start = np.arange(hash_start, hash_end) * self.hash_block_size
                token_offsets = token_start % meta.token_block_size
                group_ptrs = layout.extract_addrs_with_offsets(
                    block_ids, meta.token_block_size, token_offsets
                )
                address_mode = "token_offset_inside_group_block"
            _fawa_trace(
                "ptr.fa.group",
                group_id=group_id,
                block_count=len(block_ids),
                block_preview=_preview_ints(block_ids),
                group_token_block_size=meta.token_block_size,
                hash_block_size=self.hash_block_size,
                address_mode=address_mode,
                ptr_shape=group_ptrs.shape,
            )
            all_ptrs.append(group_ptrs)

        ptrs = np.concatenate(all_ptrs, axis=1)
        _fawa_trace(
            "ptr.fa.done",
            key_count=len(store_keys),
            ptr_shape=ptrs.shape,
            ptr_dtype=str(ptrs.dtype),
        )
        return ptrs

    def _extract_wa_ptr(self, store_keys, vllm_ids):
        """Build store pointer rows for window-attention tail segments."""

        _fawa_trace(
            "ptr.wa.start",
            key_count=len(store_keys),
            key_preview=_preview_bytes(store_keys),
            group_ids=self.window_group_ids,
        )
        all_ptrs = []
        for group_id in self.window_group_ids:
            layout = self.group_layouts.get(group_id)
            if layout is None:
                continue
            meta = self.group_metas[group_id]
            if not meta.tail_tokens:
                continue

            block_ids = np.asarray(vllm_ids[group_id], dtype=np.uint64)
            if meta.tail_blocks == 1 and meta.token_block_size > meta.tail_tokens:
                # A short tail stored inside a larger group block starts near
                # the end of the physical tensor block.
                token_offsets = np.ones_like(block_ids) * (
                    meta.token_block_size - meta.tail_tokens
                )
                group_ptrs = layout.extract_addrs_with_offsets(
                    block_ids, meta.token_block_size, token_offsets
                )
                address_mode = "short_tail_offset_inside_group_block"
            else:
                token_offsets = np.zeros_like(block_ids)
                group_ptrs = layout.extract_addrs(block_ids)
                # Multi-block WA tails are flattened into one store row per
                # canonical boundary key.
                group_ptrs = group_ptrs.reshape(len(store_keys), -1)
                address_mode = "tail_group_blocks"

            _fawa_trace(
                "ptr.wa.group",
                group_id=group_id,
                block_count=len(block_ids),
                block_preview=_preview_ints(block_ids),
                group_token_block_size=meta.token_block_size,
                tail_tokens=meta.tail_tokens,
                tail_blocks=meta.tail_blocks,
                address_mode=address_mode,
                ptr_shape=group_ptrs.shape,
            )
            all_ptrs.append(group_ptrs)

        ptrs = np.concatenate(all_ptrs, axis=1)
        _fawa_trace(
            "ptr.wa.done",
            key_count=len(store_keys),
            ptr_shape=ptrs.shape,
            ptr_dtype=str(ptrs.dtype),
        )
        return ptrs

    def _get_request_all_block_ids(self, request_id: str) -> set[int]:
        """Get all VLLM block ids referenced by one request's load and dump plan."""
        metadata = self._get_connector_metadata()
        request = metadata.request_meta.get(request_id, None)
        if request is None:
            return set()
        all_group_vllm_block_ids = {
            block_id
            for block_ids in request.load_vllm_block_ids
            for block_id in block_ids
        }
        all_group_vllm_block_ids |= {
            block_id
            for block_ids in request.dump_vllm_block_ids
            for block_id in block_ids
        }
        return all_group_vllm_block_ids

    def start_load_kv(self, forward_context: "ForwardContext", **kwargs) -> None:
        metadata = self._get_connector_metadata()
        if not isinstance(metadata, UCMFAWAConnectorMetadata):
            raise RuntimeError(f"Unexpected FAWA metadata type: {type(metadata)}")

        _fawa_trace(
            "load.phase.start",
            role=str(self._role),
            request_count=len(metadata.request_meta),
            request_ids=list(metadata.request_meta),
            forward_context_type=type(forward_context).__name__,
        )
        tasks: list[FAWALoadTask] = []
        for request_id, request in metadata.request_meta.items():
            if not request.load_keys:
                _fawa_trace(
                    "load.request.skip",
                    request_id=request_id,
                    reason="no external-hit keys",
                )
                continue

            try:
                if self.fa_store is None:
                    raise RuntimeError("FA store is not initialized.")
                if self.wa_store is None:
                    raise RuntimeError("WA store is not initialized.")

                # FA groups are loaded for every external-hit canonical block.
                fa_ptrs = self._extract_fa_ptr(
                    request.load_keys,
                    request.load_hash_start,
                    request.load_hash_end,
                    request.load_vllm_block_ids,
                )
                fa_task = self._submit_load_task(
                    request_id,
                    "FA",
                    self.fa_store,
                    request.load_keys,
                    fa_ptrs,
                )
                tasks.append(fa_task)

                # WA groups only need the final matched boundary.
                window_keys = request.load_keys[-1:]
                window_ptrs = self._extract_wa_ptr(
                    window_keys,
                    request.load_vllm_block_ids,
                )
                wa_task = self._submit_load_task(
                    request_id,
                    "WA",
                    self.wa_store,
                    window_keys,
                    window_ptrs,
                )
                tasks.append(wa_task)
                _fawa_trace(
                    "load.request.plan",
                    request_id=request_id,
                    load_key_count=len(request.load_keys),
                    load_hash_range=(request.load_hash_start, request.load_hash_end),
                    fa_ptr_shape=fa_ptrs.shape,
                    wa_key_count=len(window_keys),
                    wa_ptr_shape=window_ptrs.shape,
                    note="FA loads every external block; WA loads final matched boundary",
                )
            except Exception as e:
                logger.error(
                    f"request {request_id} submit FAWA load task "
                    f"error. {type(e).__name__}: {e}"
                )
                self._handle_load_err(request_id)

        self._wait_all_load_task(tasks)
        _fawa_trace(
            "load.phase.done",
            task_count=len(tasks),
            task_labels=[task.label for task in tasks],
            task_key_counts=[task.key_count for task in tasks],
        )

    def _wait_all_load_task(self, tasks: list[FAWALoadTask]):
        load_bytes = 0
        for load_task in tasks:
            if self._wait_load_task(load_task):
                load_bytes += load_task.key_count * self.file_size[load_task.label]
        if tasks:
            ucmmetrics.update_stats({"load_bytes_total": load_bytes})
        _fawa_trace(
            "load.wait_all.done",
            task_count=len(tasks),
            load_bytes=load_bytes,
            file_size=self.file_size,
        )

    def wait_for_save(self) -> None:
        metadata = self._get_connector_metadata()
        if not isinstance(metadata, UCMFAWAConnectorMetadata):
            raise RuntimeError(f"Unexpected FAWA metadata type: {type(metadata)}")

        if self.fa_store is None:
            raise RuntimeError("FA store is not initialized.")
        if self.wa_store is None:
            raise RuntimeError("WA store is not initialized.")

        _fawa_trace(
            "save.phase.start",
            role=str(self._role),
            tp_rank=self.tp_rank,
            tp_size=self.tp_size,
            wa_dump_block_wise=self.wa_dump_block_wise,
            request_count=len(metadata.request_meta),
            request_ids=list(metadata.request_meta),
            pending_dump_group_count=len(self.tp_dump_tasks),
        )
        self._poll_completed_dump_tasks()

        fa_dump_keys: list[bytes] = []
        wa_dump_keys: list[bytes] = []
        fa_ptr_rows: list[np.ndarray] = []
        wa_ptr_rows: list[np.ndarray] = []
        dump_request_ids: tuple[str] = ()
        fa_dump_blocks_by_request: dict[str, set[bytes]] = {}
        wa_dump_blocks_by_request: dict[str, set[bytes]] = {}
        save_bytes = 0
        if self.tp_size > 1:
            # Split FA rows by canonical block index. Block-wise WA follows the same
            # TP key slice; chunk-wise WA assigns one final boundary per request.
            wa_dump_ring_idx = 0
            for request_id, request in metadata.request_meta.items():
                if not request.dump_keys:
                    continue
                dump_request_ids += (request_id,)
                num_keys = len(request.dump_keys)
                tp_block_start = num_keys * self.tp_rank // self.tp_size
                tp_block_end = num_keys * (self.tp_rank + 1) // self.tp_size
                tp_dump_keys = request.dump_keys[tp_block_start:tp_block_end]
                _fawa_trace(
                    "save.request.tp_split",
                    request_id=request_id,
                    total_dump_key_count=num_keys,
                    tp_block_range=(tp_block_start, tp_block_end),
                    tp_dump_key_count=len(tp_dump_keys),
                    tp_dump_key_preview=_preview_bytes(tp_dump_keys),
                    wa_dump_block_wise=self.wa_dump_block_wise,
                )
                if tp_dump_keys:
                    fa_dump_blocks_by_request[request_id] = set(tp_dump_keys)
                    fa_dump_vllm_block_ids = tuple(
                        (
                            group_block_ids[tp_block_start:tp_block_end]
                            if group_id in self.fa_group_ids
                            else group_block_ids
                        )
                        for group_id, group_block_ids in enumerate(
                            request.dump_vllm_block_ids
                        )
                    )

                    fa_dump_keys.extend(tp_dump_keys)
                    fa_ptr_rows.append(
                        self._extract_fa_ptr(
                            tp_dump_keys,
                            request.dump_hash_start + tp_block_start,
                            request.dump_hash_start + tp_block_end,
                            fa_dump_vllm_block_ids,
                        )
                    )
                if self.wa_dump_block_wise:
                    if tp_dump_keys:
                        wa_dump_blocks_by_request[request_id] = set(tp_dump_keys)
                        wa_dump_vllm_block_ids = tuple(
                            (
                                group_block_ids[
                                    tp_block_start
                                    * self.group_metas[
                                        group_id
                                    ].tail_blocks : tp_block_end
                                    * self.group_metas[group_id].tail_blocks
                                ]
                                if group_id in self.window_group_ids
                                else group_block_ids
                            )
                            for group_id, group_block_ids in enumerate(
                                request.dump_vllm_block_ids
                            )
                        )
                        wa_dump_keys.extend(tp_dump_keys)
                        wa_ptr_rows.append(
                            self._extract_wa_ptr(
                                tp_dump_keys,
                                wa_dump_vllm_block_ids,
                            )
                        )
                elif wa_dump_ring_idx % self.tp_size == self.tp_rank:
                    request_wa_dump_keys = request.dump_keys[-1:]
                    wa_dump_blocks_by_request[request_id] = set(request_wa_dump_keys)
                    wa_dump_keys.extend(request_wa_dump_keys)
                    wa_ptr_rows.append(
                        self._extract_wa_ptr(
                            request_wa_dump_keys,
                            request.dump_vllm_block_ids,
                        )
                    )
                wa_dump_ring_idx += 1
        else:
            for request_id, request in metadata.request_meta.items():
                if not request.dump_keys:
                    continue
                dump_request_ids += (request_id,)
                _fawa_trace(
                    "save.request.single_rank",
                    request_id=request_id,
                    dump_key_count=len(request.dump_keys),
                    dump_key_preview=_preview_bytes(request.dump_keys),
                    hash_range=(request.dump_hash_start, request.dump_hash_end),
                    wa_dump_block_wise=self.wa_dump_block_wise,
                )
                fa_dump_blocks_by_request[request_id] = set(request.dump_keys)
                fa_dump_keys.extend(request.dump_keys)
                fa_ptr_rows.append(
                    self._extract_fa_ptr(
                        request.dump_keys,
                        request.dump_hash_start,
                        request.dump_hash_end,
                        request.dump_vllm_block_ids,
                    )
                )
                if self.wa_dump_block_wise:
                    wa_dump_blocks_by_request[request_id] = set(request.dump_keys)
                    wa_dump_keys.extend(request.dump_keys)
                    wa_ptr_rows.append(
                        self._extract_wa_ptr(
                            request.dump_keys,
                            request.dump_vllm_block_ids,
                        )
                    )
                else:
                    request_wa_dump_keys = request.dump_keys[-1:]
                    wa_dump_blocks_by_request[request_id] = set(request_wa_dump_keys)
                    wa_dump_keys.extend(request_wa_dump_keys)
                    wa_ptr_rows.append(
                        self._extract_wa_ptr(
                            request_wa_dump_keys,
                            request.dump_vllm_block_ids,
                        )
                    )

        _fawa_trace(
            "save.plan",
            dump_request_ids=dump_request_ids,
            fa_key_count=len(fa_dump_keys),
            wa_key_count=len(wa_dump_keys),
            fa_key_preview=_preview_bytes(fa_dump_keys),
            wa_key_preview=_preview_bytes(wa_dump_keys),
            fa_ptr_row_count=len(fa_ptr_rows),
            wa_ptr_row_count=len(wa_ptr_rows),
            fa_ptr_shapes=[ptrs.shape for ptrs in fa_ptr_rows],
            wa_ptr_shapes=[ptrs.shape for ptrs in wa_ptr_rows],
        )
        if fa_dump_keys:
            event_handle = self._get_dump_event_handle()
            fa_ptrs = np.vstack(fa_ptr_rows)
            if dump_request_ids not in self.tp_dump_tasks:
                self.tp_dump_tasks[dump_request_ids] = []
            try:
                fa_dump_task = self._submit_dump_task(
                    "FA",
                    self.fa_store,
                    fa_dump_keys,
                    fa_ptrs,
                    event_handle,
                    fa_dump_blocks_by_request,
                )
                self.tp_dump_tasks[dump_request_ids].append(fa_dump_task)
                save_bytes += fa_dump_task.key_count * self.file_size["FA"]
                _fawa_trace(
                    "save.fa.submitted",
                    dump_request_ids=dump_request_ids,
                    key_count=fa_dump_task.key_count,
                    ptr_shape=fa_ptrs.shape,
                    event_handle=event_handle,
                    pending_task_count=len(self.tp_dump_tasks[dump_request_ids]),
                )
            except Exception as e:
                self.device.destroy_event_handle(event_handle)
                logger.error(f"dump FAWA kv cache failed. {type(e).__name__}: {e}")
                self._record_counter("connector_dump_submit_errors_total")
        if wa_dump_keys:
            event_handle = self._get_dump_event_handle()
            window_ptrs = np.vstack(wa_ptr_rows)
            try:
                # Sliding-window blocks can be released and reused by the next
                # allocate_slots() call while the request is still running.
                # Wait for the WA dump here so the store no longer references
                # those source blocks when wait_for_save() returns.
                wa_dump_task = self._submit_dump_task(
                    "WA",
                    self.wa_store,
                    wa_dump_keys,
                    window_ptrs,
                    event_handle,
                    wa_dump_blocks_by_request,
                )
                save_bytes += wa_dump_task.key_count * self.file_size["WA"]
                try:
                    self._rank_consistency.wait_dump(wa_dump_task.task)
                except Exception as e:
                    logger.error(
                        "Synchronous FAWA WA dump task failed; external cache "
                        f"may miss. keys={wa_dump_task.key_count}, "
                        f"{type(e).__name__}: {e}"
                    )
                    self._record_counter("connector_dump_wait_errors_total")
                finally:
                    self.device.destroy_event_handle(wa_dump_task.event_handle)
                _fawa_trace(
                    "save.wa.submitted_and_waited",
                    key_count=wa_dump_task.key_count,
                    ptr_shape=window_ptrs.shape,
                    event_handle=event_handle,
                )
            except Exception as e:
                self.device.destroy_event_handle(event_handle)
                logger.error(f"dump FAWA WA kv cache failed. {type(e).__name__}: {e}")
                self._record_counter("connector_dump_submit_errors_total")
        if fa_dump_keys or wa_dump_keys:
            ucmmetrics.update_stats({"save_bytes_total": save_bytes})
        _fawa_trace(
            "save.phase.done",
            fa_key_count=len(fa_dump_keys),
            wa_key_count=len(wa_dump_keys),
            save_bytes=save_bytes,
            pending_dump_group_count=len(self.tp_dump_tasks),
        )

    def _poll_completed_dump_tasks(self) -> None:
        """Reap completed FAWA dump tasks without waiting for in-flight tasks."""

        _fawa_trace(
            "dump.poll.start",
            pending_group_count=len(self.tp_dump_tasks),
            pending_task_count=sum(
                len(dump_tasks) for dump_tasks in self.tp_dump_tasks.values()
            ),
        )
        completed_task_count = 0
        still_in_flight_count = 0
        for request_ids, dump_tasks in list(self.tp_dump_tasks.items()):
            in_flight_tasks = []
            for dump_task in dump_tasks:
                task_finished = False

                try:
                    task_finished = dump_task.store.check(dump_task.task)
                except Exception as e:
                    logger.error(
                        "Check FAWA dump task failed; external cache may miss. "
                        f"label={dump_task.label}, keys={dump_task.key_count}, "
                        f"{type(e).__name__}: {e}"
                    )
                    in_flight_tasks.append(dump_task)
                    continue

                if task_finished:
                    completed_task_count += 1
                    try:
                        self._rank_consistency.wait_dump(dump_task.task)
                    except Exception as e:
                        logger.error(
                            "Best-effort FAWA dump task failed; external cache may miss. "
                            f"label={dump_task.label}, keys={dump_task.key_count}, "
                            f"{type(e).__name__}: {e}"
                        )
                    finally:
                        self.device.destroy_event_handle(dump_task.event_handle)
                else:
                    still_in_flight_count += 1
                    in_flight_tasks.append(dump_task)

            if in_flight_tasks:
                self.tp_dump_tasks[request_ids] = in_flight_tasks
            else:
                self.tp_dump_tasks.pop(request_ids, None)
        _fawa_trace(
            "dump.poll.done",
            completed_task_count=completed_task_count,
            still_in_flight_count=still_in_flight_count,
            pending_group_count=len(self.tp_dump_tasks),
        )

    def _drain_best_effort_dump_tasks(self, finished_req_ids: set[str]) -> None:
        """Best-effort wait for FAWA dump tasks.

        Dump failures only mean the external cache may miss later. They must not
        block vLLM from releasing HBM blocks, so failed tasks are logged and then
        removed from tracking.
        """
        if not finished_req_ids:
            return

        _fawa_trace(
            "dump.drain.start",
            finished_request_ids=sorted(finished_req_ids),
            pending_group_count=len(self.tp_dump_tasks),
        )
        finished_chunk_req_ids = []
        for request_ids, dump_tasks in self.tp_dump_tasks.items():
            if finished_req_ids.intersection(request_ids):
                finished_chunk_req_ids.append(request_ids)
                for dump_task in dump_tasks:
                    try:
                        self._rank_consistency.wait_dump(dump_task.task)
                    except Exception as e:
                        logger.error(
                            "Best-effort FAWA dump task failed; external cache may miss. "
                            f"label={dump_task.label}, keys={dump_task.key_count}, "
                            f"{type(e).__name__}: {e}"
                        )
                        self._record_counter("connector_dump_wait_errors_total")
                    finally:
                        self.device.destroy_event_handle(dump_task.event_handle)

        for request_ids in finished_chunk_req_ids:
            self.tp_dump_tasks.pop(request_ids, None)
        _fawa_trace(
            "dump.drain.done",
            drained_group_count=len(finished_chunk_req_ids),
            pending_group_count=len(self.tp_dump_tasks),
        )

    def handle_preemptions(self, kv_connector_metadata: UCMFAWAConnectorMetadata):
        # Worker side method
        _fawa_trace(
            "request.preemptions",
            preempted_request_ids=sorted(kv_connector_metadata.preempted_req_ids),
        )
        self._drain_best_effort_dump_tasks(kv_connector_metadata.preempted_req_ids)

    def get_finished(
        self,
        finished_req_ids: set[str],
    ) -> tuple[set[str] | None, set[str] | None]:
        # Worker side method
        _fawa_trace(
            "request.finished",
            finished_request_ids=sorted(finished_req_ids),
        )
        self._drain_best_effort_dump_tasks(finished_req_ids)
        self._rank_consistency.finish_dump(finished_req_ids)
        return finished_req_ids, None

    def request_finished_all_groups(
        self,
        request: "Request",
        block_ids: tuple[list[int], ...],
    ) -> tuple[bool, dict[str, object] | None]:
        # Scheduler side method
        return True, None
