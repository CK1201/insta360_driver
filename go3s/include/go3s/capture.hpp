#pragma once
#include "go3s/config.hpp"
#include "go3s/wire.hpp"
#include "go3s/decoder.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
namespace go3s {
class EventQueue {
public:
    explicit EventQueue(size_t capacity = 64u * 1024u * 1024u) : capacity_(capacity) {}
    bool push(Event event);
    bool pop(Event& event);
    void close();
    size_t high_water() const;
private:
    const size_t capacity_;
    size_t bytes_ = 0, high_water_ = 0;
    bool closed_ = false;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Event> events_;
};
struct CaptureResult { bool complete = false; std::string error; Json summary; std::filesystem::path directory; };
struct CaptureOptions {
    std::function<void(const Frame&)> on_frame;
    std::function<void(const std::string&)> on_error;
    bool monitor_stdin = true;
    // Explicit Reader.stop() may cancel before the first camera packet.
    // Timed captures and all protocol/decode/output errors remain failures.
    bool allow_empty_stop = false;
};
// Owns no physical-device discovery. Caller supplies one connected loopback socket.
CaptureResult capture(int fd, const Config& config, const std::function<bool()>& interrupted = {},
                      DecodeMode mode = DecodeMode::complete_h264_low_delay,
                      const CaptureOptions& options = {});
}
