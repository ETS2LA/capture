#pragma once

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "../../include/ets2la_capture/protocol.h"

namespace ets2la_capture {

class SecurityAttrs {
public:
    SecurityAttrs() {
        std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
        std::wstring sid = current_user_sid();
        if (!sid.empty()) {
            sddl += L"(A;;GA;;;" + sid + L")";
        }
        sddl += L"S:(ML;;NW;;;ME)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
            sa_.nLength = sizeof(sa_);
            sa_.lpSecurityDescriptor = sd_;
            sa_.bInheritHandle = FALSE;
            ok_ = true;
        }
    }
    ~SecurityAttrs() {
        if (sd_) {
            LocalFree(sd_);
        }
    }
    SecurityAttrs(const SecurityAttrs&) = delete;
    SecurityAttrs& operator=(const SecurityAttrs&) = delete;
    SECURITY_ATTRIBUTES* get() {
        return ok_ ? &sa_ : nullptr;
    }

private:
    static std::wstring current_user_sid() {
        std::wstring result;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return result;
        }
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        if (needed) {
            std::string buf(needed, '\0');
            if (GetTokenInformation(token, TokenUser, &buf[0], needed, &needed)) {
                auto* user = reinterpret_cast<TOKEN_USER*>(&buf[0]);
                LPWSTR str = nullptr;
                if (ConvertSidToStringSidW(user->User.Sid, &str)) {
                    result = str;
                    LocalFree(str);
                }
            }
        }
        CloseHandle(token);
        return result;
    }

    SECURITY_ATTRIBUTES sa_{};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    bool ok_ = false;
};

class SharedFrameWriter {
public:
    ~SharedFrameWriter() {
        shutdown();
    }

    const std::string& last_error() const {
        return lastError_;
    }

    bool init() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ctl_) {
            return true;
        }

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);

        bool existed = false;
        ctlMap_ = create_mapping(kControlName, kControlBytes, &existed);
        if (!ctlMap_) {
            return fail("CreateFileMapping(control) failed");
        }
        ctl_ = static_cast<ControlBlock*>(MapViewOfFile(ctlMap_, FILE_MAP_WRITE, 0, 0, kControlBytes));
        if (!ctl_) {
            return fail("MapViewOfFile(control) failed");
        }

        for (int i = 0; i < 2; ++i) {
            events_[i] = create_event(kEventNames[i]);
            if (!events_[i]) {
                return fail("CreateEvent failed");
            }
        }

        if (!existed || ctl_->magic != kControlMagic) {
            std::memset(ctl_, 0, sizeof(ControlBlock));
        }
        ctl_->slot_count = kSlotCount;
        ctl_->max_readers = kMaxReaders;
        ctl_->qpc_frequency = uint64_t(freq.QuadPart);
        ctl_->writer_pid = GetCurrentProcessId();
        ctl_->capturing.store(0, std::memory_order_relaxed);
        ctl_->magic = kControlMagic;

        const uint64_t latest = ctl_->latest_frame.load(std::memory_order_relaxed);
        ResetEvent(events_[(latest + 1) & 1]);
        if (latest > 0) {
            SetEvent(events_[latest & 1]);
        } else {
            ResetEvent(events_[latest & 1]);
        }
    
        nextGeneration_ = ctl_->generation.load(std::memory_order_relaxed) + 1;
        return true;
    }

    bool ensure_geometry(uint32_t width, uint32_t height, uint32_t dxgiFormat, uint32_t bytesPerPixel) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ensure_geometry_locked(width, height, dxgiFormat, bytesPerPixel);
    }

    bool publish(const uint8_t* src, uint32_t srcPitch, uint64_t timestampQpc) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ctl_ || !data_) {
            return false;
        }

        const uint64_t index = ctl_->latest_frame.load(std::memory_order_relaxed) + 1;
        uint8_t* slotBase = data_ + kDataHeaderBytes + (index % kSlotCount) * slotStride_;
        auto* slot = reinterpret_cast<SlotHeader*>(slotBase);
        uint8_t* dst = slotBase + kSlotHeaderBytes;

        const uint64_t s = slot->seq.load(std::memory_order_relaxed);
        slot->seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        slot->frame_index = index;
        slot->timestamp_qpc = timestampQpc;
        if (srcPitch == rowBytes_) {
            std::memcpy(dst, src, size_t(rowBytes_) * height_);
        } else {
            for (uint32_t y = 0; y < height_; ++y) {
                std::memcpy(dst + size_t(y) * rowBytes_, src + size_t(y) * srcPitch, rowBytes_);
            }
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        slot->publish_qpc = uint64_t(now.QuadPart);

        slot->seq.store(s + 2, std::memory_order_release);

        ResetEvent(events_[(index + 1) & 1]);
        if (generationDirty_) {
            ctl_->generation.store(generation_, std::memory_order_release);
            generationDirty_ = false;
        }
        ctl_->latest_frame.store(index, std::memory_order_release);
        ctl_->last_publish_qpc.store(uint64_t(now.QuadPart), std::memory_order_relaxed);
        ctl_->frames_published.fetch_add(1, std::memory_order_relaxed);
        SetEvent(events_[index & 1]);
        return true;
    }

    bool has_active_readers() const {
        if (!ctl_) {
            return false;
        }
        const uint64_t now = GetTickCount64();
        for (uint32_t i = 0; i < kMaxReaders; ++i) {
            const auto& r = ctl_->readers[i];
            if (r.id.load(std::memory_order_relaxed) != 0 && now - r.heartbeat_ms.load(std::memory_order_relaxed) < kReaderTimeoutMs) {
                return true;
            }
        }
        return false;
    }

    void set_capturing(bool on) {
        if (ctl_) {
            ctl_->capturing.store(on ? 1u : 0u, std::memory_order_relaxed);
        }
    }
    void note_drop() {
        if (ctl_) {
            ctl_->frames_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ctl_) {
            ctl_->capturing.store(0, std::memory_order_relaxed);
        }
        close_data();
        if (ctl_) {
            UnmapViewOfFile(ctl_);
            ctl_ = nullptr;
        }
        if (ctlMap_) {
            CloseHandle(ctlMap_);
            ctlMap_ = nullptr;
        }
        for (auto& e : events_) {
            if (e) {
                CloseHandle(e);
                e = nullptr; 
            }
        }
    }

