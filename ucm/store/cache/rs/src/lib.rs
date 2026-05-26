use std::ffi::c_char;
use std::slice;
use std::sync::atomic::{AtomicUsize, Ordering};

mod trans_buffer;

const STATUS_OK: i32 = 0;
const STATUS_ERROR: i32 = 1;
const STATUS_INVALID_PARAM: i32 = 2;

const MESSAGE_CAPACITY: usize = 256;

static NEXT_TASK_ID: AtomicUsize = AtomicUsize::new(1);

#[repr(C)]
pub struct CacheStoreFfiStatus {
    code: i32,
    message: [c_char; MESSAGE_CAPACITY],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CacheStoreBlockId {
    bytes: [u8; 16],
}

impl CacheStoreFfiStatus {
    fn ok(&mut self) {
        self.code = STATUS_OK;
        self.message[0] = 0;
    }

    fn error(&mut self, code: i32, message: &str) {
        self.code = code;
        write_message(&mut self.message, message);
    }
}

#[repr(C)]
pub struct CacheStoreConfigView {
    store_backend_present: bool,
    unique_id_ptr: *const u8,
    unique_id_len: usize,
    device_id: i32,
    tensor_sizes_ptr: *const usize,
    tensor_sizes_len: usize,
    shard_size: usize,
    block_size: usize,
    cpu_affinity_cores_ptr: *const isize,
    cpu_affinity_cores_len: usize,
    buffer_capacity: usize,
    load_exclusive_buffer_number: usize,
    waiting_queue_depth: usize,
    running_queue_depth: usize,
    stream_number: usize,
    cpu_set_size: usize,
}

pub struct CacheStoreCore {
    trans_enabled: bool,
}

fn write_message(dst: &mut [c_char; MESSAGE_CAPACITY], message: &str) {
    dst.fill(0);
    let bytes = message.as_bytes();
    let len = bytes.len().min(MESSAGE_CAPACITY - 1);
    for (slot, byte) in dst.iter_mut().take(len).zip(bytes.iter().copied()) {
        *slot = byte as c_char;
    }
}

fn tensor_sizes(config: &CacheStoreConfigView) -> Result<&[usize], &'static str> {
    if config.tensor_sizes_len == 0 {
        return Ok(&[]);
    }
    if config.tensor_sizes_ptr.is_null() {
        return Err("invalid tensor sizes");
    }
    Ok(unsafe { slice::from_raw_parts(config.tensor_sizes_ptr, config.tensor_sizes_len) })
}

fn cpu_affinity_cores(config: &CacheStoreConfigView) -> Result<&[isize], &'static str> {
    if config.cpu_affinity_cores_len == 0 {
        return Ok(&[]);
    }
    if config.cpu_affinity_cores_ptr.is_null() {
        return Err("invalid cpu affinity cores");
    }
    Ok(unsafe {
        slice::from_raw_parts(config.cpu_affinity_cores_ptr, config.cpu_affinity_cores_len)
    })
}

fn check_size_config(config: &CacheStoreConfigView) -> Result<(), String> {
    let tensor_sizes = tensor_sizes(config).map_err(str::to_owned)?;
    if tensor_sizes.is_empty() {
        return Err("invalid tensor size".to_owned());
    }
    if config.shard_size == 0 {
        return Err("invalid shard size".to_owned());
    }
    if config.block_size == 0 {
        return Err("invalid block size".to_owned());
    }
    let total = tensor_sizes.iter().try_fold(0usize, |acc, size| {
        acc.checked_add(*size).ok_or("invalid shard size")
    })?;
    if total != config.shard_size {
        return Err(format!("invalid shard size({})", config.shard_size));
    }
    if config.block_size % config.shard_size != 0 {
        return Err(format!("invalid block size({})", config.block_size));
    }
    Ok(())
}

