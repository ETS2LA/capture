#include "ets2la_capture/synced_reader.h"
#include <opencv2/opencv.hpp>
#include <cstdio>


int main() {
    ets2la_capture::SyncedReader reader;
    ets2la_capture::SyncedFrame synced;

    while (true) {
        if (!reader.get(synced) || !synced.telemetry.valid) {
            continue;
        }

        cv::Mat native(int(synced.frame.height), int(synced.frame.width), CV_8UC4, synced.frame.data.data(), synced.frame.stride);
        cv::Mat bgr;
        if (synced.frame.layout() == ets2la_capture::PixelLayout::RGBA8) {
            cv::cvtColor(native, bgr, cv::COLOR_RGBA2BGR);
        } else {
            cv::cvtColor(native, bgr, cv::COLOR_BGRA2BGR);
        }

        cv::putText(
            bgr,
            std::format("{:.1f} km/h", synced.telemetry.truck.speed * 3.6f),
            cv::Point(10, 30),
            cv::FONT_HERSHEY_SIMPLEX,
            1.0,
            cv::Scalar(0, 255, 0),
            2
        );

        cv::imshow("ets2la_capture synced", bgr);
        cv::waitKey(1);
    }
    return 0;
}