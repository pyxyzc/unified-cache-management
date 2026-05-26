use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::fs;
use std::hint;
use std::mem;
use std::ptr;
use std::slice;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::thread;
use std::time::Duration;

use super::{
    require_const_ptr, require_out_ptr, set_status, CacheStoreBlockId, CacheStoreFfiStatus,
    MESSAGE_CAPACITY, STATUS_OK,
};

const INVALID_INDEX: usize = usize::MAX;
const HASH_BUCKET_COUNT: usize = 16_411;
const SHARED_BUFFER_MAGIC: usize = 0x5362_5232; // SbR2
const SHARED_BUFFER_PREFIX: &str = "uc_shm_cache_rs2_";
const SHM_KEEP_SECONDS: u64 = 10 * 60;

const O_RDWR: c_int = 0o2;
const O_CREAT: c_int = 0o100;
const O_EXCL: c_int = 0o200;
const PROT_READ: c_int = 0x1;
const PROT_WRITE: c_int = 0x2;
const MAP_SHARED: c_int = 0x01;
const EEXIST: c_int = 17;
const SHM_MODE: u32 = 0o644;

type ModeT = u32;
type OffT = i64;

unsafe extern "C" {
    fn shm_open(name: *const c_char, oflag: c_int, mode: ModeT) -> c_int;
    fn shm_unlink(name: *const c_char) -> c_int;
    fn ftruncate(fd: c_int, length: OffT) -> c_int;
    fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: OffT,
    ) -> *mut c_void;
    fn munmap(addr: *mut c_void, length: usize) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn __errno_location() -> *mut c_int;
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CacheStoreTransBufferMetaNode {
    block: CacheStoreBlockId,
    shard: usize,
    reference: usize,
    hash: usize,
    prev: usize,
    next: usize,
    ready: bool,
}