fn validate_config(config: &CacheStoreConfigView) -> Result<(), String> {
    if !config.store_backend_present {
        return Err("invalid store backend".to_owned());
    }
    if config.device_id < -1 {
        return Err(format!("invalid device({})", config.device_id));
    }
    if config.unique_id_ptr.is_null() || config.unique_id_len == 0 {
        return Err("invalid unique id".to_owned());
    }
    for core in cpu_affinity_cores(config).map_err(str::to_owned)? {
        if *core < 0 || (*core as usize) >= config.cpu_set_size {
            return Err(format!("invalid cpu core({})", core));
        }
    }
    if config.device_id == -1 {
        return Ok(());
    }

    check_size_config(config)?;

    let buffer_number = config.buffer_capacity / config.shard_size;
    if buffer_number < 1024 || buffer_number < config.load_exclusive_buffer_number.saturating_mul(2)
    {
        return Err(format!(
            "too small buffer({}) on shard({})",
            config.buffer_capacity, config.shard_size
        ));
    }
    if config.waiting_queue_depth <= 1 || config.running_queue_depth <= 1 {
        return Err(format!(
            "invalid queue depth({},{})",
            config.waiting_queue_depth, config.running_queue_depth
        ));
    }
    if config.stream_number < 1 || config.stream_number > 32 {
        return Err(format!("invalid stream number({})", config.stream_number));
    }
    Ok(())
}

fn set_status(status: *mut CacheStoreFfiStatus, result: Result<(), String>) {
    if status.is_null() {
        return;
    }
    let status = unsafe { &mut *status };
    match result {
        Ok(()) => status.ok(),
        Err(message) => status.error(STATUS_INVALID_PARAM, &message),
    }
}

fn require_const_ptr<T>(ptr: *const T, len: usize, name: &str) -> Result<(), String> {
    if len != 0 && ptr.is_null() {
        return Err(format!("invalid {name}"));
    }
    Ok(())
}

fn require_mut_ptr<T>(ptr: *mut T, len: usize, name: &str) -> Result<(), String> {
    if len != 0 && ptr.is_null() {
        return Err(format!("invalid {name}"));
    }
    Ok(())
}

fn require_out_ptr<T>(ptr: *mut T, name: &str) -> Result<(), String> {
    if ptr.is_null() {
        return Err(format!("invalid {name}"));
    }
    Ok(())
}

fn usize_to_isize(value: usize, name: &str) -> Result<isize, String> {
    isize::try_from(value).map_err(|_| format!("invalid {name}"))
}

fn last_lookup_index(num: usize) -> Result<isize, String> {
    if num == 0 {
        return Ok(-1);
    }
    usize_to_isize(num - 1, "lookup result index")
}

fn collect_misses(
    blocks: *const CacheStoreBlockId,
    local_hits: *const u8,
    num: usize,
    miss_blocks: *mut CacheStoreBlockId,
    miss_indices: *mut usize,
    miss_count: *mut usize,
) -> Result<(), String> {
    require_const_ptr(blocks, num, "lookup blocks")?;
    require_const_ptr(local_hits, num, "lookup local hits")?;
    require_mut_ptr(miss_blocks, num, "lookup miss blocks")?;
    require_mut_ptr(miss_indices, num, "lookup miss indices")?;
    require_out_ptr(miss_count, "lookup miss count")?;

    let mut count = 0usize;
    for i in 0..num {
        let hit = unsafe { *local_hits.add(i) != 0 };
        if hit {
            continue;
        }
        unsafe {
            *miss_blocks.add(count) = *blocks.add(i);
            *miss_indices.add(count) = i;
        }
        count += 1;
    }
    unsafe {
        *miss_count = count;
    }
    Ok(())
}

