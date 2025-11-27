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


class UnifiedCacheConnector():
    def __init__(self, uc_connector_name: str, unifiedCacheConfig: UnifiedCacheConfig, environmentConfig: EnvironmentConfig):
        self.environmentConfig = environmentConfig
        self.tp_rank = unifiedCacheConfig.device
        unifiedCacheConfig_dict = asdict(unifiedCacheConfig)
        self.connector = UcmConnectorFactory.create_connector(uc_connector_name, unifiedCacheConfig_dict)


    def start_load_kv(self):
        pass


    def wait_for_layer_load(self):
        pass


    def save_kv_layer(self):
        pass


    def wait_for_save(self):
        pass

        
