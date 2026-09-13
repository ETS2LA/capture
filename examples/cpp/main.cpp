
#include "frame_reader.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <print>

using namespace std;

int main() {
    ets2la::FrameReader reader;
    if (!reader.ok()) {
        printf("Failed to open capture: %s\n", reader.last_error().c_str());
        println("Is the game running with ets2la_capture.dll loaded?");
        return 1;
    }

    ets2la::Frame frame;
    for (;;) {
        if (!reader.get_frame(frame)) {
            println("No frame within timeout, is the game running?");
            continue;
        }

        cv::Mat img(frame.height, frame.width, CV_8UC4, frame.data.data());
        cv::imshow("ets2la capture", img);
        cv::waitKey(1);
    }
    return 0;
}