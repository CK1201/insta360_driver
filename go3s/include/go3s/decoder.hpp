#pragma once
#include <opencv2/core.hpp>
#include <functional>
#include <memory>
#include <string>
namespace go3s {
enum class DecodeMode { byte_stream, complete_h264_low_delay };
struct Frame { cv::Mat bgr; int64_t host_receive_ns = 0, host_decode_ns = 0, timestamp_ns = 0; };
class Decoder {
public:
    // Generic fragments use the parser; verified live H.264 callbacks are complete
    // I/P access units and can avoid parser/reorder waiting on a decoder-only copy.
    Decoder(const std::string& codec, std::function<void(Frame)> callback,
            DecodeMode mode = DecodeMode::byte_stream);
    ~Decoder();
    void feed(const uint8_t* data, size_t size, int64_t host_receive_ns);
    void finish();
    uint64_t startup_skipped() const;
    uint64_t sps_patches() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
