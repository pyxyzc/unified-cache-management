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
#include "ascend_stream.h"
#include "ascend_rs.h"

namespace UC::Trans {

AscendStream::~AscendStream()
{
    if (cbThread_.joinable()) {
        auto tid = cbThread_.native_handle();
        (void)ucm_ascend_trans_unsubscribe_report(static_cast<uint64_t>(tid), stream_);
        stop_ = true;
        cbThread_.join();
    }
    if (stream_) {
        (void)ucm_ascend_trans_destroy_stream(stream_);
        stream_ = nullptr;
    }
}

Status AscendStream::Setup()
{
    auto ret = ucm_ascend_trans_create_stream(reinterpret_cast<void**>(&stream_));
    if (ret != ACL_SUCCESS) [[unlikely]] { return Status{ret, std::to_string(ret)}; }
    cbThread_ = std::thread([this] {
        while (!this->stop_) { (void)ucm_ascend_trans_process_report(10); }
    });
    auto tid = cbThread_.native_handle();
    ret = ucm_ascend_trans_subscribe_report(static_cast<uint64_t>(tid), stream_);
    if (ret != ACL_SUCCESS) [[unlikely]] { return Status{ret, std::to_string(ret)}; }
    return Status::OK();
}

Status AscendStream::DeviceToHost(void* device, void* host, size_t size)
{
    auto ret = ucm_ascend_trans_device_to_host(device, host, size);
    if (ret == ACL_SUCCESS) { return Status::OK(); }
    return Status{ret, std::to_string(ret)};
}

Status AscendStream::DeviceToHost(void* device[], void* host[], size_t size, size_t number)
{
    auto s = DeviceToHostAsync(device, host, size, number);
    if (s.Failure()) [[unlikely]] { return s; }
    return Synchronized();
}

Status AscendStream::DeviceToHost(void* device[], void* host, size_t size, size_t number)
{
    auto s = DeviceToHostAsync(device, host, size, number);
    if (s.Failure()) [[unlikely]] { return s; }
    return Synchronized();
}

Status AscendStream::DeviceToHostAsync(void* device, void* host, size_t size)
{
    auto ret = ucm_ascend_trans_device_to_host_async(device, host, size, stream_);
    if (ret == ACL_SUCCESS) { return Status::OK(); }
    return Status{ret, std::to_string(ret)};
}

Status AscendStream::DeviceToHostAsync(void* device[], void* host[], size_t size, size_t number)
{
    for (size_t i = 0; i < number; i++) {
        auto s = DeviceToHostAsync(device[i], host[i], size);
        if (s.Failure()) [[unlikely]] { return s; }
    }
    return Status::OK();
}

Status AscendStream::DeviceToHostAsync(void* device[], void* host, size_t size, size_t number)
{
    for (size_t i = 0; i < number; i++) {
        auto pHost = (void*)(((int8_t*)host) + size * i);
        auto s = DeviceToHostAsync(device[i], pHost, size);
        if (s.Failure()) [[unlikely]] { return s; }
    }
    return Status::OK();
}

Status AscendStream::HostToDevice(void* host, void* device, size_t size)
{
    auto ret = ucm_ascend_trans_host_to_device(host, device, size);
    if (ret == ACL_SUCCESS) { return Status::OK(); }
    return Status{ret, std::to_string(ret)};
}

Status AscendStream::HostToDevice(void* host[], void* device[], size_t size, size_t number)
{
    auto s = HostToDeviceAsync(host, device, size, number);
    if (s.Failure()) [[unlikely]] { return s; }
    return Synchronized();
}

Status AscendStream::HostToDevice(void* host, void* device[], size_t size, size_t number)
{
    auto s = HostToDeviceAsync(host, device, size, number);
    if (s.Failure()) [[unlikely]] { return s; }
    return Synchronized();
}

Status AscendStream::HostToDeviceAsync(void* host, void* device, size_t size)
{
    auto ret = ucm_ascend_trans_host_to_device_async(host, device, size, stream_);
    if (ret == ACL_SUCCESS) { return Status::OK(); }
    return Status{ret, std::to_string(ret)};
}

Status AscendStream::HostToDeviceAsync(void* host[], void* device[], size_t size, size_t number)
{
    for (size_t i = 0; i < number; i++) {
        auto s = HostToDeviceAsync(host[i], device[i], size);
        if (s.Failure()) [[unlikely]] { return s; }
    }
    return Status::OK();
}

Status AscendStream::HostToDeviceAsync(void* host, void* device[], size_t size, size_t number)
{
    for (size_t i = 0; i < number; i++) {
        auto pHost = (void*)(((int8_t*)host) + size * i);
        auto s = HostToDeviceAsync(pHost, device[i], size);
        if (s.Failure()) [[unlikely]] { return s; }
    }
    return Status::OK();
}

using Closure = std::function<void(bool)>;

static void Trampoline(void* data)
{
    auto c = static_cast<Closure*>(data);
    (*c)(true);
    delete c;
}

Status Trans::AscendStream::AppendCallback(std::function<void(bool)> cb)
{
    auto c = new (std::nothrow) Closure{std::move(cb)};
    if (!c) [[unlikely]] { return Status::Error("out of memory for appending callback"); }
    auto ret = ucm_ascend_trans_launch_callback(Trampoline, (void*)c, stream_);
    if (ret != ACL_SUCCESS) [[unlikely]] {
        delete c;
        return Status{ret, std::to_string(ret)};
    }
    return Status::OK();
}

Status AscendStream::Synchronized()
{
    auto ret = ucm_ascend_trans_synchronize_stream(stream_);
    if (ret == ACL_SUCCESS) { return Status::OK(); }
    return Status{ret, std::to_string(ret)};
}

Status AscendStream::WaitEvent(void* event)
{
    if (event == nullptr) { return Status::OK(); }
    auto ret = ucm_ascend_trans_stream_wait_event(stream_, event);
    if (ret != ACL_SUCCESS) [[unlikely]] { return Status{ret, std::to_string(ret)}; }
    return Status::OK();
}

}  // namespace UC::Trans
