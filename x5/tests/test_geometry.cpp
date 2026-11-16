#include "x5/capture.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
std::vector<uint8_t> bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("fixture not available");
    return {std::istreambuf_iterator<char>(in), {}};
}
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: test_geometry GOOD_H264 WRONG_H264 OUTPUT_DIRECTORY");
        auto good = bytes(argv[1]), wrong = bytes(argv[2]);
        for (bool startup : {false, true}) {
            std::vector<uint8_t> stream;
            for (const auto* part : startup ? std::vector<const std::vector<uint8_t>*>{&wrong, &good} :
                                            std::vector<const std::vector<uint8_t>*>{&good, &wrong, &good})
                stream.insert(stream.end(), part->begin(), part->end());
            fs::path path = fs::path(argv[3]) / (startup ? "cached_startup" : "sticky_violation");
            fs::create_directories(path);
            x5::CaptureOptions options; options.directory = path; options.native_resolution = 80;
            x5::CapturePipeline pipeline(options);
            require(pipeline.video(stream.data(), stream.size(), 123, 0, 1000), "combined stream rejected before decode");
            pipeline.finish();
            auto stats = pipeline.stats();const auto& last = stats.streams.at(0);
            require(last.width == 160 && last.height == 80, "test must finish on a good frame");
            require(last.native_size_seen, "target geometry was not confirmed");
            if (startup) require(stats.error.empty() && last.native_size_mismatch_frames == 0,
                                 "known pre-target cached geometry was treated as drift");
            else require(!stats.error.empty() && last.native_size_mismatch_frames == 1,
                         "good-wrong-good decoded frames hid a geometry violation");
            require(bytes(path / "video_stream_0.h264") == stream, "geometry enforcement discarded raw bytes");
        }
        std::cout << "PASS: sticky per-frame geometry violation and permitted cached startup; raw bytes preserved\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
