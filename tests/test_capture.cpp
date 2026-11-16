#include "x5/capture.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "fixture cannot be opened");
    return {std::istreambuf_iterator<char>(in), {}};
}
void check_codec(const fs::path& fixture, const fs::path& directory, x5::Codec codec,
                 cv::Vec3b expected_bgr = {0, 0, 255}, uint64_t expected_startup_skips = 0) {
    fs::create_directories(directory);
    const auto bytes = read_bytes(fixture);
    x5::CaptureOptions options;
    options.directory = directory;
    options.codec = codec;
    x5::CapturePipeline pipeline(options);
    // Deliberately split NAL units across arbitrary callback-sized pieces.
    size_t cursor = 0;
    uint64_t packets = 0;
    while (cursor < bytes.size()) {
        size_t count = std::min<size_t>(37 + cursor % 113, bytes.size() - cursor);
        require(pipeline.video(bytes.data() + cursor, count, 900000 + packets,
                               0, 1000000 + packets), "video submission failed");
        cursor += count;
        ++packets;
    }
    require(pipeline.imu({{91, 1, 2, 3, 4, 5, 6}, {92, -1, -2, -3, -4, -5, -6}}, 700), "IMU submission failed");
    require(pipeline.exposure({93.5, 0.008}, 800), "exposure submission failed");
    pipeline.finish();
    pipeline.finish(); // Explicit shutdown followed by destruction must be harmless.
    const auto stats = pipeline.stats();
    require(stats.error.empty(), stats.error.c_str());
    require(stats.streams.at(0).frames == 12, "decoder lost buffered frames");
    require(stats.streams.at(0).width == 160 && stats.streams.at(0).height == 80, "decoded dimensions wrong");
    require(stats.streams.at(0).packets == packets, "accepted packet not processed");
    require(stats.streams.at(0).startup_units_skipped == expected_startup_skips,
            "pre-keyframe units not accounted for");
    require(stats.imu_samples == 2 && stats.exposure_samples == 1, "metadata not drained");
    require(stats.latest_imu.has_value(), "latest IMU sample unavailable to preview");
    require(stats.latest_imu->timestamp == 92 && stats.latest_imu->ax == -1 &&
            stats.latest_imu->ay == -2 && stats.latest_imu->az == -3 &&
            stats.latest_imu->gx == -4 && stats.latest_imu->gy == -5 && stats.latest_imu->gz == -6,
            "preview IMU is not the latest complete sample");
    require(stats.latest_imu_received_ns == 700, "preview IMU receive time incorrect");
    auto image = pipeline.latest_frames().at(0).image;
    auto bgr = image.at<cv::Vec3b>(40, 80);
    for (int channel = 0; channel < 3; ++channel)
        require(std::abs(int(bgr[channel]) - int(expected_bgr[channel])) <= 4,
                "decoded BGR colors wrong (range or color matrix)");
    auto saved = read_bytes(directory / (std::string("video_stream_0.") + x5::codec_name(codec)));
    require(saved == bytes, "saved video differs from accepted callback bytes");
    require(!pipeline.video(bytes.data(), 1, 0, 0, 0), "submission accepted after shutdown");
}
int main(int argc, char** argv) {
    try {
        require(argc == 6, "usage: test_capture H264 H265 FULL_RANGE_H264 PREFIXED_H264 OUTPUT_DIRECTORY");
        fs::path root = argv[5];
        check_codec(argv[1], root / "h264", x5::Codec::h264);
        check_codec(argv[2], root / "h265", x5::Codec::h265);
        check_codec(argv[3], root / "full_range", x5::Codec::h264, {192, 128, 64});
        check_codec(argv[4], root / "leading_interframe", x5::Codec::h264, {0, 0, 255}, 1);
        {
            auto directory = root / "decoder_failure";
            fs::create_directories(directory);
            x5::CaptureOptions options;
            options.directory = directory;
            x5::CapturePipeline pipeline(options);
            // The initial batch makes the writer busy while later events are
            // accepted, without a test-only pause hook or timing sleeps.
            std::vector<x5::ImuSample> initial(8000, {1, 1, 2, 3, 4, 5, 6});
            require(pipeline.imu(std::move(initial), 100), "initial IMU batch rejected");
            uint8_t malformed[] = {0,0,0,1,0x41,0x80,0,0,0,1,0x65,0x80,0,0,0,1,0x65,0x80,0,0,0,1,0x09,0xf0};
            require(pipeline.video(malformed, sizeof(malformed), 10, 0, 200), "malformed video rejected before decode");
            require(pipeline.imu({{2, 7, 8, 9, 10, 11, 12}}, 300), "trailing IMU not accepted");
            require(pipeline.exposure({3, 0.01}, 400), "trailing exposure not accepted");
            require(pipeline.video(malformed, sizeof(malformed), 20, 1, 500), "trailing video not accepted");
            pipeline.finish();
            auto stats = pipeline.stats();
            require(!stats.error.empty(), "invalid encoded video not reported");
            require(stats.streams.at(0).startup_units_skipped == 1,
                    "decoder error discarded startup-skip accounting");
            require(stats.imu_samples == 8001 && stats.exposure_samples == 1,
                    "decoder failure discarded accepted metadata");
            require(read_bytes(directory / "video_stream_1.h264").size() == sizeof(malformed),
                    "decoder failure discarded accepted raw video");
        }
        {
            auto directory = root / "multiple_streams";
            fs::create_directories(directory);
            x5::CaptureOptions options;
            options.directory = directory;
            options.decode = false;
            x5::CapturePipeline pipeline(options);
            uint8_t first[] = {1, 2, 3}, second[] = {9, 8};
            require(pipeline.video(first, 3, 10, 0, 100), "stream 0 rejected");
            require(pipeline.video(second, 2, 20, 1, 200), "stream 1 rejected");
            require(pipeline.video(first, 3, 30, 0, 300), "stream 0 continuation rejected");
            pipeline.finish();
            require(read_bytes(directory / "video_stream_0.h264") == std::vector<uint8_t>({1,2,3,1,2,3}), "stream 0 mixed/corrupted");
            require(read_bytes(directory / "video_stream_1.h264") == std::vector<uint8_t>({9,8}), "stream 1 mixed/corrupted");
        }
        {
            x5::CaptureOptions options;
            options.save = false;
            options.decode = false;
            options.queue_budget_bytes = 4;
            x5::CapturePipeline pipeline(options);
            uint8_t bytes[16] = {};
            require(!pipeline.video(bytes, sizeof(bytes), 0, 0, 0), "queue budget not enforced");
            pipeline.finish();
            require(!pipeline.stats().error.empty(), "overflow silently ignored");
        }
        {
            bool failed = false;
            try {
                x5::CaptureOptions options;
                options.directory = root / "missing_parent" / "run";
                x5::CapturePipeline pipeline(options);
            } catch (const std::exception&) { failed = true; }
            require(failed, "unwritable output accepted");
        }
        {
            fs::path directory = root / "disk_failure";
            fs::create_directories(directory);
            fs::create_symlink("/dev/full", directory / "imu.csv");
            bool failed = false;
            try {
                x5::CaptureOptions options;
                options.directory = directory;
                options.decode = false;
                x5::CapturePipeline pipeline(options);
                pipeline.imu({{1, 1, 2, 3, 4, 5, 6}}, 7);
                pipeline.finish();
                failed = !pipeline.stats().error.empty();
            } catch (const std::exception&) { failed = true; }
            require(failed, "buffered disk write error silently ignored");
        }
        std::cout << "PASS: chunked H264/H265, exact saving, drain, queue limit, write errors\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
