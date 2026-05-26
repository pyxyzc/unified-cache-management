use std::ffi::c_void;

type AclError = i32;
type AclCallback = Option<extern "C" fn(*mut c_void)>;

const ACL_MEMCPY_HOST_TO_DEVICE: i32 = 1;
const ACL_MEMCPY_DEVICE_TO_HOST: i32 = 2;
const ACL_MEM_TYPE_HIGH_BAND_WIDTH: i32 = 0x1000;
const ACL_HOST_REGISTER_MAPPED: i32 = 0;
const ACL_HOST_REG_MAPPED: u32 = 0x2;
const ACL_HOST_REG_PINNED: u32 = 0x1000_0000;
const ACL_STREAM_FAST_LAUNCH: u32 = 0x1;
const ACL_STREAM_FAST_SYNC: u32 = 0x2;
const ACL_CALLBACK_NO_BLOCK: i32 = 0;

extern "C" {
    fn aclrtSetDevice(device_id: i32) -> AclError;
    fn aclrtCreateStreamWithConfig(stream: *mut *mut c_void, priority: u32, flag: u32) -> AclError;
    fn aclrtDestroyStream(stream: *mut c_void) -> AclError;
    fn aclrtSubscribeReport(thread_id: u64, stream: *mut c_void) -> AclError;
    fn aclrtUnSubscribeReport(thread_id: u64, stream: *mut c_void) -> AclError;
    fn aclrtProcessReport(timeout: i32) -> AclError;
    fn aclrtLaunchCallback(
        callback: AclCallback,
        user_data: *mut c_void,
        block_type: i32,
        stream: *mut c_void,
    ) -> AclError;
    fn aclrtMemcpy(
        dst: *mut c_void,
        dest_max: usize,
        src: *const c_void,
        count: usize,
        kind: i32,
    ) -> AclError;
    fn aclrtMemcpyAsync(
        dst: *mut c_void,
        dest_max: usize,
        src: *const c_void,
        count: usize,
        kind: i32,
        stream: *mut c_void,
    ) -> AclError;
    fn aclrtSynchronizeStream(stream: *mut c_void) -> AclError;
    fn aclrtStreamWaitEvent(stream: *mut c_void, event: *mut c_void) -> AclError;
    fn aclrtMalloc(dev_ptr: *mut *mut c_void, size: usize, policy: i32) -> AclError;
    fn aclrtFree(dev_ptr: *mut c_void) -> AclError;
    fn aclrtMallocHost(host_ptr: *mut *mut c_void, size: usize) -> AclError;
    fn aclrtFreeHost(host_ptr: *mut c_void) -> AclError;
    fn aclrtHostRegister(
        ptr: *mut c_void,
        size: u64,
        register_type: i32,
        dev_ptr: *mut *mut c_void,
    ) -> AclError;
    fn aclrtHostRegisterV2(ptr: *mut c_void, size: u64, flag: u32) -> AclError;
    fn aclrtHostGetDevicePointer(
        host_ptr: *mut c_void,
        device_ptr: *mut *mut c_void,
        flag: u32,
    ) -> AclError;
    fn aclrtHostUnregister(ptr: *mut c_void) -> AclError;
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_set_device(device_id: i32) -> AclError {
    unsafe { aclrtSetDevice(device_id) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_create_stream(stream: *mut *mut c_void) -> AclError {
    unsafe { aclrtCreateStreamWithConfig(stream, 0, ACL_STREAM_FAST_LAUNCH | ACL_STREAM_FAST_SYNC) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_destroy_stream(stream: *mut c_void) -> AclError {
    unsafe { aclrtDestroyStream(stream) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_subscribe_report(
    thread_id: u64,
    stream: *mut c_void,
) -> AclError {
    unsafe { aclrtSubscribeReport(thread_id, stream) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_unsubscribe_report(
    thread_id: u64,
    stream: *mut c_void,
) -> AclError {
    unsafe { aclrtUnSubscribeReport(thread_id, stream) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_process_report(timeout: i32) -> AclError {
    unsafe { aclrtProcessReport(timeout) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_launch_callback(
    callback: AclCallback,
    user_data: *mut c_void,
    stream: *mut c_void,
) -> AclError {
    unsafe { aclrtLaunchCallback(callback, user_data, ACL_CALLBACK_NO_BLOCK, stream) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_device_to_host(
    device: *const c_void,
    host: *mut c_void,
    size: usize,
) -> AclError {
    unsafe { aclrtMemcpy(host, size, device, size, ACL_MEMCPY_DEVICE_TO_HOST) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_device_to_host_async(
    device: *const c_void,
    host: *mut c_void,
    size: usize,
    stream: *mut c_void,
) -> AclError {
    unsafe { aclrtMemcpyAsync(host, size, device, size, ACL_MEMCPY_DEVICE_TO_HOST, stream) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_host_to_device(
    host: *const c_void,
    device: *mut c_void,
    size: usize,
) -> AclError {
    unsafe { aclrtMemcpy(device, size, host, size, ACL_MEMCPY_HOST_TO_DEVICE) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_host_to_device_async(
    host: *const c_void,
    device: *mut c_void,
    size: usize,
    stream: *mut c_void,
) -> AclError {
    unsafe { aclrtMemcpyAsync(device, size, host, size, ACL_MEMCPY_HOST_TO_DEVICE, stream) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_synchronize_stream(stream: *mut c_void) -> AclError {
    unsafe { aclrtSynchronizeStream(stream) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_stream_wait_event(
    stream: *mut c_void,
    event: *mut c_void,
) -> AclError {
    unsafe { aclrtStreamWaitEvent(stream, event) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_malloc_device(
    device: *mut *mut c_void,
    size: usize,
) -> AclError {
    unsafe { aclrtMalloc(device, size, ACL_MEM_TYPE_HIGH_BAND_WIDTH) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_free_device(device: *mut c_void) -> AclError {
    unsafe { aclrtFree(device) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_malloc_host(host: *mut *mut c_void, size: usize) -> AclError {
    unsafe { aclrtMallocHost(host, size) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_free_host(host: *mut c_void) -> AclError {
    unsafe { aclrtFreeHost(host) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_host_register(
    host: *mut c_void,
    size: usize,
    device: *mut *mut c_void,
) -> AclError {
    unsafe { aclrtHostRegister(host, size as u64, ACL_HOST_REGISTER_MAPPED, device) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_host_register_v2(host: *mut c_void, size: usize) -> AclError {
    unsafe { aclrtHostRegisterV2(host, size as u64, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_host_get_device_pointer(
    host: *mut c_void,
    device: *mut *mut c_void,
) -> AclError {
    unsafe { aclrtHostGetDevicePointer(host, device, 0) }
}

#[no_mangle]
pub extern "C" fn ucm_ascend_trans_host_unregister(host: *mut c_void) -> AclError {
    unsafe { aclrtHostUnregister(host) }
}
