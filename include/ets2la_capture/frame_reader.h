#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

namespace ets2la_capture {

inline uint64_t qpc_now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return uint64_t(q.QuadPart);
}

struct FrameInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    uint32_t bytes_per_pixel = 0;
    uint64_t frame_index = 0;
    uint64_t timestamp_qpc = 0;
    uint64_t publish_qpc = 0;

    PixelLayout layout() const { return pixel_layout(format); }
    size_t size_bytes() const { return size_t(stride) * height; }
};

// Owning copy of a frame. Reuse one instance across calls to avoid allocations.
struct Frame : FrameInfo {
    std::vector<uint8_t> data;
};

// Non-owning view straight into shared memory. Keeps the mapping alive, but the
// writer may overwrite the slot after kSlotCount-1 newer frames: call valid()
// after you are done reading and discard the result if it returns false.
class FrameView : public FrameInfo {
public:
    const uint8_t* data = nullptr;
    bool valid() const {
        if (!slot_) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        return slot_->seq.load(std::memory_order_relaxed) == seq_;
    }

private:
    friend class FrameReader;
    std::shared_ptr<void> keepalive_;
    const SlotHeader* slot_ = nullptr;
    uint64_t seq_ = 0;
};

struct ReaderOptions {
    uint32_t spin_us = 0;
};

struct CaptureStats {
    uint64_t latest_frame = 0;
    uint64_t frames_published = 0;
    uint64_t frames_dropped_by_capture = 0;
    uint32_t generation = 0;
    bool capturing = false;
    uint32_t writer_pid = 0;
};

class FrameReader {
public:
    explicit FrameReader(ReaderOptions options = {}) : options_(options) {
        connect();
    }
    ~FrameReader() {
        disconnect();
    }
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;

    bool ok() const {
        return ctl_ != nullptr;
    }
    const std::string& last_error() const {
        return lastError_;
    }

    // Blocks until a frame newer than the previous one returned by this reader
    // exists (or timeout) and copies it into `out`.
    bool get_frame(Frame& out, uint32_t timeout_ms = 1000) {
        return acquire(&out, nullptr, true, timeout_ms);
    }

    // Non-blocking: newest frame available right now, copied into `out`.
    bool get_latest_frame(Frame& out) {
        return acquire(&out, nullptr, false, 0);
    }

    // Blocks until a frame newer than the previous one returned by this reader
    // exists (or timeout) and returns a view into shared memory. The caller must
    // call valid() on the returned view after it is done reading to ensure that
    // the writer did not overwrite the slot in the meantime.
    bool get_frame_view(FrameView& out, uint32_t timeout_ms = 1000) {
        return acquire(nullptr, &out, true, timeout_ms);
    }

    // Non-blocking: newest frame available right now, returns a view into shared
    // memory. The caller must call valid() on the returned view after it is done
    // reading to ensure that the writer did not overwrite the slot in the meantime.
    bool get_latest_frame_view(FrameView& out) {
        return acquire(nullptr, &out, false, 0);
    }

    double ticks_to_ms(uint64_t ticks) const {
        return freq_ ? double(ticks) * 1000.0 / double(freq_) : 0.0;
    }
    // Time since the game presented this frame, in milliseconds.
    double age_ms(const FrameInfo& f) const {
        return ticks_to_ms(qpc_now() - f.timestamp_qpc);
    }

    CaptureStats stats() const {
        CaptureStats s;
        if (!ctl_) {
            return s;
        }
        s.latest_frame = ctl_->latest_frame.load(std::memory_order_relaxed);
        s.frames_published = ctl_->frames_published.load(std::memory_order_relaxed);
        s.frames_dropped_by_capture = ctl_->frames_dropped.load(std::memory_order_relaxed);
        s.generation = ctl_->generation.load(std::memory_order_relaxed);
        s.capturing = ctl_->capturing.load(std::memory_order_relaxed) != 0;
        s.writer_pid = ctl_->writer_pid;
        return s;
    }

