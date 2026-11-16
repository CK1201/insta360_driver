// Test-only producer: real CapturePipeline/FFmpeg, no camera or synthetic IMU claims.
#include "x5/reader.hpp"
#include "../python/reader_holder.hpp"
#include <pybind11/pybind11.h>
#include <atomic>
#include <deque>
namespace py = pybind11;
namespace {
struct Packet { std::string bytes; int64_t received; int stream; };
struct State {
    std::mutex mutex;
    std::condition_variable available;
    std::deque<Packet> packets;
    bool ended = false;
    std::string error;
    std::atomic<uint64_t> frames{0};
};
struct Fixture {
    std::shared_ptr<State> state = std::make_shared<State>();
    std::shared_ptr<x5::Reader> reader;
    Fixture(const std::string& codec, const std::string& directory, bool save, int shutdown_delay_ms, double sensor_delay_s) {
        x5::Config config; config.save = save; config.sensor_delay_s = sensor_delay_s;
        auto shared = state;
        reader = x5::python::make_reader(config,
            [shared, codec, directory, save, shutdown_delay_ms](const auto& config, const auto& stop, const auto& publish) {
                x5::CaptureOptions options;
                options.codec = codec == "h264" ? x5::Codec::h264 : x5::Codec::h265;
                options.save = save; options.directory = directory;
                options.sensor_delay_ns = config.sensor_delay_ns();
                options.on_frame = [shared, publish](int stream, const x5::PreviewFrame& frame) {
                    publish(stream, frame); ++shared->frames;
                };
                x5::CapturePipeline pipeline(options);
                while (!stop()) {
                    Packet packet;
                    {
                        std::unique_lock<std::mutex> lock(shared->mutex);
                        shared->available.wait_for(lock, std::chrono::milliseconds(5), [&] {
                            return !shared->packets.empty() || shared->ended || !shared->error.empty();
                        });
                        if (!shared->error.empty()) { pipeline.fail(shared->error); break; }
                        if (shared->packets.empty()) { if (shared->ended) break; else continue; }
                        packet = std::move(shared->packets.front()); shared->packets.pop_front();
                    }
                    pipeline.video(reinterpret_cast<const uint8_t*>(packet.bytes.data()), packet.bytes.size(),
                        packet.received + 1000, packet.stream, packet.received);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(shutdown_delay_ms));
                pipeline.finish();
                const auto error = pipeline.stats().error;
                return x5::RunResult{error.empty() ? 0 : 1, error};
            });
    }
    void push(const std::string& bytes, int64_t received, int stream) {
        { std::lock_guard<std::mutex> lock(state->mutex); state->packets.push_back({bytes, received, stream}); }
        state->available.notify_one();
    }
    void end() {
        { std::lock_guard<std::mutex> lock(state->mutex); state->ended = true; }
        state->available.notify_one();
    }
    void fail(const std::string& error) {
        { std::lock_guard<std::mutex> lock(state->mutex); state->error = error; }
        state->available.notify_one();
    }
};
}
PYBIND11_MODULE(_x5_reader_fixture, m) {
    py::module_::import("insta360_x5");
    py::class_<Fixture>(m, "Fixture")
        .def(py::init<const std::string&, const std::string&, bool, int, double>(),
            py::arg("codec"), py::arg("directory"), py::arg("save"), py::arg("shutdown_delay_ms") = 0,
            py::arg("sensor_delay_s") = 0.0)
        .def_readonly("reader", &Fixture::reader)
        .def("push", &Fixture::push).def("end", &Fixture::end).def("fail", &Fixture::fail)
        .def_property_readonly("frames", [](const Fixture& f) { return f.state->frames.load(); });
}
