#pragma once
#include "x5/capture.hpp"
#include <functional>

namespace x5 {
// One decoder per stream_index. Byte streams use a parser; verified complete
// X5 H.264 access units bypass it and patch only their preview SPS copy.
class VideoDecoder {
public:
    VideoDecoder(Codec codec, std::function<void(PreviewFrame)> on_frame,
                 DecodeMode mode = DecodeMode::byte_stream);
    ~VideoDecoder();
    void feed(const uint8_t* data, size_t size, int64_t received_ns = 0);
    void finish();
    uint64_t startup_units_skipped() const;
    uint64_t sps_patches() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
