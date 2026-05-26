/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#ifndef UNIFIEDCACHE_TRANS_ASCEND_RS_H
#define UNIFIEDCACHE_TRANS_ASCEND_RS_H

#include <cstddef>
#include <cstdint>

extern "C" {

using UcmAscendCallback = void (*)(void*);

int ucm_ascend_trans_set_device(int32_t deviceId);
int ucm_ascend_trans_create_stream(void** stream);
int ucm_ascend_trans_destroy_stream(void* stream);
int ucm_ascend_trans_subscribe_report(uint64_t threadId, void* stream);
int ucm_ascend_trans_unsubscribe_report(uint64_t threadId, void* stream);
int ucm_ascend_trans_process_report(int32_t timeout);
int ucm_ascend_trans_launch_callback(UcmAscendCallback callback, void* userData, void* stream);

int ucm_ascend_trans_device_to_host(const void* device, void* host, size_t size);
int ucm_ascend_trans_device_to_host_async(const void* device, void* host, size_t size,
                                          void* stream);
int ucm_ascend_trans_host_to_device(const void* host, void* device, size_t size);
int ucm_ascend_trans_host_to_device_async(const void* host, void* device, size_t size,
                                          void* stream);
int ucm_ascend_trans_synchronize_stream(void* stream);
int ucm_ascend_trans_stream_wait_event(void* stream, void* event);

int ucm_ascend_trans_malloc_device(void** device, size_t size);
int ucm_ascend_trans_free_device(void* device);
int ucm_ascend_trans_malloc_host(void** host, size_t size);
int ucm_ascend_trans_free_host(void* host);
int ucm_ascend_trans_host_register(void* host, size_t size, void** device);
int ucm_ascend_trans_host_register_v2(void* host, size_t size);
int ucm_ascend_trans_host_get_device_pointer(void* host, void** device);
int ucm_ascend_trans_host_unregister(void* host);
}

#endif