impl CacheStoreTransBufferMetaNode {
    fn empty() -> Self {
        Self {
            block: CacheStoreBlockId { bytes: [0; 16] },
            shard: 0,
            reference: 0,
            hash: INVALID_INDEX,
            prev: INVALID_INDEX,
            next: INVALID_INDEX,
            ready: false,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CacheStoreTransBufferConfigView {
    share_buffer_enable: bool,
    bypass_hit_on_load: bool,
    io_direct: bool,
    unique_id_ptr: *const u8,
    unique_id_len: usize,
    device_id: i32,
    node_size: usize,
    total_size: usize,
    reserved_number: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CacheStoreTransBufferCallbacks {
    ctx: *mut c_void,
    make_local_host_buffer: Option<
        unsafe extern "C" fn(
            *mut c_void,
            i32,
            usize,
            bool,
            *mut *mut c_void,
            *mut *mut c_void,
            *mut CacheStoreFfiStatus,
        ),
    >,
    free_local_host_buffer: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    register_shared_host_buffer: Option<
        unsafe extern "C" fn(
            *mut c_void,
            i32,
            *mut c_void,
            usize,
            *mut *mut c_void,
            *mut CacheStoreFfiStatus,
        ),
    >,
    unregister_shared_host_buffer: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    page_size: Option<unsafe extern "C" fn(*mut c_void) -> usize>,
    shared_mutex_size: Option<unsafe extern "C" fn(*mut c_void) -> usize>,
    shared_mutex_align: Option<unsafe extern "C" fn(*mut c_void) -> usize>,
    shared_mutex_init: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool>,
    shared_mutex_lock: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    shared_mutex_try_lock: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool>,
    shared_mutex_unlock: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    shared_spin_size: Option<unsafe extern "C" fn(*mut c_void) -> usize>,
    shared_spin_align: Option<unsafe extern "C" fn(*mut c_void) -> usize>,
    shared_spin_init: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool>,
    shared_spin_lock: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    shared_spin_try_lock: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool>,
    shared_spin_unlock: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CacheStoreTransBufferGetResult {
    index: usize,
    owner: bool,
}

#[repr(C)]
struct SharedHeader {
    magic: AtomicUsize,
    n_node: usize,
    free_head: AtomicUsize,
    node_size: usize,
    bucket_count: usize,
    bucket_lock_size: usize,
    bucket_lock_align: usize,
    node_lock_size: usize,
    node_lock_align: usize,
    fetch_lock_offset: usize,
    bucket_locks_offset: usize,
    node_locks_offset: usize,
    buckets_offset: usize,
    meta_offset: usize,
    data_offset: usize,
    total_size: usize,
}

pub struct CacheStoreTransBufferCore {
    strategy: Strategy,
    bypass_hit_on_load: bool,
}

#[derive(Clone)]
struct Config {
    share_buffer_enable: bool,
    bypass_hit_on_load: bool,
    io_direct: bool,
    unique_id: String,
    device_id: i32,
    node_size: usize,
    total_size: usize,
    reserved_number: usize,
}

#[derive(Clone, Copy)]
struct Callbacks {
    view: CacheStoreTransBufferCallbacks,
}

enum Strategy {
    Local(LocalStrategy),
    Shared(SharedStrategy),
}

struct SpinLock {
    locked: AtomicBool,
}

struct LocalStrategy {
    callbacks: Callbacks,
    data: *mut u8,
    data_handle: *mut c_void,
    node_size: usize,
    n_node: usize,
    reserved_number: usize,
    free_head: AtomicUsize,
    buckets: Box<[UnsafeCell<usize>]>,
    bucket_locks: Box<[SpinLock]>,
    node_locks: Box<[SpinLock]>,
    meta: Box<[UnsafeCell<CacheStoreTransBufferMetaNode>]>,
}

struct SharedLayout {
    n_node: usize,
    node_size: usize,
    bucket_lock_size: usize,
    bucket_lock_align: usize,
    node_lock_size: usize,
    node_lock_align: usize,
    fetch_lock_offset: usize,
    bucket_locks_offset: usize,
    node_locks_offset: usize,
    buckets_offset: usize,
    meta_offset: usize,
    data_offset: usize,
    total_size: usize,
}

struct SharedStrategy {
    callbacks: Callbacks,
    shm_name: CString,
    fd: c_int,
    address: *mut u8,
    map_size: usize,
    data: *mut u8,
    registered_data: *mut c_void,
    registered: bool,
    watcher: bool,
    reserved_number: usize,
}

unsafe impl Sync for LocalStrategy {}
unsafe impl Sync for SharedStrategy {}

impl SpinLock {
    fn new() -> Self {
        Self {
            locked: AtomicBool::new(false),
        }
    }

    fn lock(&self) {
        while self
            .locked
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            while self.locked.load(Ordering::Relaxed) {
                hint::spin_loop();
            }
        }
    }

    fn try_lock(&self) -> bool {
        self.locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
    }

    fn unlock(&self) {
        self.locked.store(false, Ordering::Release);
    }
}

impl Config {
    unsafe fn from_view(view: *const CacheStoreTransBufferConfigView) -> Result<Self, String> {
        if view.is_null() {
            return Err("invalid trans buffer config".to_owned());
        }
        let view = &*view;
        let unique_id = if view.unique_id_len == 0 {
            String::new()
        } else {
            require_const_ptr(
                view.unique_id_ptr,
                view.unique_id_len,
                "trans buffer unique id",
            )?;
            String::from_utf8_lossy(slice::from_raw_parts(
                view.unique_id_ptr,
                view.unique_id_len,
            ))
            .into_owned()
        };
        let config = Self {
            share_buffer_enable: view.share_buffer_enable,
            bypass_hit_on_load: view.bypass_hit_on_load,
            io_direct: view.io_direct,
            unique_id,
            device_id: view.device_id,
            node_size: view.node_size,
            total_size: view.total_size,
            reserved_number: view.reserved_number,
        };
        config.validate()?;
        Ok(config)
    }

    fn validate(&self) -> Result<(), String> {
        if self.share_buffer_enable && self.unique_id.is_empty() {
            return Err("invalid trans buffer unique id".to_owned());
        }
        if self.share_buffer_enable && self.device_id < 0 {
            return Ok(());
        }
        if self.device_id < 0 {
            return Err(format!("invalid trans buffer device({})", self.device_id));
        }
        if self.node_size == 0 {
            return Err("invalid trans buffer node size".to_owned());
        }
        if self.total_size < self.node_size || self.total_size % self.node_size != 0 {
            return Err(format!(
                "invalid trans buffer size({}, {})",
                self.total_size, self.node_size
            ));
        }
        let n_node = self.n_node()?;
        if n_node < self.reserved_number {
            return Err(format!(
                "invalid trans buffer reserved nodes({}, {})",
                self.reserved_number, n_node
            ));
        }
        Ok(())
    }

    fn n_node(&self) -> Result<usize, String> {
        if self.node_size == 0 {
            return Err("invalid trans buffer node size".to_owned());
        }
        Ok(self.total_size / self.node_size)
    }
}

impl Callbacks {
    unsafe fn from_view(view: *const CacheStoreTransBufferCallbacks) -> Result<Self, String> {
        if view.is_null() {
            return Err("invalid trans buffer callbacks".to_owned());
        }
        Ok(Self { view: *view })
    }

    fn validate_for_local(&self) -> Result<(), String> {
        if self.view.make_local_host_buffer.is_none() || self.view.free_local_host_buffer.is_none()
        {
            return Err("invalid local trans buffer callbacks".to_owned());
        }
        Ok(())
    }

    fn validate_for_shared(&self) -> Result<(), String> {
        if self.view.register_shared_host_buffer.is_none()
            || self.view.unregister_shared_host_buffer.is_none()
            || self.view.page_size.is_none()
            || self.view.shared_mutex_size.is_none()
            || self.view.shared_mutex_align.is_none()
            || self.view.shared_mutex_init.is_none()
            || self.view.shared_mutex_lock.is_none()
            || self.view.shared_mutex_try_lock.is_none()
            || self.view.shared_mutex_unlock.is_none()
            || self.view.shared_spin_size.is_none()
            || self.view.shared_spin_align.is_none()
            || self.view.shared_spin_init.is_none()
            || self.view.shared_spin_lock.is_none()
            || self.view.shared_spin_try_lock.is_none()
            || self.view.shared_spin_unlock.is_none()
        {
            return Err("invalid shared trans buffer callbacks".to_owned());
        }
        Ok(())
    }

    unsafe fn make_local_host_buffer(
        &self,
        device_id: i32,
        size: usize,
        io_direct: bool,
    ) -> Result<(*mut u8, *mut c_void), String> {
        let mut data: *mut c_void = ptr::null_mut();
        let mut handle: *mut c_void = ptr::null_mut();
        let mut status = empty_status();
        self.view.make_local_host_buffer.unwrap_unchecked()(
            self.view.ctx,
            device_id,
            size,
            io_direct,
            &mut data,
            &mut handle,
            &mut status,
        );
        status_to_result(&status, "failed to make local trans buffer")?;
        if data.is_null() || handle.is_null() {
            return Err("failed to make local trans buffer".to_owned());
        }
        Ok((data.cast::<u8>(), handle))
    }

    unsafe fn free_local_host_buffer(&self, handle: *mut c_void) {
        if !handle.is_null() {
            self.view.free_local_host_buffer.unwrap_unchecked()(self.view.ctx, handle);
        }
    }

    unsafe fn register_shared_host_buffer(
        &self,
        device_id: i32,
        data: *mut c_void,
        size: usize,
    ) -> Result<*mut c_void, String> {
        let mut device_data: *mut c_void = ptr::null_mut();
        let mut status = empty_status();
        self.view.register_shared_host_buffer.unwrap_unchecked()(
            self.view.ctx,
            device_id,
            data,
            size,
            &mut device_data,
            &mut status,
        );
        status_to_result(&status, "failed to register shared trans buffer")?;
        Ok(device_data)
    }

    unsafe fn unregister_shared_host_buffer(&self, data: *mut c_void) {
        if !data.is_null() {
            self.view.unregister_shared_host_buffer.unwrap_unchecked()(self.view.ctx, data);
        }
    }

    unsafe fn page_size(&self) -> usize {
        self.view.page_size.unwrap_unchecked()(self.view.ctx)
    }

    unsafe fn shared_mutex_size(&self) -> usize {
        self.view.shared_mutex_size.unwrap_unchecked()(self.view.ctx)
    }

    unsafe fn shared_mutex_align(&self) -> usize {
        self.view.shared_mutex_align.unwrap_unchecked()(self.view.ctx)
    }

    unsafe fn shared_mutex_init(&self, ptr: *mut u8) -> bool {
        self.view.shared_mutex_init.unwrap_unchecked()(self.view.ctx, ptr.cast::<c_void>())
    }

    unsafe fn shared_mutex_lock(&self, ptr: *mut u8) {
        self.view.shared_mutex_lock.unwrap_unchecked()(self.view.ctx, ptr.cast::<c_void>());
    }

    unsafe fn shared_mutex_try_lock(&self, ptr: *mut u8) -> bool {
        self.view.shared_mutex_try_lock.unwrap_unchecked()(self.view.ctx, ptr.cast::<c_void>())
    }

    unsafe fn shared_mutex_unlock(&self, ptr: *mut u8) {
        self.view.shared_mutex_unlock.unwrap_unchecked()(self.view.ctx, ptr.cast::<c_void>());
    }

    unsafe fn shared_spin_size(&self) -> usize {
        self.view.shared_spin_size.unwrap_unchecked()(self.view.ctx)
    }

    unsafe fn shared_spin_align(&self) -> usize {
        self.view.shared_spin_align.unwrap_unchecked()(self.view.ctx)
    }

    unsafe fn shared_spin_init(&self, ptr: *mut u8) -> bool {
        self.view.shared_spin_init.unwrap_unchecked()(self.view.ctx, ptr.cast::<c_void>())
    }

    unsafe fn shared_spin_lock(&self, ptr: *mut u8) {
        self.view.shared_spin_lock.unwrap_unchecked()(self.view.ctx, ptr.cast::<c_void>());
    }

    unsafe fn shared_spin_unlock(&self, ptr: *mut u8) {
        self.view.shared_spin_unlock.unwrap_unchecked()(self.view.ctx, ptr.cast::<c_void>());
    }
}

impl Strategy {
    unsafe fn new(config: &Config, callbacks: Callbacks) -> Result<Self, String> {
        if config.share_buffer_enable {
            Ok(Self::Shared(SharedStrategy::new(config, callbacks)?))
        } else {
            Ok(Self::Local(LocalStrategy::new(config, callbacks)?))
        }
    }

    unsafe fn bucket_lock(&self, i_bucket: usize) {
        match self {
            Self::Local(strategy) => strategy.bucket_lock(i_bucket),
            Self::Shared(strategy) => strategy.bucket_lock(i_bucket),
        }
    }

    unsafe fn bucket_try_lock(&self, i_bucket: usize) -> bool {
        match self {
            Self::Local(strategy) => strategy.bucket_try_lock(i_bucket),
            Self::Shared(strategy) => strategy.bucket_try_lock(i_bucket),
        }
    }

    unsafe fn bucket_unlock(&self, i_bucket: usize) {
        match self {
            Self::Local(strategy) => strategy.bucket_unlock(i_bucket),
            Self::Shared(strategy) => strategy.bucket_unlock(i_bucket),
        }
    }

    unsafe fn node_lock(&self, i_node: usize) {
        match self {
            Self::Local(strategy) => strategy.node_lock(i_node),
            Self::Shared(strategy) => strategy.node_lock(i_node),
        }
    }

    unsafe fn node_unlock(&self, i_node: usize) {
        match self {
            Self::Local(strategy) => strategy.node_unlock(i_node),
            Self::Shared(strategy) => strategy.node_unlock(i_node),
        }
    }

    unsafe fn first_at(&self, i_bucket: usize) -> *mut usize {
        match self {
            Self::Local(strategy) => strategy.first_at(i_bucket),
            Self::Shared(strategy) => strategy.first_at(i_bucket),
        }
    }

    unsafe fn fetch_node(&self, allow_reserved: bool) -> usize {
        match self {
            Self::Local(strategy) => strategy.fetch_node(allow_reserved),
            Self::Shared(strategy) => strategy.fetch_node(allow_reserved),
        }
    }

    unsafe fn meta_at(&self, i_node: usize) -> *mut CacheStoreTransBufferMetaNode {
        match self {
            Self::Local(strategy) => strategy.meta_at(i_node),
            Self::Shared(strategy) => strategy.meta_at(i_node),
        }
    }

    fn data_at(&self, i_node: usize) -> *mut c_void {
        match self {
            Self::Local(strategy) => strategy.data_at(i_node),
            Self::Shared(strategy) => strategy.data_at(i_node),
        }
    }

    fn bucket_of(&self, block: &CacheStoreBlockId, shard: usize) -> usize {
        stable_hash(block, shard) % HASH_BUCKET_COUNT
    }

    fn n_node(&self) -> usize {
        match self {
            Self::Local(strategy) => strategy.n_node,
            Self::Shared(strategy) => strategy.n_node(),
        }
    }
}

impl LocalStrategy {
    unsafe fn new(config: &Config, callbacks: Callbacks) -> Result<Self, String> {
        callbacks.validate_for_local()?;
        let n_node = config.n_node()?;
        let total_size = config
            .node_size
            .checked_mul(n_node)
            .ok_or_else(|| "invalid local trans buffer size".to_owned())?;
        let (data, data_handle) =
            callbacks.make_local_host_buffer(config.device_id, total_size, config.io_direct)?;
        let buckets = (0..HASH_BUCKET_COUNT)
            .map(|_| UnsafeCell::new(INVALID_INDEX))
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let bucket_locks = (0..HASH_BUCKET_COUNT)
            .map(|_| SpinLock::new())
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let node_locks = (0..n_node)
            .map(|_| SpinLock::new())
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let meta = (0..n_node)
            .map(|_| UnsafeCell::new(CacheStoreTransBufferMetaNode::empty()))
            .collect::<Vec<_>>()
            .into_boxed_slice();
        Ok(Self {
            callbacks,
            data,
            data_handle,
            node_size: config.node_size,
            n_node,
            reserved_number: config.reserved_number,
            free_head: AtomicUsize::new(0),
            buckets,
            bucket_locks,
            node_locks,
            meta,
        })
    }

    fn bucket_lock(&self, i_bucket: usize) {
        self.bucket_locks[i_bucket].lock();
    }

    fn bucket_try_lock(&self, i_bucket: usize) -> bool {
        self.bucket_locks[i_bucket].try_lock()
    }

    fn bucket_unlock(&self, i_bucket: usize) {
        self.bucket_locks[i_bucket].unlock();
    }

    fn node_lock(&self, i_node: usize) {
        self.node_locks[i_node].lock();
    }

    fn node_unlock(&self, i_node: usize) {
        self.node_locks[i_node].unlock();
    }

    fn first_at(&self, i_bucket: usize) -> *mut usize {
        self.buckets[i_bucket].get()
    }

    fn fetch_node(&self, allow_reserved: bool) -> usize {
        let limit = self.n_node
            - if allow_reserved {
                0
            } else {
                self.reserved_number
            };
        loop {
            let current = self.free_head.load(Ordering::Relaxed);
            let (node, next) = if current >= limit {
                (0, 1)
            } else {
                (current, current + 1)
            };
            if self
                .free_head
                .compare_exchange(current, next, Ordering::AcqRel, Ordering::Relaxed)
                .is_ok()
            {
                return node;
            }
        }
    }

    fn meta_at(&self, i_node: usize) -> *mut CacheStoreTransBufferMetaNode {
        self.meta[i_node].get()
    }

    fn data_at(&self, i_node: usize) -> *mut c_void {
        if i_node >= self.n_node || self.data.is_null() {
            return ptr::null_mut();
        }
        unsafe { self.data.add(self.node_size * i_node).cast::<c_void>() }
    }
}

impl Drop for LocalStrategy {
    fn drop(&mut self) {
        unsafe {
            self.callbacks.free_local_host_buffer(self.data_handle);
        }
        self.data_handle = ptr::null_mut();
        self.data = ptr::null_mut();
    }
}

impl SharedLayout {
    unsafe fn new(n_node: usize, node_size: usize, callbacks: Callbacks) -> Result<Self, String> {
        let bucket_lock_size = callbacks.shared_mutex_size();
        let bucket_lock_align = callbacks.shared_mutex_align();
        let node_lock_size = callbacks.shared_spin_size();
        let node_lock_align = callbacks.shared_spin_align();
        let page_size = callbacks.page_size();
        if bucket_lock_size == 0
            || bucket_lock_align == 0
            || node_lock_size == 0
            || node_lock_align == 0
            || page_size == 0
        {
            return Err("invalid shared trans buffer lock layout".to_owned());
        }

        let mut offset = mem::size_of::<SharedHeader>();
        offset = align_up(offset, node_lock_align)?;
        let fetch_lock_offset = offset;
        offset = checked_add(offset, node_lock_size)?;
        offset = align_up(offset, bucket_lock_align)?;
        let bucket_locks_offset = offset;
        offset = checked_add(offset, checked_mul(bucket_lock_size, HASH_BUCKET_COUNT)?)?;
        offset = align_up(offset, node_lock_align)?;
        let node_locks_offset = offset;
        offset = checked_add(offset, checked_mul(node_lock_size, n_node)?)?;
        offset = align_up(offset, mem::align_of::<usize>())?;
        let buckets_offset = offset;
        offset = checked_add(
            offset,
            checked_mul(mem::size_of::<usize>(), HASH_BUCKET_COUNT)?,
        )?;
        offset = align_up(offset, mem::align_of::<CacheStoreTransBufferMetaNode>())?;
        let meta_offset = offset;
        offset = checked_add(
            offset,
            checked_mul(mem::size_of::<CacheStoreTransBufferMetaNode>(), n_node)?,
        )?;
        let data_offset = align_up(offset, page_size)?;
        let total_size = checked_add(data_offset, checked_mul(node_size, n_node)?)?;
        Ok(Self {
            n_node,
            node_size,
            bucket_lock_size,
            bucket_lock_align,
            node_lock_size,
            node_lock_align,
            fetch_lock_offset,
            bucket_locks_offset,
            node_locks_offset,
            buckets_offset,
            meta_offset,
            data_offset,
            total_size,
        })
    }

    fn from_header(header: &SharedHeader) -> Result<Self, String> {
        if header.magic.load(Ordering::Acquire) != SHARED_BUFFER_MAGIC {
            return Err("shared trans buffer is not ready".to_owned());
        }
        if header.bucket_count != HASH_BUCKET_COUNT || header.n_node == 0 {
            return Err("invalid shared trans buffer header".to_owned());
        }
        Ok(Self {
            n_node: header.n_node,
            node_size: header.node_size,
            bucket_lock_size: header.bucket_lock_size,
            bucket_lock_align: header.bucket_lock_align,
            node_lock_size: header.node_lock_size,
            node_lock_align: header.node_lock_align,
            fetch_lock_offset: header.fetch_lock_offset,
            bucket_locks_offset: header.bucket_locks_offset,
            node_locks_offset: header.node_locks_offset,
            buckets_offset: header.buckets_offset,
            meta_offset: header.meta_offset,
            data_offset: header.data_offset,
            total_size: header.total_size,
        })
    }
}

impl SharedStrategy {
    unsafe fn new(config: &Config, callbacks: Callbacks) -> Result<Self, String> {
        callbacks.validate_for_shared()?;
        let watcher = config.device_id < 0;
        let shm_name = shared_name(&config.unique_id)?;
        cleanup_shm_files_except_me(shm_name.to_str().unwrap_or_default());
        if watcher {
            return Self::load_watcher(config, callbacks, shm_name);
        }
        let n_node = config.n_node()?;
        let layout = SharedLayout::new(n_node, config.node_size, callbacks)?;
        let fd = shm_open(shm_name.as_ptr(), O_RDWR | O_CREAT | O_EXCL, SHM_MODE);
        if fd >= 0 {
            let mut strategy = Self::empty(callbacks, shm_name, fd, config.reserved_number, false);
            strategy.create_mapping(config.device_id, layout)?;
            return Ok(strategy);
        }
        if errno() != EEXIST {
            return Err("failed to create shared trans buffer".to_owned());
        }
        Self::load_existing(config, callbacks, shm_name)
    }

    fn empty(
        callbacks: Callbacks,
        shm_name: CString,
        fd: c_int,
        reserved_number: usize,
        watcher: bool,
    ) -> Self {
        Self {
            callbacks,
            shm_name,
            fd,
            address: ptr::null_mut(),
            map_size: 0,
            data: ptr::null_mut(),
            registered_data: ptr::null_mut(),
            registered: false,
            watcher,
            reserved_number,
        }
    }

    unsafe fn create_mapping(
        &mut self,
        device_id: i32,
        layout: SharedLayout,
    ) -> Result<(), String> {
        if ftruncate(self.fd, layout.total_size as OffT) != 0 {
            return Err("failed to resize shared trans buffer".to_owned());
        }
        self.map(layout.total_size)?;
        self.init_header(&layout)?;
        self.register_buffer(device_id, &layout)
    }

    unsafe fn load_existing(
        config: &Config,
        callbacks: Callbacks,
        shm_name: CString,
    ) -> Result<Self, String> {
        let fd = shm_open(shm_name.as_ptr(), O_RDWR, SHM_MODE);
        if fd < 0 {
            return Err("failed to open shared trans buffer".to_owned());
        }
        let mut strategy = Self::empty(callbacks, shm_name, fd, config.reserved_number, false);
        let layout = strategy.read_layout_from_header()?;
        strategy.remap(layout.total_size)?;
        strategy.register_buffer(config.device_id, &layout)?;
        Ok(strategy)
    }

    unsafe fn load_watcher(
        config: &Config,
        callbacks: Callbacks,
        shm_name: CString,
    ) -> Result<Self, String> {
        let fd = shm_open(shm_name.as_ptr(), O_RDWR, SHM_MODE);
        if fd < 0 {
            return Err("failed to open shared trans buffer watcher".to_owned());
        }
        let mut strategy = Self::empty(callbacks, shm_name, fd, config.reserved_number, true);
        let layout = strategy.read_layout_from_header()?;
        strategy.remap(layout.data_offset)?;
        Ok(strategy)
    }

    unsafe fn map(&mut self, size: usize) -> Result<(), String> {
        let address = mmap(
            ptr::null_mut(),
            size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            self.fd,
            0,
        );
        if address as isize == -1 {
            return Err("failed to mmap shared trans buffer".to_owned());
        }
        self.address = address.cast::<u8>();
        self.map_size = size;
        Ok(())
    }

    unsafe fn remap(&mut self, size: usize) -> Result<(), String> {
        self.unmap();
        self.map(size)
    }

    unsafe fn unmap(&mut self) {
        if !self.address.is_null() && self.map_size != 0 {
            munmap(self.address.cast::<c_void>(), self.map_size);
        }
        self.address = ptr::null_mut();
        self.map_size = 0;
        self.data = ptr::null_mut();
    }

    unsafe fn read_layout_from_header(&mut self) -> Result<SharedLayout, String> {
        self.map(mem::size_of::<SharedHeader>())?;
        self.wait_ready()?;
        let layout = SharedLayout::from_header(self.header())?;
        Ok(layout)
    }

    unsafe fn init_header(&mut self, layout: &SharedLayout) -> Result<(), String> {
        let header = self.header_mut();
        ptr::write(
            header,
            SharedHeader {
                magic: AtomicUsize::new(0),
                n_node: layout.n_node,
                free_head: AtomicUsize::new(0),
                node_size: layout.node_size,
                bucket_count: HASH_BUCKET_COUNT,
                bucket_lock_size: layout.bucket_lock_size,
                bucket_lock_align: layout.bucket_lock_align,
                node_lock_size: layout.node_lock_size,
                node_lock_align: layout.node_lock_align,
                fetch_lock_offset: layout.fetch_lock_offset,
                bucket_locks_offset: layout.bucket_locks_offset,
                node_locks_offset: layout.node_locks_offset,
                buckets_offset: layout.buckets_offset,
                meta_offset: layout.meta_offset,
                data_offset: layout.data_offset,
                total_size: layout.total_size,
            },
        );
        if !self
            .callbacks
            .shared_spin_init(self.address.add(layout.fetch_lock_offset))
        {
            return Err("failed to initialize shared trans buffer fetch lock".to_owned());
        }
        for i in 0..HASH_BUCKET_COUNT {
            if !self
                .callbacks
                .shared_mutex_init(self.bucket_lock_at(layout, i))
            {
                return Err("failed to initialize shared trans buffer bucket lock".to_owned());
            }
            *self.first_at_layout(layout, i) = INVALID_INDEX;
        }
        for i in 0..layout.n_node {
            if !self
                .callbacks
                .shared_spin_init(self.node_lock_at(layout, i))
            {
                return Err("failed to initialize shared trans buffer node lock".to_owned());
            }
            ptr::write(
                self.meta_at_layout(layout, i),
                CacheStoreTransBufferMetaNode::empty(),
            );
        }
        (*header)
            .magic
            .store(SHARED_BUFFER_MAGIC, Ordering::Release);
        Ok(())
    }

    unsafe fn wait_ready(&self) -> Result<(), String> {
        for _ in 0..=100 {
            if self.header().magic.load(Ordering::Acquire) == SHARED_BUFFER_MAGIC {
                return Ok(());
            }
            thread::sleep(Duration::from_millis(100));
        }
        Err("shared trans buffer is not ready".to_owned())
    }

    unsafe fn register_buffer(
        &mut self,
        device_id: i32,
        layout: &SharedLayout,
    ) -> Result<(), String> {
        self.data = self.address.add(layout.data_offset);
        let size = checked_mul(layout.node_size, layout.n_node)?;
        self.registered_data = self.callbacks.register_shared_host_buffer(
            device_id,
            self.data.cast::<c_void>(),
            size,
        )?;
        self.registered = true;
        Ok(())
    }

    unsafe fn header(&self) -> &SharedHeader {
        &*(self.address.cast::<SharedHeader>())
    }

    unsafe fn header_mut(&mut self) -> *mut SharedHeader {
        self.address.cast::<SharedHeader>()
    }

    unsafe fn layout(&self) -> SharedLayout {
        SharedLayout::from_header(self.header()).unwrap_unchecked()
    }

    fn n_node(&self) -> usize {
        if self.address.is_null() {
            return 0;
        }
        unsafe { self.header().n_node }
    }

    unsafe fn fetch_lock(&self, layout: &SharedLayout) -> *mut u8 {
        self.address.add(layout.fetch_lock_offset)
    }

    unsafe fn bucket_lock_at(&self, layout: &SharedLayout, i_bucket: usize) -> *mut u8 {
        self.address
            .add(layout.bucket_locks_offset + layout.bucket_lock_size * i_bucket)
    }

    unsafe fn node_lock_at(&self, layout: &SharedLayout, i_node: usize) -> *mut u8 {
        self.address
            .add(layout.node_locks_offset + layout.node_lock_size * i_node)
    }

    unsafe fn first_at_layout(&self, layout: &SharedLayout, i_bucket: usize) -> *mut usize {
        self.address
            .add(layout.buckets_offset)
            .cast::<usize>()
            .add(i_bucket)
    }

    unsafe fn meta_at_layout(
        &self,
        layout: &SharedLayout,
        i_node: usize,
    ) -> *mut CacheStoreTransBufferMetaNode {
        self.address
            .add(layout.meta_offset)
            .cast::<CacheStoreTransBufferMetaNode>()
            .add(i_node)
    }

    unsafe fn bucket_lock(&self, i_bucket: usize) {
        let layout = self.layout();
        self.callbacks
            .shared_mutex_lock(self.bucket_lock_at(&layout, i_bucket));
    }

    unsafe fn bucket_try_lock(&self, i_bucket: usize) -> bool {
        let layout = self.layout();
        self.callbacks
            .shared_mutex_try_lock(self.bucket_lock_at(&layout, i_bucket))
    }

    unsafe fn bucket_unlock(&self, i_bucket: usize) {
        let layout = self.layout();
        self.callbacks
            .shared_mutex_unlock(self.bucket_lock_at(&layout, i_bucket));
    }

    unsafe fn node_lock(&self, i_node: usize) {
        let layout = self.layout();
        self.callbacks
            .shared_spin_lock(self.node_lock_at(&layout, i_node));
    }

    unsafe fn node_unlock(&self, i_node: usize) {
        let layout = self.layout();
        self.callbacks
            .shared_spin_unlock(self.node_lock_at(&layout, i_node));
    }

    unsafe fn first_at(&self, i_bucket: usize) -> *mut usize {
        let layout = self.layout();
        self.first_at_layout(&layout, i_bucket)
    }

    unsafe fn fetch_node(&self, allow_reserved: bool) -> usize {
        let layout = self.layout();
        let limit = layout.n_node
            - if allow_reserved {
                0
            } else {
                self.reserved_number
            };
        let lock = self.fetch_lock(&layout);
        self.callbacks.shared_spin_lock(lock);
        let header = self.header();
        let current = header.free_head.load(Ordering::Relaxed);
        let node = if current >= limit { 0 } else { current };
        header.free_head.store(node + 1, Ordering::Relaxed);
        self.callbacks.shared_spin_unlock(lock);
        node
    }

    unsafe fn meta_at(&self, i_node: usize) -> *mut CacheStoreTransBufferMetaNode {
        let layout = self.layout();
        self.meta_at_layout(&layout, i_node)
    }

    fn data_at(&self, i_node: usize) -> *mut c_void {
        if self.watcher || self.data.is_null() || i_node >= self.n_node() {
            return ptr::null_mut();
        }
        unsafe {
            self.data
                .add(self.header().node_size * i_node)
                .cast::<c_void>()
        }
    }
}

impl Drop for SharedStrategy {
    fn drop(&mut self) {
        unsafe {
            if self.registered {
                self.callbacks
                    .unregister_shared_host_buffer(self.data.cast::<c_void>());
            }
            self.unmap();
            if self.fd >= 0 {
                close(self.fd);
                self.fd = -1;
            }
            shm_unlink(self.shm_name.as_ptr());
        }
    }
}

impl CacheStoreTransBufferCore {
    unsafe fn new(
        config: *const CacheStoreTransBufferConfigView,
        callbacks: *const CacheStoreTransBufferCallbacks,
    ) -> Result<Self, String> {
        let config = Config::from_view(config)?;
        let callbacks = Callbacks::from_view(callbacks)?;
        Ok(Self {
            strategy: Strategy::new(&config, callbacks)?,
            bypass_hit_on_load: config.bypass_hit_on_load,
        })
    }

    unsafe fn get(
        &self,
        block: CacheStoreBlockId,
        shard: usize,
        allow_reserved: bool,
        is_load: bool,
    ) -> CacheStoreTransBufferGetResult {
        let i_bucket = self.strategy.bucket_of(&block, shard);
        self.strategy.bucket_lock(i_bucket);
        let mut owner = false;
        let mut i_node = self.find_at(i_bucket, &block, shard, &mut owner);
        if i_node != INVALID_INDEX {
            if self.bypass_hit_on_load && is_load && owner && self.ready(i_node) {
                self.mark_not_ready(i_node);
            }
            self.strategy.bucket_unlock(i_bucket);
            return CacheStoreTransBufferGetResult {
                index: i_node,
                owner,
            };
        }
        i_node = self.alloc(block, shard, i_bucket, allow_reserved);
        self.strategy.bucket_unlock(i_bucket);
        CacheStoreTransBufferGetResult {
            index: i_node,
            owner: true,
        }
    }

    unsafe fn exist(&self, block: &CacheStoreBlockId, shard: usize) -> bool {
        let i_bucket = self.strategy.bucket_of(block, shard);
        self.strategy.bucket_lock(i_bucket);
        let exist = self.exist_at(i_bucket, block, shard);
        self.strategy.bucket_unlock(i_bucket);
        exist
    }

    unsafe fn exist_at(&self, i_bucket: usize, block: &CacheStoreBlockId, shard: usize) -> bool {
        let mut i_node = *self.strategy.first_at(i_bucket);
        while i_node != INVALID_INDEX {
            let meta = self.strategy.meta_at(i_node);
            self.strategy.node_lock(i_node);
            if (*meta).block == *block && (*meta).shard == shard {
                self.strategy.node_unlock(i_node);
                return true;
            }
            let next = (*meta).next;
            self.strategy.node_unlock(i_node);
            i_node = next;
        }
        false
    }

    unsafe fn find_at(
        &self,
        i_bucket: usize,
        block: &CacheStoreBlockId,
        shard: usize,
        owner: &mut bool,
    ) -> usize {
        let mut i_node = *self.strategy.first_at(i_bucket);
        while i_node != INVALID_INDEX {
            let meta = self.strategy.meta_at(i_node);
            self.strategy.node_lock(i_node);
            if (*meta).block == *block && (*meta).shard == shard {
                *owner = (*meta).reference == 0;
                (*meta).reference += 1;
                self.strategy.node_unlock(i_node);
                return i_node;
            }
            let next = (*meta).next;
            self.strategy.node_unlock(i_node);
            i_node = next;
        }
        INVALID_INDEX
    }

    unsafe fn alloc(
        &self,
        block: CacheStoreBlockId,
        shard: usize,
        i_bucket: usize,
        allow_reserved: bool,
    ) -> usize {
        loop {
            let i_node = self.strategy.fetch_node(allow_reserved);
            let meta = self.strategy.meta_at(i_node);
            self.strategy.node_lock(i_node);
            if (*meta).reference > 0 {
                self.strategy.node_unlock(i_node);
                continue;
            }
            let old_bucket = (*meta).hash;
            if old_bucket != i_bucket {
                if old_bucket != INVALID_INDEX {
                    if !self.strategy.bucket_try_lock(old_bucket) {
                        self.strategy.node_unlock(i_node);
                        continue;
                    }
                    self.remove(old_bucket, i_node);
                    self.strategy.bucket_unlock(old_bucket);
                }
                self.move_to(i_bucket, i_node);
            }
            (*meta).reference += 1;
            (*meta).block = block;
            (*meta).shard = shard;
            (*meta).ready = false;
            self.strategy.node_unlock(i_node);
            return i_node;
        }
    }

    unsafe fn move_to(&self, i_bucket: usize, i_node: usize) {
        let meta = self.strategy.meta_at(i_node);
        let head = self.strategy.first_at(i_bucket);
        let next_node = *head;
        (*meta).next = next_node;
        (*meta).prev = INVALID_INDEX;
        if next_node != INVALID_INDEX {
            let next = self.strategy.meta_at(next_node);
            self.strategy.node_lock(next_node);
            (*next).prev = i_node;
            self.strategy.node_unlock(next_node);
        }
        (*meta).hash = i_bucket;
        *head = i_node;
    }

    unsafe fn remove(&self, i_bucket: usize, i_node: usize) {
        let meta = self.strategy.meta_at(i_node);
        let prev_node = (*meta).prev;
        if prev_node != INVALID_INDEX {
            let prev = self.strategy.meta_at(prev_node);
            self.strategy.node_lock(prev_node);
            (*prev).next = (*meta).next;
            self.strategy.node_unlock(prev_node);
        }
        let next_node = (*meta).next;
        if next_node != INVALID_INDEX {
            let next = self.strategy.meta_at(next_node);
            self.strategy.node_lock(next_node);
            (*next).prev = (*meta).prev;
            self.strategy.node_unlock(next_node);
        }
        let head = self.strategy.first_at(i_bucket);
        if *head == i_node {
            *head = next_node;
        }
        (*meta).prev = INVALID_INDEX;
        (*meta).next = INVALID_INDEX;
        (*meta).hash = INVALID_INDEX;
    }

    unsafe fn acquire(&self, pos: usize) {
        if pos >= self.strategy.n_node() {
            return;
        }
        self.strategy.node_lock(pos);
        (*self.strategy.meta_at(pos)).reference += 1;
        self.strategy.node_unlock(pos);
    }

    unsafe fn release(&self, pos: usize) {
        if pos >= self.strategy.n_node() {
            return;
        }
        self.strategy.node_lock(pos);
        let meta = self.strategy.meta_at(pos);
        if (*meta).reference > 0 {
            (*meta).reference -= 1;
        }
        self.strategy.node_unlock(pos);
    }

    unsafe fn ready(&self, pos: usize) -> bool {
        if pos >= self.strategy.n_node() {
            return false;
        }
        self.strategy.node_lock(pos);
        let ready = (*self.strategy.meta_at(pos)).ready;
        self.strategy.node_unlock(pos);
        ready
    }

    unsafe fn mark_ready(&self, pos: usize) {
        if pos >= self.strategy.n_node() {
            return;
        }
        self.strategy.node_lock(pos);
        (*self.strategy.meta_at(pos)).ready = true;
        self.strategy.node_unlock(pos);
    }

    unsafe fn mark_not_ready(&self, pos: usize) {
        if pos >= self.strategy.n_node() {
            return;
        }
        self.strategy.node_lock(pos);
        (*self.strategy.meta_at(pos)).ready = false;
        self.strategy.node_unlock(pos);
    }

    fn data_at(&self, pos: usize) -> *mut c_void {
        self.strategy.data_at(pos)
    }
}

unsafe fn core_from_ptr<'a>(
    core: *const CacheStoreTransBufferCore,
) -> Result<&'a CacheStoreTransBufferCore, String> {
    if core.is_null() {
        return Err("invalid trans buffer core".to_owned());
    }
    Ok(&*core)
}

fn trans_buffer_get(
    core: *const CacheStoreTransBufferCore,
    block: *const CacheStoreBlockId,
    shard: usize,
    allow_reserved: bool,
    is_load: bool,
    out: *mut CacheStoreTransBufferGetResult,
) -> Result<(), String> {
    require_const_ptr(block, 1, "trans buffer block")?;
    require_out_ptr(out, "trans buffer get result")?;
    let result = unsafe { core_from_ptr(core)?.get(*block, shard, allow_reserved, is_load) };
    unsafe {
        *out = result;
    }
    Ok(())
}

fn trans_buffer_exist(
    core: *const CacheStoreTransBufferCore,
    block: *const CacheStoreBlockId,
    shard: usize,
    out: *mut bool,
) -> Result<(), String> {
    require_const_ptr(block, 1, "trans buffer block")?;
    require_out_ptr(out, "trans buffer exist result")?;
    let result = unsafe { core_from_ptr(core)?.exist(&*block, shard) };
    unsafe {
        *out = result;
    }
    Ok(())
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_new(
    config: *const CacheStoreTransBufferConfigView,
    callbacks: *const CacheStoreTransBufferCallbacks,
    status: *mut CacheStoreFfiStatus,
) -> *mut CacheStoreTransBufferCore {
    let result = unsafe { CacheStoreTransBufferCore::new(config, callbacks) };
    match result {
        Ok(core) => {
            set_status(status, Ok(()));
            Box::into_raw(Box::new(core))
        }
        Err(message) => {
            set_status(status, Err(message));
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_free(core: *mut CacheStoreTransBufferCore) {
    if !core.is_null() {
        unsafe {
            drop(Box::from_raw(core));
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_get(
    core: *const CacheStoreTransBufferCore,
    block: *const CacheStoreBlockId,
    shard: usize,
    allow_reserved: bool,
    is_load: bool,
    out: *mut CacheStoreTransBufferGetResult,
    status: *mut CacheStoreFfiStatus,
) {
    set_status(
        status,
        trans_buffer_get(core, block, shard, allow_reserved, is_load, out),
    );
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_exist(
    core: *const CacheStoreTransBufferCore,
    block: *const CacheStoreBlockId,
    shard: usize,
    out: *mut bool,
    status: *mut CacheStoreFfiStatus,
) {
    set_status(status, trans_buffer_exist(core, block, shard, out));
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_data_at(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) -> *mut c_void {
    unsafe {
        match core_from_ptr(core) {
            Ok(core) => core.data_at(pos),
            Err(_) => ptr::null_mut(),
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_acquire(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) {
    unsafe {
        if let Ok(core) = core_from_ptr(core) {
            core.acquire(pos);
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_release(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) {
    unsafe {
        if let Ok(core) = core_from_ptr(core) {
            core.release(pos);
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_ready(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) -> bool {
    unsafe {
        core_from_ptr(core)
            .map(|core| core.ready(pos))
            .unwrap_or(false)
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_mark_ready(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) {
    unsafe {
        if let Ok(core) = core_from_ptr(core) {
            core.mark_ready(pos);
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_mark_not_ready(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) {
    unsafe {
        if let Ok(core) = core_from_ptr(core) {
            core.mark_not_ready(pos);
        }
    }
}

fn stable_hash(block: &CacheStoreBlockId, shard: usize) -> usize {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for byte in block.bytes {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    for byte in shard.to_le_bytes() {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash as usize
}

fn empty_status() -> CacheStoreFfiStatus {
    CacheStoreFfiStatus {
        code: STATUS_OK,
        message: [0; MESSAGE_CAPACITY],
    }
}

fn status_to_result(status: &CacheStoreFfiStatus, context: &str) -> Result<(), String> {
    if status.code == STATUS_OK {
        return Ok(());
    }
    let message = unsafe { CStr::from_ptr(status.message.as_ptr()) }
        .to_string_lossy()
        .into_owned();
    if message.is_empty() {
        Err(context.to_owned())
    } else {
        Err(message)
    }
}

fn shared_name(unique_id: &str) -> Result<CString, String> {
    CString::new(format!("{SHARED_BUFFER_PREFIX}{unique_id}"))
        .map_err(|_| "invalid shared trans buffer name".to_owned())
}

fn cleanup_shm_files_except_me(me: &str) {
    let Ok(entries) = fs::read_dir("/dev/shm") else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Some(name) = path.file_name().and_then(|name| name.to_str()) else {
            continue;
        };
        if !name.starts_with(SHARED_BUFFER_PREFIX) || name == me {
            continue;
        }
        let Ok(metadata) = entry.metadata() else {
            continue;
        };
        if !metadata.is_file() {
            continue;
        }
        let Ok(modified) = metadata.modified() else {
            continue;
        };
        let Ok(elapsed) = modified.elapsed() else {
            continue;
        };
        if elapsed.as_secs() > SHM_KEEP_SECONDS {
            let _ = fs::remove_file(path);
        }
    }
}

fn checked_add(lhs: usize, rhs: usize) -> Result<usize, String> {
    lhs.checked_add(rhs)
        .ok_or_else(|| "invalid shared trans buffer layout".to_owned())
}

fn checked_mul(lhs: usize, rhs: usize) -> Result<usize, String> {
    lhs.checked_mul(rhs)
        .ok_or_else(|| "invalid shared trans buffer layout".to_owned())
}

fn align_up(value: usize, align: usize) -> Result<usize, String> {
    if align == 0 {
        return Err("invalid shared trans buffer alignment".to_owned());
    }
    let remainder = value % align;
    if remainder == 0 {
        Ok(value)
    } else {
        checked_add(value, align - remainder)
    }
}

fn errno() -> c_int {
    unsafe { *__errno_location() }
}

#[cfg(test)]
mod tests {
    use std::ffi::c_void;
    use std::ptr;

    use super::{
        CacheStoreBlockId, CacheStoreFfiStatus, CacheStoreTransBufferCallbacks,
        CacheStoreTransBufferConfigView, CacheStoreTransBufferCore, CacheStoreTransBufferMetaNode,
        SharedLayout, INVALID_INDEX, STATUS_OK,
    };

    const NODE_SIZE: usize = 64;
    const TOTAL_SIZE: usize = NODE_SIZE * 1024;
    const RESERVED: usize = 8;

    struct TestBuffer {
        data: Vec<u8>,
    }

    unsafe extern "C" fn make_local_host_buffer(
        _ctx: *mut c_void,
        _device_id: i32,
        size: usize,
        _io_direct: bool,
        out_data: *mut *mut c_void,
        out_handle: *mut *mut c_void,
        status: *mut CacheStoreFfiStatus,
    ) {
        let mut buffer = Box::new(TestBuffer {
            data: vec![0u8; size],
        });
        let data = buffer.data.as_mut_ptr().cast::<c_void>();
        *out_data = data;
        *out_handle = Box::into_raw(buffer).cast::<c_void>();
        (*status).code = STATUS_OK;
        (*status).message = [0; 256];
    }

    unsafe extern "C" fn free_local_host_buffer(_ctx: *mut c_void, handle: *mut c_void) {
        drop(Box::from_raw(handle.cast::<TestBuffer>()));
    }

    unsafe extern "C" fn register_shared_host_buffer(
        _ctx: *mut c_void,
        _device_id: i32,
        data: *mut c_void,
        _size: usize,
        out_device: *mut *mut c_void,
        status: *mut CacheStoreFfiStatus,
    ) {
        *out_device = data;
        (*status).code = STATUS_OK;
        (*status).message = [0; 256];
    }

    unsafe extern "C" fn unregister_shared_host_buffer(_ctx: *mut c_void, _data: *mut c_void) {}
    unsafe extern "C" fn page_size(_ctx: *mut c_void) -> usize {
        4096
    }
    unsafe extern "C" fn lock_size(_ctx: *mut c_void) -> usize {
        8
    }
    unsafe extern "C" fn lock_align(_ctx: *mut c_void) -> usize {
        8
    }
    unsafe extern "C" fn lock_init(_ctx: *mut c_void, ptr: *mut c_void) -> bool {
        *ptr.cast::<usize>() = 0;
        true
    }
    unsafe extern "C" fn lock(_ctx: *mut c_void, _ptr: *mut c_void) {}
    unsafe extern "C" fn try_lock(_ctx: *mut c_void, _ptr: *mut c_void) -> bool {
        true
    }
    unsafe extern "C" fn unlock(_ctx: *mut c_void, _ptr: *mut c_void) {}

    fn callbacks() -> CacheStoreTransBufferCallbacks {
        CacheStoreTransBufferCallbacks {
            ctx: ptr::null_mut(),
            make_local_host_buffer: Some(make_local_host_buffer),
            free_local_host_buffer: Some(free_local_host_buffer),
            register_shared_host_buffer: Some(register_shared_host_buffer),
            unregister_shared_host_buffer: Some(unregister_shared_host_buffer),
            page_size: Some(page_size),
            shared_mutex_size: Some(lock_size),
            shared_mutex_align: Some(lock_align),
            shared_mutex_init: Some(lock_init),
            shared_mutex_lock: Some(lock),
            shared_mutex_try_lock: Some(try_lock),
            shared_mutex_unlock: Some(unlock),
            shared_spin_size: Some(lock_size),
            shared_spin_align: Some(lock_align),
            shared_spin_init: Some(lock_init),
            shared_spin_lock: Some(lock),
            shared_spin_try_lock: Some(try_lock),
            shared_spin_unlock: Some(unlock),
        }
    }

    fn local_config(bypass_hit_on_load: bool) -> CacheStoreTransBufferConfigView {
        CacheStoreTransBufferConfigView {
            share_buffer_enable: false,
            bypass_hit_on_load,
            io_direct: false,
            unique_id_ptr: ptr::null(),
            unique_id_len: 0,
            device_id: 0,
            node_size: NODE_SIZE,
            total_size: TOTAL_SIZE,
            reserved_number: RESERVED,
        }
    }

    fn local_config_with_capacity(
        total_size: usize,
        reserved_number: usize,
    ) -> CacheStoreTransBufferConfigView {
        CacheStoreTransBufferConfigView {
            share_buffer_enable: false,
            bypass_hit_on_load: false,
            io_direct: false,
            unique_id_ptr: ptr::null(),
            unique_id_len: 0,
            device_id: 0,
            node_size: NODE_SIZE,
            total_size,
            reserved_number,
        }
    }

    fn block(id: u8) -> CacheStoreBlockId {
        CacheStoreBlockId { bytes: [id; 16] }
    }

    #[test]
    fn get_allocates_and_tracks_existence() {
        let config = local_config(false);
        let callbacks = callbacks();
        let core = unsafe { CacheStoreTransBufferCore::new(&config, &callbacks) }.unwrap();

        let result = unsafe { core.get(block(1), 0, false, false) };
        assert!(result.owner);
        assert_ne!(result.index, INVALID_INDEX);
        assert!(unsafe { core.exist(&block(1), 0) });
        assert!(!unsafe { core.exist(&block(2), 0) });
    }

    #[test]
    fn repeated_get_transfers_ownership_after_release() {
        let config = local_config(false);
        let callbacks = callbacks();
        let core = unsafe { CacheStoreTransBufferCore::new(&config, &callbacks) }.unwrap();

        let first = unsafe { core.get(block(7), 1, false, false) };
        let second = unsafe { core.get(block(7), 1, false, false) };
        assert_eq!(first.index, second.index);
        assert!(!second.owner);

        unsafe {
            core.release(first.index);
            core.release(second.index);
        }
        let third = unsafe { core.get(block(7), 1, false, false) };
        assert_eq!(first.index, third.index);
        assert!(third.owner);
    }

    #[test]
    fn bypass_hit_on_load_marks_owner_hit_not_ready() {
        let config = local_config(true);
        let callbacks = callbacks();
        let core = unsafe { CacheStoreTransBufferCore::new(&config, &callbacks) }.unwrap();

        let first = unsafe { core.get(block(3), 2, false, false) };
        unsafe {
            core.mark_ready(first.index);
            core.release(first.index);
        }
        assert!(unsafe { core.ready(first.index) });

        let second = unsafe { core.get(block(3), 2, false, true) };
        assert_eq!(first.index, second.index);
        assert!(second.owner);
        assert!(!unsafe { core.ready(second.index) });
    }

    #[test]
    fn data_at_uses_node_stride() {
        let config = local_config(false);
        let callbacks = callbacks();
        let core = unsafe { CacheStoreTransBufferCore::new(&config, &callbacks) }.unwrap();

        let first = core.data_at(0) as usize;
        let second = core.data_at(1) as usize;
        assert_eq!(second - first, NODE_SIZE);
        assert!(core.data_at(TOTAL_SIZE / NODE_SIZE).is_null());
    }

    #[test]
    fn reserved_number_may_match_node_count_for_legacy_configs() {
        let config = local_config_with_capacity(NODE_SIZE * RESERVED, RESERVED);
        let callbacks = callbacks();
        assert!(unsafe { CacheStoreTransBufferCore::new(&config, &callbacks) }.is_ok());
    }

    #[test]
    fn shared_layout_places_data_on_page_boundary() {
        let callbacks = unsafe { super::Callbacks::from_view(&callbacks()) }.unwrap();
        let layout = unsafe { SharedLayout::new(128, NODE_SIZE, callbacks) }.unwrap();
        assert_eq!(layout.data_offset % 4096, 0);
        assert_eq!(layout.total_size, layout.data_offset + 128 * NODE_SIZE);
        assert!(layout.meta_offset > layout.buckets_offset);
    }

    #[test]
    fn meta_node_empty_matches_cxx_invalid_links() {
        let meta = CacheStoreTransBufferMetaNode::empty();
        assert_eq!(meta.reference, 0);
        assert_eq!(meta.hash, INVALID_INDEX);
        assert_eq!(meta.prev, INVALID_INDEX);
        assert_eq!(meta.next, INVALID_INDEX);
        assert!(!meta.ready);
    }
}
