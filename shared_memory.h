#pragma once
#include <windows.h>
#include "common/shared_frame.h"
#include "scs_logging.h"

class SharedFrameWriter {
public:
    bool init() {
        hMapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, (DWORD)SHM_TOTAL_SIZE, SHM_NAME);
        if (!hMapping_) return false;

        view_ = MapViewOfFile(hMapping_, FILE_MAP_ALL_ACCESS, 0, 0, SHM_TOTAL_SIZE);
        if (!view_) return false;

        hEvent_ = CreateEventW(nullptr, TRUE, FALSE, EVENT_NAME);
        if (!hEvent_) return false;

        hMutex_ = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
        if (!hMutex_) return false;

        header_ = reinterpret_cast<FrameHeader*>(view_);
        pixels_ = reinterpret_cast<uint8_t*>(view_) + sizeof(FrameHeader);

        ZeroMemory(header_, sizeof(FrameHeader));
        header_->magic = FRAME_MAGIC;
        return true;
    }

    void publish(const uint8_t* src, uint32_t width, uint32_t height, uint32_t srcRowPitch) {
        if (!view_) return;
        if (width == 0 || height == 0 || width > MAX_WIDTH || height > MAX_HEIGHT) return;

        uint32_t dstRowPitch = width * 4;
        size_t payloadBytes = size_t(dstRowPitch) * height;
        if (payloadBytes > MAX_FRAME_BYTES) return;

        DWORD wait = WaitForSingleObject(hMutex_, 100);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            return;
        }

        ResetEvent(hEvent_);

        if (srcRowPitch == dstRowPitch) {
            memcpy(pixels_, src, payloadBytes);
        } else {
            for (uint32_t y = 0; y < height; ++y) {
                memcpy(
                    pixels_ + size_t(y) * dstRowPitch,
                    src + size_t(y) * srcRowPitch,
                    dstRowPitch
                );
            }
        }

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        header_->width = width;
        header_->height = height;
        header_->stride = dstRowPitch;
        header_->timestamp_qpc = qpc.QuadPart;
        header_->frame_index++;
        header_->format = 0;

        ReleaseMutex(hMutex_);
        SetEvent(hEvent_);
    }

    ~SharedFrameWriter() {
        if (view_) UnmapViewOfFile(view_);
        if (hMapping_) CloseHandle(hMapping_);
        if (hEvent_) CloseHandle(hEvent_);
        if (hMutex_) CloseHandle(hMutex_);
    }

private:
    HANDLE hMapping_ = nullptr;
    HANDLE hEvent_ = nullptr;
    HANDLE hMutex_ = nullptr;
    void* view_ = nullptr;
    FrameHeader* header_ = nullptr;
    uint8_t* pixels_ = nullptr;
};