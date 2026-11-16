#include "x5/reader.hpp"
#include <cmath>

namespace x5 {
namespace {
void valid_stream(int index) { if (index < 0) throw std::invalid_argument("stream_index must be >= 0"); }
}
Reader::Reader(Config config, Runner runner) : config_(std::move(config)), runner_(std::move(runner)) {
    validate_config(config_);
    config_.preview = false;
}
Reader::~Reader() { stop(); }
void Reader::start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    { std::lock_guard<std::mutex> lock(mutex_); if (running_) return; }
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear(); seen_.clear(); error_.clear(); stop_requested_ = false; running_ = true;
        ++generation_;
    }
    try {
        worker_ = std::thread([this] {
            std::string error;
            try {
                auto result = runner_(config_, [this] { return stop_requested_.load(); },
                    [this](int index, const PreviewFrame& frame) { publish(index, frame); });
                if (result.code && !(stop_requested_ && result.cancelled_before_data))
                    error = result.error.empty() ? "capture failed" : result.error;
            } catch (const std::exception& e) { error = e.what(); }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = std::move(error); running_ = false;
            }
            ready_.notify_all();
        });
    } catch (...) {
        { std::lock_guard<std::mutex> lock(mutex_); running_ = false; }
        ready_.notify_all();
        throw;
    }
}
void Reader::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void Reader::publish(int index, const PreviewFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) return;
        auto& slot = frames_[index];
        slot = {frame.image, slot.index + 1, index, frame.received_ns, frame.decoded_ns, frame.timestamp_ns};
    }
    ready_.notify_all();
}
std::optional<ReaderFrame> Reader::latest(int index) {
    valid_stream(index);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error_.empty()) throw std::runtime_error(error_);
    auto found = frames_.find(index);
    if (found == frames_.end()) return std::nullopt;
    seen_[index] = found->second.index;
    return found->second;
}
ReaderFrame Reader::read(std::optional<double> timeout, int index) {
    valid_stream(index);
    if (timeout && (!std::isfinite(*timeout) || *timeout < 0))
        throw std::invalid_argument("timeout must be a finite number >= 0 seconds, or None");
    std::unique_lock<std::mutex> lock(mutex_);
    const auto generation = generation_;
    auto available = [&] {
        auto it = frames_.find(index);
        return generation != generation_ || !error_.empty() || !running_ || stop_requested_ ||
            (it != frames_.end() && it->second.index > seen_[index]);
    };
    if (timeout) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::steady_clock::time_point::max() - now;
        const auto deadline = *timeout >= std::chrono::duration<double>(remaining).count()
            ? std::chrono::steady_clock::time_point::max()
            : now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(*timeout));
        if (!ready_.wait_until(lock, deadline, available)) throw ReadTimeout();
    } else ready_.wait(lock, available);
    if (generation != generation_) throw ReaderStopped();
    if (!error_.empty()) throw std::runtime_error(error_);
    auto found = frames_.find(index);
    if (found != frames_.end() && found->second.index > seen_[index]) {
        seen_[index] = found->second.index;
        return found->second;
    }
    throw ReaderStopped();
}
bool Reader::running() const { std::lock_guard<std::mutex> lock(mutex_); return running_; }
std::string Reader::error() const { std::lock_guard<std::mutex> lock(mutex_); return error_; }
}
