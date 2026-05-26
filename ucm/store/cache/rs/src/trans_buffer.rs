use std::ffi::c_void;

use super::{
    require_const_ptr, require_out_ptr, set_status, CacheStoreBlockId, CacheStoreFfiStatus,
};

const INVALID_INDEX: usize = usize::MAX;

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

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CacheStoreTransBufferStrategyView {
    ctx: *mut c_void,
    bucket_of: Option<unsafe extern "C" fn(*mut c_void, *const CacheStoreBlockId, usize) -> usize>,
    bucket_lock: Option<unsafe extern "C" fn(*mut c_void, usize)>,
    bucket_try_lock: Option<unsafe extern "C" fn(*mut c_void, usize) -> bool>,
    bucket_unlock: Option<unsafe extern "C" fn(*mut c_void, usize)>,
    node_lock: Option<unsafe extern "C" fn(*mut c_void, usize)>,
    node_unlock: Option<unsafe extern "C" fn(*mut c_void, usize)>,
    first_at: Option<unsafe extern "C" fn(*mut c_void, usize) -> *mut usize>,
    fetch_node: Option<unsafe extern "C" fn(*mut c_void, bool) -> usize>,
    meta_at: Option<unsafe extern "C" fn(*mut c_void, usize) -> *mut CacheStoreTransBufferMetaNode>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CacheStoreTransBufferGetResult {
    index: usize,
    owner: bool,
}

pub struct CacheStoreTransBufferCore {
    strategy: Strategy,
    bypass_hit_on_load: bool,
}

#[derive(Clone, Copy)]
struct Strategy {
    view: CacheStoreTransBufferStrategyView,
}

impl Strategy {
    fn new(view: CacheStoreTransBufferStrategyView) -> Result<Self, String> {
        if view.ctx.is_null() {
            return Err("invalid trans buffer strategy context".to_owned());
        }
        if view.bucket_of.is_none()
            || view.bucket_lock.is_none()
            || view.bucket_try_lock.is_none()
            || view.bucket_unlock.is_none()
            || view.node_lock.is_none()
            || view.node_unlock.is_none()
            || view.first_at.is_none()
            || view.fetch_node.is_none()
            || view.meta_at.is_none()
        {
            return Err("invalid trans buffer strategy callbacks".to_owned());
        }
        Ok(Self { view })
    }

    unsafe fn bucket_of(&self, block: *const CacheStoreBlockId, shard: usize) -> usize {
        (self.view.bucket_of.unwrap_unchecked())(self.view.ctx, block, shard)
    }

    unsafe fn bucket_lock(&self, i_bucket: usize) {
        (self.view.bucket_lock.unwrap_unchecked())(self.view.ctx, i_bucket);
    }

    unsafe fn bucket_try_lock(&self, i_bucket: usize) -> bool {
        (self.view.bucket_try_lock.unwrap_unchecked())(self.view.ctx, i_bucket)
    }

    unsafe fn bucket_unlock(&self, i_bucket: usize) {
        (self.view.bucket_unlock.unwrap_unchecked())(self.view.ctx, i_bucket);
    }

    unsafe fn node_lock(&self, i_node: usize) {
        (self.view.node_lock.unwrap_unchecked())(self.view.ctx, i_node);
    }

    unsafe fn node_unlock(&self, i_node: usize) {
        (self.view.node_unlock.unwrap_unchecked())(self.view.ctx, i_node);
    }

    unsafe fn first_at(&self, i_bucket: usize) -> *mut usize {
        (self.view.first_at.unwrap_unchecked())(self.view.ctx, i_bucket)
    }

    unsafe fn fetch_node(&self, allow_reserved: bool) -> usize {
        (self.view.fetch_node.unwrap_unchecked())(self.view.ctx, allow_reserved)
    }

    unsafe fn meta_at(&self, i_node: usize) -> *mut CacheStoreTransBufferMetaNode {
        (self.view.meta_at.unwrap_unchecked())(self.view.ctx, i_node)
    }
}

impl CacheStoreTransBufferCore {
    fn new(
        strategy: CacheStoreTransBufferStrategyView,
        bypass_hit_on_load: bool,
    ) -> Result<Self, String> {
        Ok(Self {
            strategy: Strategy::new(strategy)?,
            bypass_hit_on_load,
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
        self.strategy.node_lock(pos);
        (*self.strategy.meta_at(pos)).reference += 1;
        self.strategy.node_unlock(pos);
    }

    unsafe fn release(&self, pos: usize) {
        self.strategy.node_lock(pos);
        (*self.strategy.meta_at(pos)).reference -= 1;
        self.strategy.node_unlock(pos);
    }

    unsafe fn ready(&self, pos: usize) -> bool {
        self.strategy.node_lock(pos);
        let ready = (*self.strategy.meta_at(pos)).ready;
        self.strategy.node_unlock(pos);
        ready
    }

    unsafe fn mark_ready(&self, pos: usize) {
        self.strategy.node_lock(pos);
        (*self.strategy.meta_at(pos)).ready = true;
        self.strategy.node_unlock(pos);
    }

    unsafe fn mark_not_ready(&self, pos: usize) {
        self.strategy.node_lock(pos);
        (*self.strategy.meta_at(pos)).ready = false;
        self.strategy.node_unlock(pos);
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
    unsafe {
        let core = core_from_ptr(core)?;
        *out = core.get(*block, shard, allow_reserved, is_load);
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
    unsafe {
        let core = core_from_ptr(core)?;
        *out = core.exist(&*block, shard);
    }
    Ok(())
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_new(
    strategy: *const CacheStoreTransBufferStrategyView,
    bypass_hit_on_load: bool,
    status: *mut CacheStoreFfiStatus,
) -> *mut CacheStoreTransBufferCore {
    if strategy.is_null() {
        set_status(status, Err("invalid trans buffer strategy".to_owned()));
        return std::ptr::null_mut();
    }
    let strategy = unsafe { *strategy };
    match CacheStoreTransBufferCore::new(strategy, bypass_hit_on_load) {
        Ok(core) => {
            set_status(status, Ok(()));
            Box::into_raw(Box::new(core))
        }
        Err(message) => {
            set_status(status, Err(message));
            std::ptr::null_mut()
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
pub extern "C" fn ucm_cache_store_trans_buffer_acquire(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) {
    if let Ok(core) = unsafe { core_from_ptr(core) } {
        unsafe {
            core.acquire(pos);
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_release(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) {
    if let Ok(core) = unsafe { core_from_ptr(core) } {
        unsafe {
            core.release(pos);
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_ready(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) -> bool {
    match unsafe { core_from_ptr(core) } {
        Ok(core) => unsafe { core.ready(pos) },
        Err(_) => false,
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_mark_ready(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) {
    if let Ok(core) = unsafe { core_from_ptr(core) } {
        unsafe {
            core.mark_ready(pos);
        }
    }
}

#[no_mangle]
pub extern "C" fn ucm_cache_store_trans_buffer_mark_not_ready(
    core: *const CacheStoreTransBufferCore,
    pos: usize,
) {
    if let Ok(core) = unsafe { core_from_ptr(core) } {
        unsafe {
            core.mark_not_ready(pos);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        CacheStoreTransBufferCore, CacheStoreTransBufferMetaNode,
        CacheStoreTransBufferStrategyView, INVALID_INDEX,
    };
    use crate::CacheStoreBlockId;
    use std::ffi::c_void;

    struct FakeStrategy {
        buckets: Vec<usize>,
        free_head: usize,
        reserved: usize,
        meta: Vec<CacheStoreTransBufferMetaNode>,
    }

    impl FakeStrategy {
        fn new(bucket_count: usize, n_node: usize, reserved: usize) -> Self {
            Self {
                buckets: vec![INVALID_INDEX; bucket_count],
                free_head: 0,
                reserved,
                meta: vec![empty_meta(); n_node],
            }
        }

        fn view(&mut self) -> CacheStoreTransBufferStrategyView {
            CacheStoreTransBufferStrategyView {
                ctx: self as *mut Self as *mut c_void,
                bucket_of: Some(bucket_of),
                bucket_lock: Some(noop_lock),
                bucket_try_lock: Some(try_lock),
                bucket_unlock: Some(noop_lock),
                node_lock: Some(noop_lock),
                node_unlock: Some(noop_lock),
                first_at: Some(first_at),
                fetch_node: Some(fetch_node),
                meta_at: Some(meta_at),
            }
        }
    }

    fn block(id: u8) -> CacheStoreBlockId {
        CacheStoreBlockId { bytes: [id; 16] }
    }

    fn empty_meta() -> CacheStoreTransBufferMetaNode {
        CacheStoreTransBufferMetaNode {
            block: block(0),
            shard: 0,
            reference: 0,
            hash: INVALID_INDEX,
            prev: INVALID_INDEX,
            next: INVALID_INDEX,
            ready: false,
        }
    }

    unsafe extern "C" fn bucket_of(
        ctx: *mut c_void,
        block: *const CacheStoreBlockId,
        shard: usize,
    ) -> usize {
        let fake = &*(ctx as *mut FakeStrategy);
        ((*block).bytes[0] as usize + shard) % fake.buckets.len()
    }

    unsafe extern "C" fn noop_lock(_ctx: *mut c_void, _index: usize) {}

    unsafe extern "C" fn try_lock(_ctx: *mut c_void, _index: usize) -> bool {
        true
    }

    unsafe extern "C" fn first_at(ctx: *mut c_void, bucket: usize) -> *mut usize {
        let fake = &mut *(ctx as *mut FakeStrategy);
        &mut fake.buckets[bucket]
    }

    unsafe extern "C" fn fetch_node(ctx: *mut c_void, allow_reserved: bool) -> usize {
        let fake = &mut *(ctx as *mut FakeStrategy);
        let limit = fake.meta.len() - if allow_reserved { 0 } else { fake.reserved };
        if fake.free_head >= limit {
            fake.free_head = 0;
        }
        let node = fake.free_head;
        fake.free_head += 1;
        node
    }

    unsafe extern "C" fn meta_at(
        ctx: *mut c_void,
        node: usize,
    ) -> *mut CacheStoreTransBufferMetaNode {
        let fake = &mut *(ctx as *mut FakeStrategy);
        &mut fake.meta[node]
    }

    #[test]
    fn get_first_node_and_share_ready_state() {
        let mut fake = FakeStrategy::new(8, 4, 0);
        let core = CacheStoreTransBufferCore::new(fake.view(), false).unwrap();

        let first = unsafe { core.get(block(1), 0, false, false) };
        assert_eq!(first.index, 0);
        assert!(first.owner);
        assert!(!unsafe { core.ready(first.index) });

        let second = unsafe { core.get(block(1), 0, false, false) };
        assert_eq!(second.index, first.index);
        assert!(!second.owner);
        assert_eq!(fake.meta[first.index].reference, 2);

        unsafe {
            core.mark_ready(first.index);
        }
        assert!(unsafe { core.ready(second.index) });
    }

    #[test]
    fn backend_only_load_reuses_idle_ready_entry() {
        let mut fake = FakeStrategy::new(8, 4, 0);
        let core = CacheStoreTransBufferCore::new(fake.view(), true).unwrap();

        let first = unsafe { core.get(block(2), 0, false, false) };
        unsafe {
            core.mark_ready(first.index);
            core.release(first.index);
        }

        let second = unsafe { core.get(block(2), 0, true, true) };
        assert_eq!(second.index, first.index);
        assert!(second.owner);
        assert!(!unsafe { core.ready(second.index) });
    }

    #[test]
    fn backend_only_load_coalesces_in_flight_entry() {
        let mut fake = FakeStrategy::new(8, 4, 0);
        let core = CacheStoreTransBufferCore::new(fake.view(), true).unwrap();

        let owner = unsafe { core.get(block(3), 0, true, true) };
        let waiter = unsafe { core.get(block(3), 0, true, true) };

        assert_eq!(owner.index, waiter.index);
        assert!(owner.owner);
        assert!(!waiter.owner);
    }

    #[test]
    fn reserved_nodes_are_used_only_when_allowed() {
        let mut fake = FakeStrategy::new(8, 17, 16);
        let core = CacheStoreTransBufferCore::new(fake.view(), false).unwrap();

        let block1 = block(4);
        let block2 = block(5);
        let first = unsafe { core.get(block1, 0, false, false) };
        unsafe {
            core.release(first.index);
        }
        let second = unsafe { core.get(block2, 0, false, false) };
        assert_eq!(second.index, first.index);
        unsafe {
            core.release(second.index);
        }

        let reserved = unsafe { core.get(block1, 0, true, false) };
        let existing = unsafe { core.get(block2, 0, true, false) };
        assert_ne!(reserved.index, existing.index);
    }

    #[test]
    fn move_to_new_bucket_removes_old_chain_links() {
        let mut fake = FakeStrategy::new(8, 2, 0);
        let core = CacheStoreTransBufferCore::new(fake.view(), false).unwrap();

        let first = unsafe { core.get(block(8), 0, false, false) };
        unsafe {
            core.release(first.index);
        }
        let second = unsafe { core.get(block(16), 0, false, false) };
        unsafe {
            core.release(second.index);
        }
        assert_eq!(fake.buckets[0], second.index);
        assert_eq!(fake.meta[second.index].next, first.index);

        let moved = unsafe { core.get(block(1), 0, false, false) };
        assert_eq!(moved.index, first.index);
        assert_eq!(fake.buckets[0], second.index);
        assert_eq!(fake.meta[second.index].next, INVALID_INDEX);
        assert_eq!(fake.buckets[1], first.index);
        assert!(unsafe { core.exist(&block(1), 0) });
        assert!(!unsafe { core.exist(&block(8), 0) });
    }
}
