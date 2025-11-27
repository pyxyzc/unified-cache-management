import hashlib
import pickle
from typing import TYPE_CHECKING, Any, List


from sglang.srt.managers.schedule_batch import Req

def md5(input) -> int:
    input_bytes = pickle.dumps(input, protocol=pickle.HIGHEST_PROTOCOL)
    md5_bytes = hashlib.md5(input_bytes).digest()
    return int.from_bytes(md5_bytes, byteorder="big")

def hash_request_tokens(
    hash_function: Any, block_size: int, token_ids: List[int]
) -> list[str]:
    ret = []
    parent_block_hash_value = None
    for start in range(0, len(token_ids), block_size):
        end = start + block_size
        block_token_ids = token_ids[start:end]
        # Do not hash the block if it is not full.
        if len(block_token_ids) < block_size:
            break

        if not parent_block_hash_value:
            parent_block_hash_value = md5("UCMHASHSEED")

        block_token_ids_tuple = tuple(block_token_ids)
        hash_value = hash_function(
            (parent_block_hash_value, block_token_ids_tuple)
        )
        parent_block_hash_value = hash_value
        ret.append(str(hash_value))

    return ret