#pragma once
#include "x5/capture.hpp"
#include "x5/config.hpp"

namespace x5 {
struct PreviewContext {
    Config config;
    YAML::Node identity;
    YAML::Node readback;
    std::filesystem::path output_directory;
    Codec codec = Codec::h264;
};
struct PreviewLive {
    CaptureStats capture;
    std::optional<double> video_fps, imu_hz;
    std::string battery = "--", power = "--";
    double elapsed_s = 0;
    int64_t frame_received_ns = 0, frame_decoded_ns = 0;
};

// Composes a display-only image. The camera image and recording path stay raw.
cv::Mat render_preview(const cv::Mat& frame, int stream_index,
                       const PreviewContext& context, const PreviewLive& live,
                       int64_t now_ns);
}