private:
    bool fail(const char* msg) { lastError_ = msg; return false; }

    HANDLE create_mapping(const wchar_t* name, uint64_t size, bool* existed) {
        const DWORD hi = DWORD(size >> 32), lo = DWORD(size & 0xFFFFFFFFu);
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, sa_.get(), PAGE_READWRITE, hi, lo, name);
        if (!h && sa_.get()) {
            h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, name);
        }
        if (existed) {
            *existed = (h != nullptr) && GetLastError() == ERROR_ALREADY_EXISTS;
        }
        return h;
    }

    HANDLE create_event(const wchar_t* name) {
        HANDLE h = CreateEventW(sa_.get(), TRUE, FALSE, name);
        if (!h && sa_.get()) {
            h = CreateEventW(nullptr, TRUE, FALSE, name);
        }
        return h;
    }

    void close_data() {
        if (data_) {
            UnmapViewOfFile(data_);
            data_ = nullptr;
        }
        if (dataMap_) {
            CloseHandle(dataMap_);
            dataMap_ = nullptr;
        }
    }

    bool ensure_geometry_locked(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
        if (!ctl_) {
            return false;
        }
        if (data_ && w == width_ && h == height_ && fmt == format_) {
            return true;
        }
        if (w == 0 || h == 0 || bpp == 0) {
            return fail("invalid frame geometry");
        }

        const uint64_t rowBytes = uint64_t(w) * bpp;
        const uint64_t payload = rowBytes * h;
        const uint64_t stride = slot_stride_for(payload);
        const uint64_t total = kDataHeaderBytes + stride * kSlotCount;

        uint32_t gen = nextGeneration_;
        HANDLE map = nullptr;
        for (int attempt = 0; attempt < 64; ++attempt, ++gen) {
            wchar_t name[96];
            make_data_name(name, 96, gen);
            bool existed = false;
            map = create_mapping(name, total, &existed);
            if (!map) {
                return fail("CreateFileMapping(data) failed");
            }
            if (!existed) break;
            CloseHandle(map);
            map = nullptr;
        }
        if (!map) {
            return fail("no free data mapping name");
        }

        auto* base = static_cast<uint8_t*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0));
        if (!base) {
            CloseHandle(map);
            return fail("MapViewOfFile(data) failed");
        }

        auto* hdr = reinterpret_cast<DataHeader*>(base);
        hdr->generation = gen;
        hdr->slot_count = kSlotCount;
        hdr->slot_stride = stride;
        hdr->slots_offset = kDataHeaderBytes;
        hdr->width = w;
        hdr->height = h;
        hdr->stride = uint32_t(rowBytes);
        hdr->format = fmt;
        hdr->bytes_per_pixel = bpp;
        hdr->total_size = total;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->magic = kDataMagic;

        close_data();
        dataMap_ = map;
        data_ = base;
        width_ = w; height_ = h; format_ = fmt; bpp_ = bpp;
        rowBytes_ = uint32_t(rowBytes);
        slotStride_ = stride;
        generation_ = gen;
        nextGeneration_ = gen + 1;
        generationDirty_ = true;
        return true;
    }

    std::mutex mutex_;
    SecurityAttrs sa_;
    std::string lastError_;

    HANDLE ctlMap_ = nullptr;
    ControlBlock* ctl_ = nullptr;
    HANDLE events_[2] = {nullptr, nullptr};

    HANDLE dataMap_ = nullptr;
    uint8_t* data_ = nullptr;
    uint32_t width_ = 0, height_ = 0, format_ = 0, bpp_ = 0, rowBytes_ = 0;
    uint64_t slotStride_ = 0;
    uint32_t generation_ = 0, nextGeneration_ = 1;
    bool generationDirty_ = false;
};

}