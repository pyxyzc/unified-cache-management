use std::ffi::c_char;
use std::slice;
use std::sync::atomic::{AtomicUsize, Ordering};

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
