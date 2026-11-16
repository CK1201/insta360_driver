#include "x5/decoder.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <array>
extern "C" {
#include <libavutil/md5.h>
}

namespace fs = std::filesystem;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Packet { std::vector<uint8_t> data; int64_t received; };
struct Result {
    std::vector<std::array<uint8_t, 16>> hashes;
    std::vector<int> hold;
    std::vector<int64_t> receipts;
    int frames_before_flush = 0;
    uint64_t startup_skips = 0;
};
Result replay(const std::vector<Packet>& packets, x5::DecodeMode mode) {
    Result result;
    int feed_index = 0;
    bool flushing = false;
    x5::VideoDecoder decoder(x5::Codec::h264, [&](x5::PreviewFrame frame) {
        if (!flushing) result.hold.push_back(feed_index - static_cast<int>(result.hashes.size()));
        std::array<uint8_t, 16> hash;
        av_md5_sum(hash.data(), frame.image.data, frame.image.total() * frame.image.elemSize());
        result.hashes.push_back(hash);
        result.receipts.push_back(frame.received_ns);
    }, mode);
    for (const auto& packet : packets) {
        auto original = packet.data;
        decoder.feed(packet.data.data(), packet.data.size(), packet.received);
        require(packet.data == original, "preview decoder modified original packet bytes");
        ++feed_index;
    }
    result.frames_before_flush = result.hashes.size();
    flushing = true;
    decoder.finish();
    result.startup_skips = decoder.startup_units_skipped();
    return result;
}
std::vector<Packet> load_packets(const fs::path& dir) {
    std::ifstream csv(dir / "video_packets.csv"), raw(dir / "video_stream_0.h264", std::ios::binary);
    require(csv.good() && raw.good(), "latency capture fixture unavailable");
    std::string line; std::getline(csv, line);
    std::vector<Packet> packets;
    while (std::getline(csv, line)) {
        std::stringstream in(line); std::vector<std::string> values; std::string value;
        while (std::getline(in, value, ',')) values.push_back(value);
        require(values.size() == 6 && values[1] == "0", "unexpected fixture packet layout");
        Packet packet;
        packet.data.resize(std::stoull(values[5]));
        packet.received = 1000000000LL + static_cast<int64_t>(packets.size()) * 33366667;
        raw.seekg(std::stoull(values[4]));
        raw.read(reinterpret_cast<char*>(packet.data.data()), packet.data.size());
        require(raw.good(), "fixture index exceeds video bytes");
        packets.push_back(std::move(packet));
    }
    require(!packets.empty(), "empty capture fixture");
    return packets;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2 || argc == 3, "usage: test_low_latency CAPTURE_DIRECTORY [B_FRAME_CAPTURE_DIRECTORY]");
        fs::path dir = argv[1];
        auto packets = load_packets(dir);
        auto reference = replay(packets, x5::DecodeMode::byte_stream);
        auto fast = replay(packets, x5::DecodeMode::x5_complete_h264);
        require(fast.hashes.size() == reference.hashes.size() && fast.hashes.size() == packets.size(), "low-latency path lost frames");
        require(fast.frames_before_flush == static_cast<int>(packets.size()), "low-latency decoder still buffers frames until flush");
        require(!fast.hold.empty(), "no frames returned during input");
        for (int frames : fast.hold) require(frames == 0, "low-latency decoder adds frame buffering");
        for (size_t i = 0; i < fast.hashes.size(); ++i) {
            require(fast.hashes[i] == reference.hashes[i], "optimized preview changed decoded pixels");
            require(fast.receipts[i] == packets[i].received, "frame lost its SDK callback receipt timestamp");
            require(reference.receipts[i] == packets[i].received, "byte-stream frame receipt association incorrect");
        }
        // Starting on an old P access unit must not feed an uninitialized decoder.
        require(packets.size() > 1, "fixture needs a P frame");
        auto prefixed = packets;
        auto prefix = packets[1];
        prefix.received -= 2 * 33366667;
        prefixed.insert(prefixed.begin(), prefix);
        auto restarted = replay(prefixed, x5::DecodeMode::x5_complete_h264);
        require(restarted.startup_skips == 1 && restarted.hashes == reference.hashes,
                "complete-packet startup gate lost or changed frames");
        require(restarted.receipts == fast.receipts, "startup skip shifted frame receipt timestamps");

        auto recording = fs::temp_directory_path() / ("x5-raw-recording-test-" + std::to_string(x5::steady_time_ns()));
        fs::create_directories(recording);
        x5::CaptureOptions options;
        options.directory = recording;
        options.decode_mode = x5::DecodeMode::x5_complete_h264;
        x5::CapturePipeline pipeline(options);
        std::vector<uint8_t> original;
        for (const auto& packet : packets) {
            original.insert(original.end(), packet.data.begin(), packet.data.end());
            require(pipeline.video(packet.data.data(), packet.data.size(), 1234, 0, packet.received), "recording submission failed");
        }
        require(pipeline.imu({{12, 1, 2, 3, 4, 5, 6}}, 5000), "IMU rejected in optimized recording");
        require(pipeline.exposure({12, 0.01}, 6000), "exposure rejected in optimized recording");
        pipeline.finish();
        auto stats = pipeline.stats();
        require(stats.error.empty(), stats.error.c_str());
        require(stats.streams.at(0).frames == packets.size() && stats.streams.at(0).sps_patches > 0,
                "recording did not use verified low-delay SPS path");
        require(stats.imu_samples == 1 && stats.exposure_samples == 1, "optimized preview dropped sensor samples");
        std::ifstream saved(recording / "video_stream_0.h264", std::ios::binary);
        std::vector<uint8_t> written{std::istreambuf_iterator<char>(saved), {}};
        require(written == original, "preview SPS patch leaked into raw recording");
        if (argc == 3) {
            bool rejected = false;
            try { replay(load_packets(argv[2]), x5::DecodeMode::x5_complete_h264); }
            catch (const std::exception& error) { rejected = std::string(error.what()).find("B-frame") != std::string::npos; }
            require(rejected, "I/P-only fast path silently accepted B frames");
        }
        std::cout << "PASS: production decoder returns " << fast.hashes.size()
                  << " unchanged frames with zero frame hold (byte-stream hold " << reference.hold.front()
                  << "), exact receipt timestamps and original recording bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
