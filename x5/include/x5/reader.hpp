#pragma once
#include "x5/session.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace x5 {
struct ReadTimeout : std::runtime_error { ReadTimeout() : std::runtime_error("timed out waiting for a new frame") {} };
struct ReaderStopped : std::runtime_error { ReaderStopped() : std::runtime_error("reader stopped") {} };
struct ReaderFrame {
    cv::Mat image;
    uint64_t index = 0;
    int stream_index = 0;
    int64_t host_receive_ns = 0, host_decode_ns = 0, timestamp_ns = 0;
};
class Reader {
public:
    using Runner = std::function<RunResult(const Config&, const std::function<bool()>&, const FrameObserver&)>;
    explicit Reader(Config config, Runner runner = run_session);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    void start();
    void stop();
    std::optional<ReaderFrame> latest(int stream_index = 0);
    ReaderFrame read(std::optional<double> timeout = 1.0, int stream_index = 0);
    bool running() const;
    std::string error() const;
private:
    void publish(int index, const PreviewFrame& frame);
    Config config_;
    Runner runner_;
    std::mutex lifecycle_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::map<int, ReaderFrame> frames_;
    std::map<int, uint64_t> seen_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    bool running_ = false;
    uint64_t generation_ = 0;
    std::string error_;
};
}
