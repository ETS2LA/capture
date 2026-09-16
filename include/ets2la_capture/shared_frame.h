#pragma once

#include <cstdint>

#define SHM_NAME   L"Local\\ETS2LA_FrameShare"
#define EVENT_NAME L"Local\\ETS2LA_FrameReady"
#define MUTEX_NAME L"Local\\ETS2LA_FrameMutex"

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t frame_index;
    uint64_t timestamp_qpc;
    uint32_t format;
};
#pragma pack(pop)

constexpr uint32_t FRAME_MAGIC = 0x45545332;

constexpr uint32_t MAX_WIDTH  = 7680;
constexpr uint32_t MAX_HEIGHT = 2160;
constexpr size_t   MAX_FRAME_BYTES = size_t(MAX_WIDTH) * MAX_HEIGHT * 4;
constexpr size_t   SHM_TOTAL_SIZE  = sizeof(FrameHeader) + MAX_FRAME_BYTES;