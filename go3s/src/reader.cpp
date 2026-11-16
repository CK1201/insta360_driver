#include "go3s/reader.hpp"
#include "go3s/adb.hpp"
#include <unistd.h>
#include <chrono>
#include <cmath>

namespace go3s {
Reader::Reader(const std::string& config) : config_(load_config(config)) {
    config_.preview = false;
}
Reader::~Reader() { stop(); }
Reader& Reader::start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(running_) return *this;
    }
    if(worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.reset(); error_.clear(); cursor_=0; next_index_=0;
        ++generation_; stop_requested_=false; running_=true;
    }
    try { worker_=std::thread(&Reader::run,this); }
    catch(...) { std::lock_guard<std::mutex> lock(mutex_); running_=false; ready_.notify_all(); throw; }
    return *this;
}
void Reader::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    { std::lock_guard<std::mutex> lock(mutex_); stop_requested_=true; }
    ready_.notify_all();
    if(worker_.joinable()) worker_.join();
}
void Reader::check_stream(int stream_index) {
    if(stream_index != 0) throw std::invalid_argument("GO3S has only stream_index 0");
}
std::optional<ReaderFrame> Reader::latest(int stream_index) {
    check_stream(stream_index);
    std::lock_guard<std::mutex> lock(mutex_);
    if(!error_.empty()) throw std::runtime_error(error_);
    if(latest_) cursor_=latest_->index;
    return latest_;
}
ReaderFrame Reader::read(std::optional<double> timeout,int stream_index) {
    check_stream(stream_index);
    if(timeout && (!std::isfinite(*timeout) || *timeout<0)) throw std::invalid_argument("timeout must be finite nonnegative seconds or None");
    std::unique_lock<std::mutex> lock(mutex_);
    const auto generation=generation_;
    auto available=[&]{return generation!=generation_ || !error_.empty() || (latest_ && latest_->index>cursor_) || !running_ || stop_requested_.load();};
    if(timeout) {
        const auto now=std::chrono::steady_clock::now();
        const auto remaining=std::chrono::steady_clock::time_point::max()-now;
        // Finite Python seconds can exceed the native clock range. Keep the
        // wait interruptible without overflowing chrono's integer duration.
        const auto deadline=*timeout>=std::chrono::duration<double>(remaining).count()
            ? std::chrono::steady_clock::time_point::max()
            : now+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(*timeout));
        if(!ready_.wait_until(lock,deadline,available)) throw ReaderTimeout("no new GO3S frame before timeout");
    } else ready_.wait(lock,available);
    if(generation!=generation_) throw ReaderEnded("GO3S capture session ended");
    if(!error_.empty()) throw std::runtime_error(error_);
    if(latest_ && latest_->index>cursor_) { cursor_=latest_->index; return *latest_; }
    throw ReaderEnded("GO3S capture ended");
}
bool Reader::running() const { std::lock_guard<std::mutex> lock(mutex_); return running_; }
std::optional<std::string> Reader::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_.empty()?std::nullopt:std::optional<std::string>(error_);
}
void Reader::publish(const Frame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_=ReaderFrame{frame.bgr,++next_index_,0,frame.host_receive_ns,frame.host_decode_ns,frame.timestamp_ns};
    ready_.notify_all();
}
void Reader::run() {
    int fd=-1; bool forwarded=false; std::string serial, failure;
    try {
        if(!stop_requested_) {
            serial=select_phone(config_.adb_serial);
            if(!stop_requested_) {
                forward_phone(serial,config_.port); forwarded=true;
                fd=connect_phone(config_.port);
                CaptureOptions options;
                options.on_frame=[this](const Frame& frame){publish(frame);};
                options.on_error=[this](const std::string& message) {
                    { std::lock_guard<std::mutex> lock(mutex_); if(error_.empty()) error_=message; }
                    ready_.notify_all();
                };
                options.monitor_stdin=false; options.allow_empty_stop=true;
                auto result=capture(fd,config_,[this]{return stop_requested_.load();},
                    DecodeMode::complete_h264_low_delay,options);
                if(!result.complete) failure=result.error;
            }
        }
    } catch(const std::exception& ex) { failure=ex.what(); }
    if(fd>=0) close(fd);
    if(forwarded) {
        try { remove_forward(serial,config_.port); }
        catch(const std::exception& ex) { if(failure.empty()) failure=ex.what(); }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_=std::move(failure); running_=false;
    }
    ready_.notify_all();
}
}
