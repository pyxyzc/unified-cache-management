import torch


class CUDABlockPool:
    """
    A simple CUDA block pool:
    - All blocks have the same shape along time dimension: [block_size, ...]
    - The pool contains `num_blocks` blocks
    - Blocks are accessed by block_id

    For non-MLA:
        pool shape: [num_blocks, block_size, head_num, head_dim]
        tensor shape when alloc:
            [block_size, head_num, head_dim]

    For MLA (is_mla=True):
        pool shape: [num_blocks, block_size, 1, kv_cache_dim]
        tensor shape when alloc:
            [block_size, kv_cache_dim]  or
            [block_size, 1, kv_cache_dim]
    """

    def __init__(self,
                 is_mla: bool = False,
                 block_size: int = 128,
                 kv_cache_dim: int | None = None,
                 head_num: int | None = None,
                 head_dim: int | None = None,
                 num_blocks: int = 100000,
                 dtype: torch.dtype = torch.float32,
                 device: str = "cuda"):
        if not torch.cuda.is_available():
            raise RuntimeError("CUDA is not available.")

        self.block_size = int(block_size)
        self.num_blocks = int(num_blocks)
        self.device = torch.device(device)
        self.dtype = dtype
        self.is_mla = is_mla
        self.kv_cache_dim = kv_cache_dim
        self.head_num = head_num
        self.head_dim = head_dim

        # Basic argument checks
        if self.is_mla:
            if self.kv_cache_dim is None:
                raise ValueError("kv_cache_dim must be provided when is_mla=True.")
        else:
            if self.head_num is None or self.head_dim is None:
                raise ValueError("head_num and head_dim must be provided when is_mla=False.")

        # The actual memory pool: each row is one block
        if self.is_mla:
            # [num_blocks, block_size, 1, kv_cache_dim]
            self._pool = torch.empty(
                (self.num_blocks, self.block_size, 1, self.kv_cache_dim),
                dtype=self.dtype,
                device=self.device,
            )
        else:
            # [num_blocks, block_size, head_num, head_dim]
            self._pool = torch.empty(
                (self.num_blocks, self.block_size, self.head_num, self.head_dim),
                dtype=self.dtype,
                device=self.device,
            )

        # Free block list & in-use block set
        self._free_list = list(range(self.num_blocks))  # stack structure
        self._in_use = set()

    # ---------------------------------------------------------------------- #
    # Block operations
    # ---------------------------------------------------------------------- #
    def alloc(self, tensor: torch.Tensor, copy: bool = True) -> int:
        """
        Allocate a block:
        - Input: a tensor (CPU or CUDA)
          * MLA (is_mla=True):
                shape [block_size, kv_cache_dim] or [block_size, 1, kv_cache_dim]
          * non-MLA:
                shape [block_size, head_num, head_dim]
        - Action: copy the tensor data into a free block (entire block)
        - Return: block_id

        Raises:
            ValueError  if sequence length L != block_size, or shape mismatch
            RuntimeError if no free blocks available
        """
        if not self._free_list:
            raise RuntimeError("No free blocks available.")
        
        # Get a free block
        block_id = self._free_list.pop()
        self._in_use.add(block_id)
        block_view = self._pool[block_id]  # [block_size, ...]


        if copy and tensor is not None:
            # Move tensor to CUDA device and correct dtype
            t = tensor.to(self.device, dtype=self.dtype)

            # Normalize / check shape
            if self.is_mla:
                # Expect [block_size, kv_cache_dim] or [block_size, 1, kv_cache_dim]
                if t.ndim == 3:
                    if t.size(0) != self.block_size:
                        raise ValueError(
                            f"Expected first dim (L) == block_size={self.block_size}, "
                            f"got L={t.size(0)}"
                        )
                    if t.size(1) != 1 or t.size(2) != self.kv_cache_dim:
                        raise ValueError(
                            f"Expected shape [block_size, 1, {self.kv_cache_dim}], "
                            f"got {tuple(t.shape)}"
                        )
                else:
                    raise ValueError(
                        f"Expected tensor dim 2 or 3 for MLA, got dim={t.ndim}"
                    )
            else:
                # non-MLA: expect [block_size, head_num, head_dim]
                if t.ndim != 3:
                    raise ValueError(
                        f"Expected tensor dim=3 [block_size, head_num, head_dim], "
                        f"got dim={t.ndim}"
                    )
                if t.size(0) != self.block_size:
                    raise ValueError(
                        f"Expected first dim (L) == block_size={self.block_size}, "
                        f"got L={t.size(0)}"
                    )
                if t.size(1) != self.head_num or t.size(2) != self.head_dim:
                    raise ValueError(
                        f"Expected shape [block_size, {self.head_num}, {self.head_dim}], "
                        f"got {tuple(t.shape)}"
                    )

            # Directly cover the entire block
            block_view.copy_(t)
        else:
            # copy=False: Nothing is done, and the data is then filled from the CPU/disk by load
            pass

        return block_id
    def free(self, block_id: int) -> None:
        """
        Free a block:
        - Zero out the block content
        - Put the block_id back to the free list
        """
        block_id = int(block_id)
        if block_id < 0 or block_id >= self.num_blocks:
            raise ValueError(f"Invalid block_id: {block_id}")

        if block_id not in self._in_use:
            raise ValueError(f"block_id {block_id} is not in use.")

        # Clear block content
        self._pool[block_id].zero_()

        # Return block to free list
        self._in_use.remove(block_id)
        self._free_list.append(block_id)

    def get_block(self, block_id: int, length: int | None = None) -> torch.Tensor:
        """
        Retrieve a block by block_id.
        - If length is None, return the full block (shape = [block_size, ...])
        - Otherwise, return the first `length` tokens along time dimension.

        Note:
            Returned tensor is a view of the pool.
        """
        block_id = int(block_id)
        if block_id not in self._in_use:
            raise ValueError(f"block_id {block_id} is not in use.")

        if length is None:
            return self._pool[block_id]

        if length < 0 or length > self.block_size:
            raise ValueError(
                f"length={length} exceeds block_size={self.block_size}"
            )

        return self._pool[block_id][:length]

    # ---------------------------------------------------------------------- #
    # Stats
    # ---------------------------------------------------------------------- #
    @property
    def num_free(self) -> int:
        """Return number of free blocks."""
        return len(self._free_list)

    @property
    def num_used(self) -> int:
        """Return number of blocks currently in use."""
        return len(self._in_use)