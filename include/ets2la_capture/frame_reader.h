#pragma once

#include <windows.h>
#include <vector>
#include <cstdint>
#include <string>
#include "shared_frame.h"

namespace ets2la {

struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame_index = 0;
    uint64_t timestamp_qpc = 0;
    std::vector<uint8_t> data;
};

class FrameReader {
public:
    FrameReader() {
        hMapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
        if (!hMapping_) { lastError_ = "OpenFileMappingW failed, is ets2la_capture.dll loaded?"; return; }

        hEvent_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, EVENT_NAME);
        if (!hEvent_) { lastError_ = "OpenEventW failed"; return; }

        hMutex_ = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, MUTEX_NAME);
        if (!hMutex_) { lastError_ = "OpenMutexW failed"; return; }

        view_ = MapViewOfFile(hMapping_, FILE_MAP_READ, 0, 0, SHM_TOTAL_SIZE);
        if (!view_) { lastError_ = "MapViewOfFile failed"; return; }

        header_ = reinterpret_cast<const FrameHeader*>(view_);
        pixels_ = reinterpret_cast<const uint8_t*>(view_) + sizeof(FrameHeader);
        ok_ = true;
    }

    ~FrameReader() {
        if (view_) UnmapViewOfFile(view_);
        if (hMapping_) CloseHandle(hMapping_);
        if (hEvent_) CloseHandle(hEvent_);
        if (hMutex_) CloseHandle(hMutex_);
    }

    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;

    bool ok() const { return ok_; }
    const std::string& last_error() const { return lastError_; }

    bool get_frame(Frame& out, DWORD timeout_ms = 1000) {
        if (!ok_) return false;
        ULONGLONG deadline = GetTickCount64() + timeout_ms;

        for (;;) {
            ULONGLONG now = GetTickCount64();
            DWORD remaining = (now >= deadline) ? 0 : DWORD(deadline - now);
            DWORD waitResult = WaitForSingleObject(hEvent_, remaining);
            if (waitResult == WAIT_TIMEOUT) return false;

            uint64_t frameIndex = 0;
            bool gotFrame = readLocked(out, frameIndex, 100);
            if (gotFrame && frameIndex != lastFrameIndex_) {
                lastFrameIndex_ = frameIndex;
                return true;
            }

            if (GetTickCount64() >= deadline) return false;
            Sleep(0);
        }
    }

    bool get_latest_frame(Frame& out) {
        if (!ok_) return false;
        uint64_t frameIndex = 0;
        if (!readLocked(out, frameIndex, 50)) return false;
        lastFrameIndex_ = frameIndex;
        return true;
    }

private:
    bool readLocked(Frame& out, uint64_t& frameIndexOut, DWORD mutexTimeoutMs) {
        DWORD wait = WaitForSingleObject(hMutex_, mutexTimeoutMs);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return false;

        FrameHeader snapshot = *header_;
        bool valid = snapshot.magic == FRAME_MAGIC && snapshot.width > 0 && snapshot.height > 0
                     && snapshot.width <= MAX_WIDTH && snapshot.height <= MAX_HEIGHT;

        if (valid) {
            out.width = snapshot.width;
            out.height = snapshot.height;
            out.frame_index = snapshot.frame_index;
            out.timestamp_qpc = snapshot.timestamp_qpc;
            out.data.resize(size_t(snapshot.width) * snapshot.height * 4);

            uint32_t dstRowBytes = snapshot.width * 4;
            if (snapshot.stride == dstRowBytes) {
                memcpy(out.data.data(), pixels_, size_t(dstRowBytes) * snapshot.height);
            } else {
                for (uint32_t y = 0; y < snapshot.height; ++y) {
                    memcpy(out.data.data() + size_t(y) * dstRowBytes,
                           pixels_ + size_t(y) * snapshot.stride,
                           dstRowBytes);
                }
            }
            frameIndexOut = snapshot.frame_index;
        }

        ReleaseMutex(hMutex_);
        return valid;
    }

    HANDLE hMapping_ = nullptr;
    HANDLE hEvent_ = nullptr;
    HANDLE hMutex_ = nullptr;
    void* view_ = nullptr;
    const FrameHeader* header_ = nullptr;
    const uint8_t* pixels_ = nullptr;
    uint64_t lastFrameIndex_ = 0;
    bool ok_ = false;
    std::string lastError_;
};

}