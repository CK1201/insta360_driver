#include "x5/capture.hpp"
#include "x5/decoder.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <variant>
namespace x5 {
const char* codec_name(Codec c) { return c == Codec::h264 ? "h264" : "h265"; }
int64_t steady_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
struct VideoEvent {
    std::vector<uint8_t> bytes;
    int64_t timestamp;
    int index;
    int64_t received;
};
struct ImuEvent { std::vector<ImuSample> samples; int64_t received; };
struct ExposureEvent { ExposureSample sample; int64_t received; };
using Event = std::variant<VideoEvent, ImuEvent, ExposureEvent>;
struct QueuedEvent { Event event; size_t cost; };
void open_output(std::ofstream& file, const std::filesystem::path& path) {
    file.exceptions(std::ios::badbit | std::ios::failbit);
    try { file.open(path, std::ios::binary | std::ios::trunc); }
    catch (const std::exception& e) { throw std::runtime_error("无法打开输出 " + path.string() + ": " + e.what()); }
    file << std::setprecision(17);
}
}
struct CapturePipeline::Impl {
    CaptureOptions options;
    mutable std::mutex queue_mutex, data_mutex;
    std::condition_variable available;
    std::deque<QueuedEvent> queue;
    size_t queued_bytes = 0;
    bool closing = false;
    std::string error;
    std::thread worker;
    CaptureStats counters;
    std::map<int, PreviewFrame> images;
    std::ofstream packets_file, imu_file, exposure_file, frame_timings_file;
    struct Output { std::ofstream file; uint64_t offset = 0; };
    std::map<int, Output> videos;
    std::map<int, std::unique_ptr<VideoDecoder>> decoders;
    std::set<int> failed_decoders;
    uint64_t packet_sequence = 0;
    uint64_t imu_sequence = 0, exposure_sequence = 0;
    uint64_t frame_sequence = 0;