fn merge_lookup_results(
    results: *mut u8,
    num: usize,
    miss_indices: *const usize,
    miss_count: usize,
    backend_hits: *const u8,
    backend_hits_len: usize,
) -> Result<(), String> {
    if backend_hits_len != miss_count {
        return Err(format!(
            "backend lookup result length mismatch({backend_hits_len}!={miss_count})"
        ));
    }
    if miss_count > num {
        return Err(format!("invalid miss count({miss_count})"));
    }
    require_mut_ptr(results, num, "lookup results")?;
    require_const_ptr(miss_indices, miss_count, "lookup miss indices")?;
    require_const_ptr(backend_hits, backend_hits_len, "backend lookup results")?;

    for i in 0..miss_count {
        let idx = unsafe { *miss_indices.add(i) };
        if idx >= num {
            return Err(format!("invalid miss index({idx})"));
        }
        let hit = unsafe { *backend_hits.add(i) != 0 };
        unsafe {
            *results.add(idx) = u8::from(hit);
        }
    }
    Ok(())
}

fn lookup_prefix_result(
    num: usize,
    miss_indices: *const usize,
    miss_count: usize,
    backend_prefix: isize,
    out_prefix: *mut isize,
) -> Result<(), String> {
    require_out_ptr(out_prefix, "lookup prefix result")?;
    if miss_count == 0 {
        unsafe {
            *out_prefix = last_lookup_index(num)?;
        }
        return Ok(());
    }
    if miss_count > num {
        return Err(format!("invalid miss count({miss_count})"));
    }
    require_const_ptr(miss_indices, miss_count, "lookup miss indices")?;
    if backend_prefix < -1 {
        return Err(format!("invalid backend prefix({backend_prefix})"));
    }
    let next_miss = usize::try_from(backend_prefix + 1)
        .map_err(|_| format!("invalid backend prefix({backend_prefix})"))?;
    if next_miss > miss_count {
        return Err(format!("invalid backend prefix({backend_prefix})"));
    }
    let result = if next_miss == miss_count {
        last_lookup_index(num)?
    } else {
        let idx = unsafe { *miss_indices.add(next_miss) };
        if idx >= num {
            return Err(format!("invalid miss index({idx})"));
        }
        if idx == 0 {
            -1
        } else {
            usize_to_isize(idx - 1, "lookup result index")?
        }
    };
    unsafe {
        *out_prefix = result;
    }
    Ok(())
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_core_new(
    config: *const CacheStoreConfigView,
    status: *mut CacheStoreFfiStatus,
) -> *mut CacheStoreCore {
    if config.is_null() {
        if !status.is_null() {
            unsafe { &mut *status }.error(STATUS_ERROR, "invalid config pointer");
        }
        return std::ptr::null_mut();
    }
    let config = unsafe { &*config };
    match validate_config(config) {
        Ok(()) => {
            set_status(status, Ok(()));
            Box::into_raw(Box::new(CacheStoreCore {
                trans_enabled: config.device_id >= 0,
            }))
        }
        Err(message) => {
            set_status(status, Err(message));
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_core_free(core: *mut CacheStoreCore) {
    if !core.is_null() {
        unsafe {
            drop(Box::from_raw(core));
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_core_trans_enabled(core: *const CacheStoreCore) -> bool {
    if core.is_null() {
        return false;
    }
    unsafe { (*core).trans_enabled }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_next_task_id() -> usize {
    NEXT_TASK_ID.fetch_add(1, Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_lookup_collect_misses(
    blocks: *const CacheStoreBlockId,
    local_hits: *const u8,
    num: usize,
    miss_blocks: *mut CacheStoreBlockId,
    miss_indices: *mut usize,
    miss_count: *mut usize,
    status: *mut CacheStoreFfiStatus,
) {
    set_status(
        status,
        collect_misses(
            blocks,
            local_hits,
            num,
            miss_blocks,
            miss_indices,
            miss_count,
        ),
    );
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_lookup_merge(
    results: *mut u8,
    num: usize,
    miss_indices: *const usize,
    miss_count: usize,
    backend_hits: *const u8,
    backend_hits_len: usize,
    status: *mut CacheStoreFfiStatus,
) {
    set_status(
        status,
        merge_lookup_results(
            results,
            num,
            miss_indices,
            miss_count,
            backend_hits,
            backend_hits_len,
        ),
    );
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_lookup_prefix_result(
    num: usize,
    miss_indices: *const usize,
    miss_count: usize,
    backend_prefix: isize,
    out_prefix: *mut isize,
    status: *mut CacheStoreFfiStatus,
) {
    set_status(
        status,
        lookup_prefix_result(num, miss_indices, miss_count, backend_prefix, out_prefix),
    );
}

#[cfg(test)]
mod tests {
    use super::{collect_misses, lookup_prefix_result, merge_lookup_results, CacheStoreBlockId};

    fn block(id: u8) -> CacheStoreBlockId {
        CacheStoreBlockId { bytes: [id; 16] }
    }

    #[test]
    fn collect_mixed_misses() {
        let blocks = [block(1), block(2), block(3), block(4)];
        let local_hits = [1, 0, 1, 0];
        let mut miss_blocks = [block(0); 4];
        let mut miss_indices = [0usize; 4];
        let mut miss_count = 0usize;

        collect_misses(
            blocks.as_ptr(),
            local_hits.as_ptr(),
            blocks.len(),
            miss_blocks.as_mut_ptr(),
            miss_indices.as_mut_ptr(),
            &mut miss_count,
        )
        .unwrap();

        assert_eq!(miss_count, 2);
        assert_eq!(miss_blocks[0].bytes, block(2).bytes);
        assert_eq!(miss_blocks[1].bytes, block(4).bytes);
        assert_eq!(&miss_indices[..miss_count], &[1, 3]);
    }

    #[test]
    fn merge_backend_results_into_local_hits() {
        let mut results = [1, 0, 1, 0];
        let miss_indices = [1usize, 3];
        let backend_hits = [1, 0];

        merge_lookup_results(
            results.as_mut_ptr(),
            results.len(),
            miss_indices.as_ptr(),
            miss_indices.len(),
            backend_hits.as_ptr(),
            backend_hits.len(),
        )
        .unwrap();

        assert_eq!(results, [1, 1, 1, 0]);
    }

    #[test]
    fn prefix_result_all_local_hits() {
        let mut out = -2;
        lookup_prefix_result(3, std::ptr::null(), 0, -1, &mut out).unwrap();
        assert_eq!(out, 2);
    }

    #[test]
    fn prefix_result_uses_next_miss_index() {
        let miss_indices = [1usize, 3];
        let mut out = -2;
        lookup_prefix_result(4, miss_indices.as_ptr(), miss_indices.len(), 0, &mut out).unwrap();
        assert_eq!(out, 2);
    }

    #[test]
    fn prefix_result_first_backend_miss() {
        let miss_indices = [0usize, 2];
        let mut out = -2;
        lookup_prefix_result(4, miss_indices.as_ptr(), miss_indices.len(), -1, &mut out).unwrap();
        assert_eq!(out, -1);
    }

    #[test]
    fn prefix_result_backend_all_hit() {
        let miss_indices = [1usize, 3];
        let mut out = -2;
        lookup_prefix_result(4, miss_indices.as_ptr(), miss_indices.len(), 1, &mut out).unwrap();
        assert_eq!(out, 3);
    }

    #[test]
    fn reject_invalid_backend_prefix() {
        let miss_indices = [1usize, 3];
        let mut out = -2;
        let err = lookup_prefix_result(4, miss_indices.as_ptr(), miss_indices.len(), 2, &mut out)
            .unwrap_err();
        assert!(err.contains("invalid backend prefix"));
    }

    #[test]
    fn reject_backend_lookup_length_mismatch() {
        let mut results = [0, 0];
        let miss_indices = [0usize, 1];
        let backend_hits = [1];

        let err = merge_lookup_results(
            results.as_mut_ptr(),
            results.len(),
            miss_indices.as_ptr(),
            miss_indices.len(),
            backend_hits.as_ptr(),
            backend_hits.len(),
        )
        .unwrap_err();

        assert!(err.contains("length mismatch"));
    }
}