    bool connect() {
        disconnect();
        ctlMap_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kControlName);
        if (!ctlMap_) {
            return fail("capture not running (OpenFileMapping failed). Is ets2la_capture.dll loaded?");
        }
        ctl_ = static_cast<ControlBlock*>(MapViewOfFile(ctlMap_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kControlBytes));
        if (!ctl_) {
            return fail("MapViewOfFile(control) failed");
        }
        if (ctl_->magic != kControlMagic) {
            disconnect();
            return fail("capture DLL not ready");
        }
        for (int i = 0; i < 2; ++i) {
            events_[i] = OpenEventW(SYNCHRONIZE, FALSE, kEventNames[i]);
            if (!events_[i]) {
                disconnect();
                return fail("OpenEvent failed");
            }
        }
        freq_ = ctl_->qpc_frequency;
        register_reader();
        last_index_ = ctl_->latest_frame.load(std::memory_order_acquire);
        lastError_.clear();
        return true;
    }

private:
    struct Mapping {
        HANDLE handle = nullptr;
        void* base = nullptr;
        const DataHeader* hdr = nullptr;
        ~Mapping() {
            if (base) {
                UnmapViewOfFile(base);
            }
            if (handle) {
                CloseHandle(handle);
            }
        }
    };

    bool fail(const char* msg) {
        lastError_ = msg;
        return false;
    }

    void disconnect() {
        unregister_reader();
        map_.reset();
        mapGen_ = 0;
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

    void register_reader() {
        static std::atomic<uint32_t> counter{0};
        myId_ = (uint64_t(GetCurrentProcessId()) << 32) | (counter.fetch_add(1) + 1);
        const uint64_t now = GetTickCount64();
        const uint32_t start = GetCurrentProcessId() % kMaxReaders;
        entry_ = &ctl_->readers[start];
        for (uint32_t i = 0; i < kMaxReaders; ++i) {
            ReaderEntry* e = &ctl_->readers[(start + i) % kMaxReaders];
            const uint64_t id = e->id.load(std::memory_order_relaxed);
            if (id == 0 || now - e->heartbeat_ms.load(std::memory_order_relaxed) > kReaderTimeoutMs * 2) {
                entry_ = e;
                break;
            }
        }
        heartbeat();
    }
    void heartbeat() {
        if (!entry_) return;
        entry_->id.store(myId_, std::memory_order_relaxed);
        entry_->heartbeat_ms.store(GetTickCount64(), std::memory_order_release);
    }
    void unregister_reader() {
        if (entry_ && ctl_ && entry_->id.load(std::memory_order_relaxed) == myId_) {
            entry_->heartbeat_ms.store(0, std::memory_order_relaxed);
            entry_->id.store(0, std::memory_order_release);
        }
        entry_ = nullptr;
    }

    bool ensure_mapping(uint32_t generation) {
        if (map_ && mapGen_ == generation) {
            return true;
        }
        wchar_t name[96];
        make_data_name(name, 96, generation);
        auto m = std::make_shared<Mapping>();
        m->handle = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!m->handle) {
            return fail("data mapping not found");
        }
        m->base = MapViewOfFile(m->handle, FILE_MAP_READ, 0, 0, 0);
        if (!m->base) {
            return fail("MapViewOfFile(data) failed");
        }
        m->hdr = static_cast<const DataHeader*>(m->base);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m->hdr->magic != kDataMagic || m->hdr->generation != generation || m->hdr->slot_count == 0) {
            return fail("data mapping header invalid");
        }
        map_ = std::move(m);
        mapGen_ = generation;
        return true;
    }

    bool snapshot(uint64_t minExclusive, FrameView& out) {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint64_t latest = ctl_->latest_frame.load(std::memory_order_acquire);
            if (latest == 0 || latest <= minExclusive) {
                return false;
            }
            const uint32_t gen = ctl_->generation.load(std::memory_order_acquire);
            if (!ensure_mapping(gen)) {
                return false;
            }

            const DataHeader* h = map_->hdr;
            const uint8_t* base = static_cast<const uint8_t*>(map_->base);
            const uint8_t* slotBase = base + h->slots_offset + (latest % h->slot_count) * h->slot_stride;
            const SlotHeader* slot = reinterpret_cast<const SlotHeader*>(slotBase);

            const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
            if ((s1 & 1) != 0 || slot->frame_index != latest) {
                YieldProcessor();
                continue;
            }

            out.width = h->width;
            out.height = h->height;
            out.stride = h->stride;
            out.format = h->format;
            out.bytes_per_pixel = h->bytes_per_pixel;
            out.frame_index = latest;
            out.timestamp_qpc = slot->timestamp_qpc;
            out.publish_qpc = slot->publish_qpc;
            out.data = slotBase + kSlotHeaderBytes;
            out.slot_ = slot;
            out.seq_ = s1;
            out.keepalive_ = map_;

            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot->seq.load(std::memory_order_relaxed) != s1) continue;
            return true;
        }
        return false;
    }

    bool wait_for_newer(uint64_t deadline) {
        if (options_.spin_us) {
            const uint64_t spinEnd = qpc_now() + uint64_t(options_.spin_us) * freq_ / 1000000ull;
            while (qpc_now() < spinEnd) {
                if (ctl_->latest_frame.load(std::memory_order_acquire) > last_index_) {
                    return true;
                }
                YieldProcessor();
            }
        }
        while (true) {
            if (ctl_->latest_frame.load(std::memory_order_acquire) > last_index_) {
                return true;
            }
            const uint64_t now = qpc_now();
            if (now >= deadline) {
                return false;
            }
            const uint64_t remainingMs = ((deadline - now) * 1000 + freq_ - 1) / freq_;
            const DWORD slice = DWORD(remainingMs < 5 ? remainingMs : 5);
            WaitForSingleObject(events_[(last_index_ + 1) & 1], slice ? slice : 1);
            heartbeat();
        }
    }

    static bool copy_view(const FrameView& v, Frame& out) {
        static_cast<FrameInfo&>(out) = v;
        out.data.resize(v.size_bytes());
        std::memcpy(out.data.data(), v.data, v.size_bytes());
        return v.valid();
    }

    bool acquire(Frame* copyOut, FrameView* viewOut, bool onlyNew, uint32_t timeoutMs) {
        if (!ctl_) {
            const DWORD now = GetTickCount();
            if (now - lastReconnectTick_ < 250) {
                Sleep(timeoutMs < 50 ? timeoutMs : 50);
                return false;
            }
            lastReconnectTick_ = now;
            if (!connect()) {
                Sleep(timeoutMs < 50 ? timeoutMs : 50);
                return false;
            }
        }
        heartbeat();
        const uint64_t t0 = qpc_now();
        const uint64_t deadline = t0 + uint64_t(onlyNew ? timeoutMs : 5) * freq_ / 1000;
        while (true) {
            if (onlyNew && !wait_for_newer(deadline)) {
                return false;
            }
            FrameView v;
            if (snapshot(onlyNew ? last_index_ : 0, v)) {
                if (viewOut) {
                    last_index_ = v.frame_index;
                    *viewOut = std::move(v);
                    return true;
                }
                if (copy_view(v, *copyOut)) {
                    last_index_ = v.frame_index;
                    return true;
                }
            }
            if (qpc_now() >= deadline) {
                return false;
            }
            YieldProcessor();
        }
    }

    ReaderOptions options_;
    std::string lastError_;
    HANDLE ctlMap_ = nullptr;
    ControlBlock* ctl_ = nullptr;
    HANDLE events_[2] = {nullptr, nullptr};
    ReaderEntry* entry_ = nullptr;
    uint64_t myId_ = 0;
    uint64_t freq_ = 0;
    uint64_t last_index_ = 0;
    std::shared_ptr<Mapping> map_;
    uint32_t mapGen_ = 0;
    DWORD lastReconnectTick_ = 0;
};

}