#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <ets2la_telemetry/telemetry_reader.h>

#include "frame_reader.h"

namespace ets2la_capture {

// Owning copy of a frame and its telemetry. Reuse one instance across calls to avoid allocations.
struct SyncedFrame {
    Frame frame;
    ets2la_telemetry::Telemetry telemetry;
    uint32_t flags = 0;

    bool telemetry_repeated() const { return (flags & kSyncFlagTelemetryRepeated) != 0; }
    bool telemetry_skipped() const { return (flags & kSyncFlagTelemetrySkipped) != 0; }
    bool telemetry_stale() const { return (flags & kSyncFlagTelemetryStale) != 0; }
};

// Non-owning view straight into shared memory. Keeps the mapping alive, but the
// writer may overwrite the slot after kSlotCount-1 newer frames: call valid()
// after you are done reading and discard the result if it returns false.
class SyncedFrameView {
public:
    FrameView frame;
    const ets2la_telemetry::TelemetryData* telemetry = nullptr;
    ets2la_telemetry::TelemetryInfo telemetry_info;
    uint32_t flags = 0;

    bool valid() const {
        return frame.valid();
    }
};

class SyncedReader {
public:
    explicit SyncedReader(ReaderOptions options = {}) : frames_(options) {}

    bool connected() const {
        return frames_.connected();
    }
    const std::string& last_error() const {
        return error_.empty() ? frames_.last_error() : error_;
    }

    // Blocks until a frame newer than the previous one returned by this reader
    // exists (or timeout) and copies the frame and its telemetry into `out`.
    bool get(SyncedFrame& out, uint32_t timeout_ms = 1000) {
        FrameView view;
        if (!frames_.get_frame_view(view, timeout_ms)) {
            return false;
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (copy_view(view, out)) {
                return true;
            }
            if (!frames_.get_latest_frame_view(view)) {
                return false;
            }
        }
        return false;
    }

    // Non-blocking: newest frame available right now, copied into `out`.
    bool get_latest(SyncedFrame& out) {
        FrameView view;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (!frames_.get_latest_frame_view(view)) {
                return false;
            }
            if (copy_view(view, out)) {
                return true;
            }
        }
        return false;
    }

    // Blocks until a frame newer than the previous one returned by this reader
    // exists (or timeout) and returns a view into shared memory. The caller must
    // call valid() on the returned view after it is done reading to ensure that
    // the writer did not overwrite the slot in the meantime.
    bool get_view(SyncedFrameView& out, uint32_t timeout_ms = 1000) {
        FrameView view;
        if (!frames_.get_frame_view(view, timeout_ms)) {
            return false;
        }
        fill_view(std::move(view), out);
        return true;
    }

    // Non-blocking: newest frame available right now, returns a view into shared
    // memory. The caller must call valid() on the returned view after it is done
    // reading to ensure that the writer did not overwrite the slot in the meantime.
    bool get_latest_view(SyncedFrameView& out) {
        FrameView view;
        if (!frames_.get_latest_frame_view(view)) {
            return false;
        }
        fill_view(std::move(view), out);
        return true;
    }

    double ticks_to_ms(uint64_t ticks) const {
        return frames_.ticks_to_ms(ticks);
    }

    // Time since the game presented this frame, in milliseconds.
    double age_ms(const SyncedFrame& f) const {
        return frames_.age_ms(f.frame);
    }
    double age_ms(const SyncedFrameView& f) const {
        return frames_.age_ms(f.frame);
    }

    CaptureStats stats() const {
        return frames_.stats();
    }

    // The underlying frame reader, for everything that is not specific to synced mode.
    FrameReader& frames() {
        return frames_;
    }

private:
    bool telemetry_usable(const FrameView& view) {
        if ((view.sync_flags & kSyncFlagTelemetryValid) == 0 || !view.telemetry_data) {
            return false;
        }
        if (view.telemetry_size != sizeof(ets2la_telemetry::TelemetryData)) {
            error_ = "telemetry layout mismatch between capture DLL and client (build both from the same telemetry version)";
            return false;
        }
        return true;
    }

    bool copy_view(const FrameView& view, SyncedFrame& out) {
        static_cast<FrameInfo&>(out.frame) = view;
        out.frame.data.resize(view.size_bytes());
        std::memcpy(out.frame.data.data(), view.data, view.size_bytes());
        out.flags = view.sync_flags;
        out.telemetry.valid = false;
        if (telemetry_usable(view)) {
            std::memcpy(
                static_cast<ets2la_telemetry::TelemetryData*>(&out.telemetry), view.telemetry_data,
                sizeof(ets2la_telemetry::TelemetryData)
            );
            out.telemetry.frame_index = view.telemetry_frame_index;
            out.telemetry.timestamp_qpc = view.telemetry_timestamp_qpc;
            out.telemetry.publish_qpc = view.telemetry_publish_qpc;
            out.telemetry.valid = true;
        }
        if (!view.valid()) {
            out.telemetry.valid = false;
            return false;
        }
        return true;
    }

    void fill_view(FrameView&& view, SyncedFrameView& out) {
        out.flags = view.sync_flags;
        out.telemetry = telemetry_usable(view)
            ? reinterpret_cast<const ets2la_telemetry::TelemetryData*>(view.telemetry_data)
            : nullptr;
        out.telemetry_info.frame_index = view.telemetry_frame_index;
        out.telemetry_info.timestamp_qpc = view.telemetry_timestamp_qpc;
        out.telemetry_info.publish_qpc = view.telemetry_publish_qpc;
        out.frame = std::move(view);
    }

    FrameReader frames_;
    std::string error_;
};

}