    explicit Impl(CaptureOptions value) : options(std::move(value)) {
        if (options.save) {
            open_output(packets_file, options.directory / "video_packets.csv");
            open_output(imu_file, options.directory / "imu.csv");
            open_output(exposure_file, options.directory / "exposure.csv");
            open_output(frame_timings_file, options.directory / "frame_timings.csv");
            packets_file << "packet_index,stream_index,device_timestamp,host_receive_steady_ns,file_offset,size_bytes\n";
            imu_file << "sample_index,device_timestamp,host_receive_steady_ns,ax,ay,az,gx,gy,gz\n";
            exposure_file << "sample_index,device_timestamp,host_receive_steady_ns,exposure_time_s\n";
            frame_timings_file << "frame_index,stream_index,host_receive_steady_ns,decode_done_steady_ns,timestamp_ns\n";
        }
        worker = std::thread([this] { work(); });
    }
    ~Impl() { finish(); }
    void fail(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (error.empty()) error = message;
            closing = true;
        }
        available.notify_one();
    }
    bool submit(Event event, size_t bytes, int64_t video_time = 0, int64_t imu_time = 0) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (closing) return false;
            if (bytes > options.queue_budget_bytes || queued_bytes > options.queue_budget_bytes - bytes) {
                error = "采集队列超过容量；停止采集，数据不完整";
                closing = true;
                available.notify_one();
                return false;
            }
            queue.push_back({std::move(event), bytes});
            queued_bytes += bytes;
            std::lock_guard<std::mutex> data_lock(data_mutex);
            if (video_time) counters.last_video_received_ns = video_time;
            if (imu_time) counters.last_imu_received_ns = imu_time;
        }
        available.notify_one();
        return true;
    }
    void process(const VideoEvent& event) {
        if (options.save) {
            auto& output = videos[event.index];
            if (!output.file.is_open()) open_output(output.file, options.directory /
                ("video_stream_" + std::to_string(event.index) + "." + codec_name(options.codec)));
            output.file.write(reinterpret_cast<const char*>(event.bytes.data()), event.bytes.size());
            packets_file << packet_sequence << ',' << event.index << ',' << event.timestamp << ','
                << event.received << ',' << output.offset << ',' << event.bytes.size() << '\n';
            output.offset += event.bytes.size();
        }
        ++packet_sequence;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            auto& stream = counters.streams[event.index];
            ++stream.packets;
            stream.bytes += event.bytes.size();
        }
        if (options.decode && !failed_decoders.count(event.index)) {
            try {
                auto& decoder = decoders[event.index];
                if (!decoder) decoder = std::make_unique<VideoDecoder>(options.codec, [this, index = event.index](PreviewFrame image) {
                    image.timestamp_ns = image.received_ns ? image.received_ns - options.sensor_delay_ns : 0;
                    if (options.save) frame_timings_file << frame_sequence << ',' << index << ','
                        << image.received_ns << ',' << image.decoded_ns << ',' << image.timestamp_ns << '\n';
                    ++frame_sequence;
                    bool drift = false;
                    int width = image.image.cols, height = image.image.rows;
                    {
                        std::lock_guard<std::mutex> lock(data_mutex);
                        auto& stream = counters.streams[index];
                        ++stream.frames;
                        stream.width = width;
                        stream.height = height;
                        if (options.native_resolution > 0) {
                            bool matches = width == 2 * options.native_resolution && height == options.native_resolution;
                            if (matches) stream.native_size_seen = true;
                            else if (stream.native_size_seen) {
                                ++stream.native_size_mismatch_frames;
                                drift = true;
                            }
                        }
                        if (image.received_ns > 0 && image.decoded_ns >= image.received_ns) {
                            double elapsed_ms = (image.decoded_ns - image.received_ns) / 1e6;
                            ++stream.timed_frames;
                            stream.decode_latency_sum_ms += elapsed_ms;
                            stream.decode_latency_max_ms = std::max(stream.decode_latency_max_ms, elapsed_ms);
                        }
                        images[index] = image;
                    }
                    // submit() takes queue_mutex then data_mutex: release the
                    // data lock before fail() takes the queue lock.
                    if (drift) fail("原生分辨率在确认后发生偏离：" + std::to_string(width) + "x" +
                        std::to_string(height) + "；原始数据保留，采集判为失败");
                    if (!drift && options.on_frame && (options.native_resolution == 0 ||
                        (width == 2 * options.native_resolution && height == options.native_resolution)))
                        options.on_frame(index, image);
                }, options.decode_mode);
                decoder->feed(event.bytes.data(), event.bytes.size(), event.received);
                snapshot_decoder(event.index, *decoder);
            } catch (const std::exception& e) {
                auto failed = decoders.find(event.index);
                if (failed != decoders.end() && failed->second) {
                    snapshot_decoder(event.index, *failed->second);
                }
                decoders.erase(event.index);
                failed_decoders.insert(event.index);
                // Stop intake, but keep writing every already accepted event.
                // A broken preview decoder does not make raw storage unwritable.
                fail(std::string("视频解码失败：") + e.what());
            }
        }
    }
    void snapshot_decoder(int index, const VideoDecoder& decoder) {
        std::lock_guard<std::mutex> lock(data_mutex);
        auto& stream = counters.streams[index];
        stream.startup_units_skipped = decoder.startup_units_skipped();
        stream.sps_patches = decoder.sps_patches();
    }
    void process(const ImuEvent& event) {
        for (const auto& sample : event.samples) {
            if (options.save) imu_file << imu_sequence << ',' << sample.timestamp << ',' << event.received << ','
                << sample.ax << ',' << sample.ay << ',' << sample.az << ','
                << sample.gx << ',' << sample.gy << ',' << sample.gz << '\n';
            ++imu_sequence;
        }
        std::lock_guard<std::mutex> lock(data_mutex);
        counters.imu_samples += event.samples.size();
        counters.latest_imu = event.samples.back();
        counters.latest_imu_received_ns = event.received;
    }
    void process(const ExposureEvent& event) {
        if (options.save) exposure_file << exposure_sequence << ',' << event.sample.timestamp << ','
            << event.received << ',' << event.sample.exposure_time << '\n';
        ++exposure_sequence;
        std::lock_guard<std::mutex> lock(data_mutex);
        ++counters.exposure_samples;
        counters.latest_exposure_s = event.sample.exposure_time;
    }
    void flush_file(std::ofstream& file) {
        if (!file.is_open()) return;
        try { file.flush(); file.close(); }
        catch (const std::exception& e) { fail(std::string("采集文件写盘失败：") + e.what()); }
    }
    void work() {
        try {
            while (true) {
                std::optional<QueuedEvent> item;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    available.wait(lock, [this] { return closing || !queue.empty(); });
                    if (queue.empty()) break;
                    item.emplace(std::move(queue.front()));
                    queue.pop_front();
                    queued_bytes -= item->cost;
                }
                std::visit([this](const auto& event) { process(event); }, item->event);
            }
            for (auto& entry : decoders) {
                try { entry.second->finish(); }
                catch (const std::exception& e) { fail(std::string("视频解码刷新失败：") + e.what()); }
                snapshot_decoder(entry.first, *entry.second);
            }
        } catch (const std::exception& e) {
            fail(std::string("采集保存/解码失败：") + e.what());
        }
        for (auto& entry : videos) flush_file(entry.second.file);
        flush_file(packets_file);
        flush_file(imu_file);
        flush_file(exposure_file);
        flush_file(frame_timings_file);
    }
    void finish() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            closing = true;
        }
        available.notify_one();
        if (worker.joinable()) worker.join();
    }
};
CapturePipeline::CapturePipeline(CaptureOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
CapturePipeline::~CapturePipeline() = default;
bool CapturePipeline::video(const uint8_t* data, size_t size, int64_t ts, int index, int64_t received) {
    if (!size) return true;
    if (!data || index < 0) { fail("无效视频回调"); return false; }
    VideoEvent event{{}, ts, index, received};
    event.bytes.assign(data, data + size);
    return impl_->submit(std::move(event), size + sizeof(VideoEvent), received);
}
bool CapturePipeline::imu(std::vector<ImuSample> samples, int64_t received) {
    if (samples.empty()) return true;
    size_t size = samples.size() * sizeof(ImuSample) + sizeof(ImuEvent);
    return impl_->submit(ImuEvent{std::move(samples), received}, size, 0, received);
}
bool CapturePipeline::exposure(ExposureSample sample, int64_t received) {
    return impl_->submit(ExposureEvent{sample, received}, sizeof(ExposureEvent));
}
void CapturePipeline::fail(const std::string& error) { impl_->fail(error); }
void CapturePipeline::finish() { impl_->finish(); }
CaptureStats CapturePipeline::stats() const {
    CaptureStats result;
    { std::lock_guard<std::mutex> lock(impl_->data_mutex); result = impl_->counters; }
    { std::lock_guard<std::mutex> lock(impl_->queue_mutex); result.error = impl_->error; }
    return result;
}
std::map<int, PreviewFrame> CapturePipeline::latest_frames() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex);
    return impl_->images;
}
}
