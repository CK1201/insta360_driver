#include "x5/reader.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        x5::Config config;
        config.preview = true; // Reader must override GUI without altering caller config.
        config.save = false;
        auto backend = [](const x5::Config& c, const std::function<bool()>& stop,
                          const x5::FrameObserver& publish) {
            require(!c.preview, "Python backend must be headless");
            cv::Mat pixels(4, 8, CV_8UC3, cv::Scalar(1, 2, 3));
            publish(0, {pixels, 123, 456});
            publish(0, {pixels, 124, 457});
            publish(1, {pixels, 125, 458});
            while (!stop()) std::this_thread::sleep_for(1ms);
            return x5::RunResult{};
        };
        x5::Reader reader(config, backend);
        require(!reader.latest(0), "latest before start");
        reader.start(); reader.start();
        auto f = reader.read(1.0, 0);
        require(f.index >= 1 && f.stream_index == 0, "frame identity");
        require(f.host_receive_ns >= 123 && f.host_decode_ns >= 456, "frame times");
        auto other = reader.read(1.0, 1);
        require(other.index == 1 && other.stream_index == 1, "per stream sequence");
        auto newest = reader.latest(0);
        require(newest && newest->index == 2, "latest slot replaces old frames");
        bool timed_out = false;
        try { reader.read(0.01, 0); } catch (const x5::ReadTimeout&) { timed_out = true; }
        require(timed_out, "read must wait for a NEW frame after latest");
        auto wait = std::async(std::launch::async, [&] {
            try { reader.read(1e30, 0); } catch (const x5::ReaderStopped&) { return true; }
            return false;
        });
        require(wait.wait_for(20ms) == std::future_status::timeout, "large finite timeout must not overflow");
        reader.stop(); reader.stop();
        require(wait.wait_for(1s) == std::future_status::ready && wait.get(), "stop wakes waiting read");
        require(!reader.running(), "stop joins backend");
        require(f.image.at<cv::Vec3b>(0,0)[0] == 1, "retained image lifetime");
        reader.start();
        require(reader.read(1.0, 1).index == 1, "restart resets cursor/index");
        reader.stop();
        x5::Reader failed(config, [](const auto&, const auto&, const auto&) -> x5::RunResult {
            throw std::runtime_error("synthetic device failure");
        });
        failed.start();
        bool propagated = false;
        try { failed.read(1.0, 0); } catch (const std::runtime_error& e) {
            propagated = std::string(e.what()) == "synthetic device failure";
        }
        failed.stop();
        require(propagated && failed.error() == "synthetic device failure", "original backend error");
        std::cout << "reader freshness, streams, timeout, shutdown, restart and error tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
