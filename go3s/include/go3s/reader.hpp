#pragma once
#include "go3s/capture.hpp"
#include <atomic>
#include <optional>
#include <stdexcept>
#include <thread>

namespace go3s {
struct ReaderFrame {
    cv::Mat image;
    uint64_t index = 0;
    int stream_index = 0;
    int64_t host_receive_ns = 0, host_decode_ns = 0, timestamp_ns = 0;
};
struct ReaderTimeout : std::runtime_error { using std::runtime_error::runtime_error; };
struct ReaderEnded : std::runtime_error { using std::runtime_error::runtime_error; };

// One immutable native frame slot and one consumption cursor per Reader.
// No Python objects or Python callbacks are accessed by capture threads.
class Reader {
public:
    explicit Reader(const std::string& config);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader& start();
    void stop();
    std::optional<ReaderFrame> latest(int stream_index = 0);
    ReaderFrame read(std::optional<double> timeout = 1.0, int stream_index = 0);
    bool running() const;
    std::optional<std::string> error() const;
private:
    void run();
    void publish(const Frame& frame);
    static void check_stream(int stream_index);
    Config config_;
    std::mutex lifecycle_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    bool running_ = false;
    std::string error_;
    std::optional<ReaderFrame> latest_;
    uint64_t next_index_ = 0, cursor_ = 0, generation_ = 0;
};
}
