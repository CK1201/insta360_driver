#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

namespace x5 {
enum class Codec { h264, h265 };
enum class DecodeMode { byte_stream, x5_complete_h264 };
const char* codec_name(Codec codec);
int64_t steady_time_ns();

struct ImuSample {
    int64_t timestamp;
    double ax, ay, az, gx, gy, gz;
};
struct ExposureSample { double timestamp, exposure_time; };
struct PreviewFrame {
    cv::Mat image;
    int64_t received_ns = 0; // Host SDK callback receipt time, not exposure time.
    int64_t decoded_ns = 0;
    int64_t timestamp_ns = 0; // Host receipt minus configured sensor delay; an estimate.
};
struct StreamStats {
    uint64_t packets = 0, bytes = 0, frames = 0;
    int width = 0, height = 0;
    uint64_t startup_units_skipped = 0;
    uint64_t sps_patches = 0, timed_frames = 0;
    double decode_latency_sum_ms = 0, decode_latency_max_ms = 0;
    bool native_size_seen = false;
    uint64_t native_size_mismatch_frames = 0;
};
struct CaptureStats {
    std::map<int, StreamStats> streams;
    uint64_t imu_samples = 0, exposure_samples = 0;
    int64_t last_video_received_ns = 0, last_imu_received_ns = 0;
    std::optional<ImuSample> latest_imu;
    int64_t latest_imu_received_ns = 0;
    std::optional<double> latest_exposure_s;
    std::string error;
};
struct CaptureOptions {
    std::filesystem::path directory;
    Codec codec = Codec::h264;
    DecodeMode decode_mode = DecodeMode::byte_stream;
    bool save = true;
    // Decode even without a window, to measure real frame rate and dimensions.
    bool decode = true;
    size_t queue_budget_bytes = 64 * 1024 * 1024;
    int native_resolution = 0; // 0: general decoder; otherwise enforce combined 2R x R.
    int64_t sensor_delay_ns = 0;
    std::function<void(int, const PreviewFrame&)> on_frame;
};

class CapturePipeline {
public:
    explicit CapturePipeline(CaptureOptions options);
    ~CapturePipeline();
    CapturePipeline(const CapturePipeline&) = delete;
    CapturePipeline& operator=(const CapturePipeline&) = delete;
    bool video(const uint8_t* data, size_t size, int64_t timestamp,
               int stream_index, int64_t received_ns);
    bool imu(std::vector<ImuSample> samples, int64_t received_ns);
    bool exposure(ExposureSample sample, int64_t received_ns);
    void fail(const std::string& message);
    void finish();
    CaptureStats stats() const;
    std::map<int, PreviewFrame> latest_frames() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace x5
