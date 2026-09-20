#include "ets2la_capture/frame_reader.h"
#include <opencv2/opencv.hpp>
#include <cstdio>


int main() {
    ets2la_capture::FrameReader reader;
    ets2la_capture::Frame frame;

    while (true) {
        if (!reader.get_frame(frame)) {
            continue;
        }

        cv::Mat native(int(frame.height), int(frame.width), CV_8UC4, frame.data.data(), frame.stride);
        cv::Mat bgr;
        if (frame.layout() == ets2la_capture::PixelLayout::RGBA8) {
            cv::cvtColor(native, bgr, cv::COLOR_RGBA2BGR);
        } else {
            cv::cvtColor(native, bgr, cv::COLOR_BGRA2BGR);
        }

        cv::imshow("ets2la_capture", bgr);
        cv::waitKey(1);
    }
    return 0;
}