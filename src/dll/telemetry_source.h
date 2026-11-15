#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include <ets2la_telemetry/telemetry_reader.h>

#include "../../include/ets2la_capture/protocol.h"
#include "scs_logging.h"

namespace ets2la_capture {

// One telemetry snapshot together with the metadata that ends up in the SlotHeader.
struct TelemetrySnapshot {
    uint32_t flags = 0;
    uint64_t frame_index = 0;
    uint64_t timestamp_qpc = 0;
    uint64_t publish_qpc = 0;
    ets2la_telemetry::TelemetryData data;
};

// Reads the newest snapshot of the ets2la_telemetry plugin that runs in the same game process.
//
// The game runs the simulation step (telemetry frame_start/frame_end) and the rendering of the
// same frame one after another on one thread, and calls Present() last. So when capture() is called
// from the Present hook, the newest published snapshot is the one of the frame being presented.
// Only call capture() from the game thread inside Present.
class TelemetrySource {
public:
    void capture(TelemetrySnapshot& out, uint64_t presentQpc) {
        out.flags = 0;
        out.frame_index = 0;
        out.timestamp_qpc = 0;
        out.publish_qpc = 0;

        if (!ensure_connected()) {
            return;
        }
        if (reader_->stats().latest_frame == 0) {
            return;
        }

        ets2la_telemetry::TelemetryView view;
        bool copied = false;
        for (int attempt = 0; attempt < 3 && !copied; ++attempt) {
            if (!reader_->get_latest_telemetry_view(view) || !view.data) {
                return;
            }
            std::memcpy(&out.data, view.data, sizeof(ets2la_telemetry::TelemetryData));
            copied = view.valid();
        }
        if (!copied) {
            return;
        }

        out.frame_index = view.frame_index;
        out.timestamp_qpc = view.timestamp_qpc;
        out.publish_qpc = view.publish_qpc;
        out.flags = kSyncFlagTelemetryValid;
        if (lastFrameIndex_ != 0) {
            if (view.frame_index == lastFrameIndex_) {
                out.flags |= kSyncFlagTelemetryRepeated;
            } else if (view.frame_index > lastFrameIndex_ + 1) {
                out.flags |= kSyncFlagTelemetrySkipped;
            }
        }
        lastFrameIndex_ = view.frame_index;

        if (presentQpc > view.timestamp_qpc && reader_->ticks_to_ms(presentQpc - view.timestamp_qpc) > double(kTelemetryStaleMs)) {
            out.flags |= kSyncFlagTelemetryStale;
        }
    }

private:
    bool ensure_connected() {
        if (reader_ && reader_->connected()) {
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (now - lastConnectAttemptMs_ < 500) {
            return false;
        }
        lastConnectAttemptMs_ = now;
        if (!reader_) {
            reader_ = std::make_unique<ets2la_telemetry::TelemetryReader>();
        } else {
            reader_->connect();
        }
        if (reader_->connected()) {
            lastFrameIndex_ = 0;
            scs_logging::write(SCS_LOG_TYPE_message, "synced mode: telemetry plugin found");
            return true;
        }
        if (!warned_) {
            warned_ = true;
            const std::string& error = reader_->last_error();
            scs_logging::writef(
                SCS_LOG_TYPE_warning, "synced mode: telemetry plugin not available yet (%s), frames carry no telemetry",
                error.empty() ? "not loaded" : error.c_str()
            );
        }
        return false;
    }

    std::unique_ptr<ets2la_telemetry::TelemetryReader> reader_;
    ULONGLONG lastConnectAttemptMs_ = 0;
    uint64_t lastFrameIndex_ = 0;
    bool warned_ = false;
};

}