#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace ets2la_capture {

constexpr uint32_t kControlMagic = 0x32435445;  // "ETC2"
constexpr uint32_t kDataMagic = 0x32445445;     // "ETD2"

constexpr uint32_t kSlotCount = 6;              // frames kept in the ring
constexpr uint32_t kMaxReaders = 32;            // heartbeat table size
constexpr uint32_t kReaderTimeoutMs = 2000;     // reader considered gone after this

constexpr uint64_t kControlBytes = 4096;
constexpr uint64_t kDataHeaderBytes = 4096;
constexpr uint64_t kSlotHeaderBytes = 256;      // pixels start this far into a slot

inline constexpr const wchar_t* kControlName = L"Local\\ETS2LA_Capture_Control";
inline constexpr const wchar_t* kEventNames[2] = {
    L"Local\\ETS2LA_Capture_Frame0",
    L"Local\\ETS2LA_Capture_Frame1",
};

inline void make_data_name(wchar_t* out, size_t count, uint32_t generation) {
    std::swprintf(out, count, L"Local\\ETS2LA_Capture_Data_%u", generation);
}

struct ReaderEntry {                        // 64 bytes, one cache line per reader
    std::atomic<uint64_t> id;               // 0 = free, else (pid << 32) | counter
    std::atomic<uint64_t> heartbeat_ms;     // GetTickCount64() at last sign of life
    uint8_t pad[48];
};

struct ControlBlock {
    // --- cache line 0: written once by the writer ---
    uint32_t magic;                         //   0
    uint32_t slot_count;                    //   4
    uint32_t max_readers;                   //   8
    uint64_t qpc_frequency;                 //  16
    uint32_t writer_pid;                    //  24
    uint32_t pad0;                          //  28
    uint8_t pad1[32];                       //  32
    // --- cache line 1..: hot, updated per frame by the writer ---
    std::atomic<uint32_t> generation;       //  64  data mapping currently in use
    std::atomic<uint32_t> capturing;        //  68  1 while the DLL is actively capturing
    std::atomic<uint64_t> latest_frame;     //  72  index of newest complete frame, 0 = none
    std::atomic<uint64_t> frames_published; //  80
    std::atomic<uint64_t> frames_dropped;   //  88  frames the capture side had to skip
    std::atomic<uint64_t> last_publish_qpc; //  96
    uint8_t pad2[24];                       // 104
    ReaderEntry readers[kMaxReaders];       // 128
};

struct DataHeader {                         // lives in the first 4096 bytes
    uint32_t magic;                         //  0
    uint32_t generation;                    //  4
    uint32_t slot_count;                    //  8
    uint64_t slot_stride;                   // 16  bytes from one slot to the next
    uint64_t slots_offset;                  // 24  == kDataHeaderBytes
    uint32_t width;                         // 32
    uint32_t height;                        // 36
    uint32_t stride;                        // 40  bytes per row (tightly packed)
    uint32_t format;                        // 44  DXGI_FORMAT value
    uint32_t bytes_per_pixel;               // 48
    uint32_t pad0;                          // 52
    uint64_t total_size;                    // 56
};

struct SlotHeader {                         // 64 bytes, pixels follow at +kSlotHeaderBytes
    std::atomic<uint64_t> seq;              //  0  even = stable, odd = being written
    uint64_t frame_index;                   //  8
    uint64_t timestamp_qpc;                 // 16  QPC when the game called Present()
    uint64_t publish_qpc;                   // 24  QPC when the frame became readable
    uint8_t pad[32];
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "need lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "need lock-free 32-bit atomics");
static_assert(sizeof(ReaderEntry) == 64, "ReaderEntry layout");
static_assert(sizeof(SlotHeader) == 64, "SlotHeader layout");
static_assert(offsetof(ControlBlock, generation) == 64, "ControlBlock layout");
static_assert(offsetof(ControlBlock, latest_frame) == 72, "ControlBlock layout");
static_assert(offsetof(ControlBlock, frames_published) == 80, "ControlBlock layout");
static_assert(offsetof(ControlBlock, frames_dropped) == 88, "ControlBlock layout");
static_assert(offsetof(ControlBlock, last_publish_qpc) == 96, "ControlBlock layout");
static_assert(offsetof(ControlBlock, readers) == 128, "ControlBlock layout");
static_assert(sizeof(ControlBlock) <= kControlBytes, "ControlBlock too large");
static_assert(offsetof(DataHeader, slot_stride) == 16, "DataHeader layout");
static_assert(offsetof(DataHeader, width) == 32, "DataHeader layout");
static_assert(offsetof(DataHeader, total_size) == 56, "DataHeader layout");
static_assert(sizeof(DataHeader) <= kDataHeaderBytes, "DataHeader too large");

inline constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
inline constexpr uint64_t slot_stride_for(uint64_t payload_bytes) {
    return align_up(kSlotHeaderBytes + payload_bytes, 4096);
}

enum class PixelLayout : uint32_t { Unknown = 0, BGRA8, BGRX8, RGBA8, RGB10A2, RGBA16F };

inline PixelLayout pixel_layout(uint32_t dxgi_format) {
    switch (dxgi_format) {
        case 87: case 90: case 91: return PixelLayout::BGRA8;    // B8G8R8A8 UNORM/TYPELESS/SRGB
        case 88: case 92: case 93: return PixelLayout::BGRX8;    // B8G8R8X8 UNORM/TYPELESS/SRGB
        case 27: case 28: case 29: return PixelLayout::RGBA8;    // R8G8B8A8 TYPELESS/UNORM/SRGB
        case 23: case 24: case 89: return PixelLayout::RGB10A2;  // R10G10B10A2, XR_BIAS
        case 9: case 10: case 11:  return PixelLayout::RGBA16F;  // R16G16B16A16 (float/typeless/unorm)
        default: return PixelLayout::Unknown;
    }
}
inline uint32_t bytes_per_pixel_for(uint32_t dxgi_format) {
    switch (pixel_layout(dxgi_format)) {
        case PixelLayout::BGRA8: case PixelLayout::BGRX8:
        case PixelLayout::RGBA8: case PixelLayout::RGB10A2: return 4;
        case PixelLayout::RGBA16F: return 8;
        default: return 0;
    }
}
inline const char* pixel_layout_name(PixelLayout l) {
    switch (l) {
        case PixelLayout::BGRA8: return "BGRA8";
        case PixelLayout::BGRX8: return "BGRX8";
        case PixelLayout::RGBA8: return "RGBA8";
        case PixelLayout::RGB10A2: return "RGB10A2";
        case PixelLayout::RGBA16F: return "RGBA16F";
        default: return "unknown";
    }
}

}