#include "x5/camera.hpp"
#include "x5/preview.hpp"
#include <opencv2/highgui.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
void help() {
    std::cout << "insta360_x5_client " << X5_VERSION << " — YAML 配置、双鱼眼预览和六轴 IMU 采集\n"
        "用法: insta360_x5_client --config <yaml> [--duration <秒>] [--check-config]\n"
        "  --config <yaml>   配置文件；输出路径相对于该文件\n"
        "  --check-config    仅校验并显示有效配置，不访问相机或创建采集目录\n"
        "  --duration <秒>   覆盖 runtime.duration_s；0 表示持续运行\n"
        "  --help            显示帮助\n"
        "运行时按 q、关闭预览窗口或 Ctrl+C 停止。\n";
}
struct Arguments {
    fs::path config;
    bool check = false, help = false;
    std::optional<double> duration;
};
Arguments arguments(int argc, char** argv) {
    Arguments result;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--help") result.help = true;
        else if (flag == "--check-config") result.check = true;
        else if (flag == "--config" || flag == "--duration") {
            if (++i >= argc) throw std::runtime_error(flag + " 缺少参数");
            if (flag == "--config") result.config = argv[i];
            else {
                size_t consumed = 0;
                try { result.duration = std::stod(argv[i], &consumed); }
                catch (const std::exception&) { throw std::runtime_error("--duration 必须是秒数"); }
                if (consumed != std::string(argv[i]).size()) throw std::runtime_error("--duration 必须是秒数");
            }
        } else throw std::runtime_error("未知参数：" + flag);
    }
    if (!result.help && result.config.empty()) throw std::runtime_error("必须提供 --config <yaml>；使用 --help 查看帮助");
    return result;
}
int64_t unix_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
void write_yaml(const fs::path& path, const YAML::Node& value) {
    std::ofstream file;
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.open(path);
    file << value << '\n';
    file.flush();
    file.close();
}
fs::path make_run_directory(const fs::path& parent) {
    fs::create_directories(parent);
    const auto now_ns = unix_time_ns();
    std::time_t now = now_ns / 1000000000;
    std::tm calendar{};
    localtime_r(&now, &calendar);
    std::ostringstream name;
    name << std::put_time(&calendar, "%Y%m%d_%H%M%S") << '_'
         << std::setw(9) << std::setfill('0') << now_ns % 1000000000;
    auto directory = parent / name.str();
    if (!fs::create_directory(directory)) throw std::runtime_error("采集目录已存在：" + directory.string());
    return directory;
}
class StreamDelegate final : public ins_camera::StreamDelegate {
public:
    explicit StreamDelegate(std::shared_ptr<x5::CapturePipeline> pipeline) : pipeline_(std::move(pipeline)) {}
    void OnAudioData(const uint8_t*, size_t, int64_t) override {}
    void OnVideoData(const uint8_t* data, size_t size, int64_t timestamp, uint8_t, int index) override {
        const auto received = x5::steady_time_ns();
        try { pipeline_->video(data, size, timestamp, index, received); }
        catch (const std::exception& e) { pipeline_->fail(std::string("视频回调失败：") + e.what()); }
    }
    void OnGyroData(const std::vector<ins_camera::GyroData>& data) override {
        const auto received = x5::steady_time_ns();
        try {
            std::vector<x5::ImuSample> samples;
            samples.reserve(data.size());
            for (const auto& sample : data) samples.push_back({sample.timestamp, sample.ax, sample.ay,
                sample.az, sample.gx, sample.gy, sample.gz});
            pipeline_->imu(std::move(samples), received);
        } catch (const std::exception& e) { pipeline_->fail(std::string("IMU 回调失败：") + e.what()); }
    }
    void OnExposureData(const ins_camera::ExposureData& data) override {
        const auto received = x5::steady_time_ns();
        try { pipeline_->exposure({data.timestamp, data.exposure_time}, received); }
        catch (const std::exception& e) { pipeline_->fail(std::string("曝光回调失败：") + e.what()); }
    }
private:
    std::shared_ptr<x5::CapturePipeline> pipeline_;
};
struct DeviceStatus {
    std::optional<ins_camera::BatteryStatus> battery;
    std::optional<ins_camera::StorageStatus> storage;
};
class StatusMonitor {
public:
    StatusMonitor(ins_camera::Camera& camera, std::shared_ptr<x5::CapturePipeline> pipeline,
                  double interval, const fs::path& directory)
        : camera_(camera), pipeline_(std::move(pipeline)), interval_(interval) {
        if (!directory.empty()) {
            output_.exceptions(std::ios::badbit | std::ios::failbit);
            output_.open(directory / "status.csv");
            output_ << "host_receive_steady_ns,connected,battery_level,battery_scale,power_type,storage_state,free_space,total_space,decoded_fps,imu_samples_per_s,exposure_time_s\n";
        }
        worker_ = std::thread([this] { work(); });
    }
    ~StatusMonitor() { stop(); }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    DeviceStatus status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
private:
    void work() {
        try {
            auto previous_time = x5::steady_time_ns();
            uint64_t previous_frames = 0, previous_imu = 0;
            while (true) {
                { std::lock_guard<std::mutex> lock(mutex_); if (stopped_) break; }
                bool connected = camera_.IsConnected();
                if (!connected) { pipeline_->fail("X5 连接已断开"); break; }
                DeviceStatus value;
                ins_camera::BatteryStatus battery{};
                ins_camera::StorageStatus storage{};
                if (camera_.GetBatteryStatus(battery)) value.battery = battery;
                if (camera_.GetStorageState(storage)) value.storage = storage;
                if (output_.is_open()) {
                    auto now = x5::steady_time_ns();
                    auto stats = pipeline_->stats();
                    uint64_t frames = 0;
                    for (const auto& entry : stats.streams) frames += entry.second.frames;
                    double elapsed = (now - previous_time) / 1e9;
                    output_ << std::setprecision(17) << now << ',' << connected << ',';
                    if (value.battery) output_ << battery.battery_level << ',' << battery.battery_scale << ',' << static_cast<int>(battery.power_type);
                    else output_ << ",,";
                    output_ << ',';
                    if (value.storage) output_ << static_cast<int>(storage.state) << ',' << storage.free_space << ',' << storage.total_space;
                    else output_ << ",,";
                    output_ << ',' << (frames - previous_frames) / elapsed << ','
                            << (stats.imu_samples - previous_imu) / elapsed << ',';
                    if (stats.latest_exposure_s) output_ << *stats.latest_exposure_s;
                    output_ << '\n';
                    output_.flush();
                    previous_time = now;
                    previous_frames = frames;
                    previous_imu = stats.imu_samples;
                }
                std::unique_lock<std::mutex> lock(mutex_);
                status_ = value;
                if (wake_.wait_for(lock, std::chrono::duration<double>(interval_), [this] { return stopped_; })) break;
            }
        } catch (const std::exception& e) { pipeline_->fail(std::string("状态查询/保存失败：") + e.what()); }
        if (output_.is_open()) {
            try { output_.flush(); output_.close(); }
            catch (const std::exception& e) { pipeline_->fail(std::string("状态文件写盘失败：") + e.what()); }
        }
    }
    ins_camera::Camera& camera_;
    std::shared_ptr<x5::CapturePipeline> pipeline_;
    double interval_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_ = false;
    DeviceStatus status_;
    std::ofstream output_;
    std::thread worker_;
};
uint64_t frame_count(const x5::CaptureStats& stats) {
    uint64_t count = 0;
    for (const auto& entry : stats.streams) count += entry.second.frames;
    return count;
}
YAML::Node summary(const x5::CaptureStats& stats, const std::string& reason, double elapsed,
                   bool valid, x5::Codec codec, bool imu_enabled) {
    YAML::Node value;
    value["exit_reason"] = reason;
    value["capture_valid"] = valid;
    value["duration_s"] = elapsed;
    value["error"] = stats.error;
    value["imu_enabled"] = imu_enabled;
    value["imu_samples"] = stats.imu_samples;
    value["exposure_samples"] = stats.exposure_samples;
    value["exposure_metadata_missing"] = stats.exposure_samples == 0;
    value["hardware_sync_validated"] = false;
    value["streams"] = YAML::Node(YAML::NodeType::Sequence);
    for (const auto& entry : stats.streams) {
        YAML::Node stream;
        stream["stream_index"] = entry.first;
        stream["codec"] = x5::codec_name(codec);
        stream["packets"] = entry.second.packets;
        stream["bytes"] = entry.second.bytes;
        stream["decoded_frames"] = entry.second.frames;
        stream["width"] = entry.second.width;
        stream["height"] = entry.second.height;
        stream["startup_units_not_decoded"] = entry.second.startup_units_skipped;
        stream["preview_sps_patches"] = entry.second.sps_patches;
        stream["sdk_to_decoded_mean_ms"] = entry.second.timed_frames ?
            YAML::Node(entry.second.decode_latency_sum_ms / entry.second.timed_frames) : YAML::Node(YAML::NodeType::Null);
        stream["sdk_to_decoded_max_ms"] = entry.second.timed_frames ?
            YAML::Node(entry.second.decode_latency_max_ms) : YAML::Node(YAML::NodeType::Null);
        stream["native_size_verified"] = entry.second.native_size_seen;
        stream["native_size_mismatch_frames"] = entry.second.native_size_mismatch_frames;
        value["streams"].push_back(stream);
    }
    return value;
}
int run(const x5::Config& config) {
    if (config.preview && !std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        throw std::runtime_error("没有桌面显示环境；请在 YAML 设置 preview.enabled: false 后后台采集");
    }
    x5::CameraSession camera(config);
    auto readback = camera.configure(config);
    std::cout << "相机：\n" << camera.identity() << "\n参数读回：\n" << readback << '\n';
    fs::path directory;
    if (config.save) directory = make_run_directory(config.output_directory);
    std::shared_ptr<x5::CapturePipeline> pipeline;
    std::unique_ptr<StatusMonitor> monitor;
    std::set<int> windows;
    std::string reason = "initialization_error";
    int result = 0;
    int64_t started = x5::steady_time_ns();
    const auto codec = camera.codec();
    const bool low_delay = codec == x5::Codec::h264;
    try {
        if (config.save) {
            fs::copy_file(config.config_path, directory / "config_source.yaml");
            write_yaml(directory / "config_effective.yaml", config.to_yaml());
            YAML::Node session;
            session["application_version"] = X5_VERSION;
            session["identity"] = camera.identity();
            session["readback"] = readback;
            session["host_clock_anchor"]["steady_ns"] = x5::steady_time_ns();
            session["host_clock_anchor"]["unix_ns"] = unix_time_ns();
            session["timestamps"] = "Raw SDK device timestamps; host_receive_steady_ns is callback arrival, not exposure time";
            session["units"] = "Unconverted CameraSDK values. Device tick duration, acceleration/gyro units and axes require per-firmware validation; MediaSDK header units are not assumed to apply";
            session["physical_units_validated"] = false;
            session["preview_decode_mode"] = low_delay ? "x5_complete_h264_low_delay" : "byte_stream";
            session["imu_enabled"] = config.imu;
            write_yaml(directory / "session.yaml", session);
            std::cout << "保存目录：" << directory << '\n';
        }
        x5::CaptureOptions options;
        options.codec = codec;
        options.decode_mode = low_delay ? x5::DecodeMode::x5_complete_h264 : x5::DecodeMode::byte_stream;
        options.native_resolution = config.resolution;
        options.save = config.save;
        options.directory = directory;
        pipeline = std::make_shared<x5::CapturePipeline>(options);
        std::shared_ptr<ins_camera::StreamDelegate> delegate = std::make_shared<StreamDelegate>(pipeline);
        camera.camera().SetTemperatureHighNotification([pipeline] { pipeline->fail("相机报告温度过高，停止采集"); });
        camera.start(config, delegate);
        started = x5::steady_time_ns();
        monitor = std::make_unique<StatusMonitor>(camera.camera(), pipeline, config.status_interval_s, directory);
        std::cout << "开始实时采集；按 q、关闭窗口或 Ctrl+C 停止。\n";
        int64_t last_report = started;
        int64_t last_preview = 0;
        x5::PreviewContext preview_context{config, camera.identity(), readback, directory, codec};
        x5::PreviewLive preview_live;
        uint64_t previous_frames = 0, previous_imu = 0;
        std::map<int, std::pair<int, int>> reported_sizes;
        bool native_size_seen = false;
        reason = "duration";
        while (true) {
            const auto now = x5::steady_time_ns();
            const auto stats = pipeline->stats();
            for (const auto& entry : stats.streams) {
                auto size = std::make_pair(entry.second.width, entry.second.height);
                if (entry.second.native_size_seen) native_size_seen = true;
                if (entry.second.width && reported_sizes[entry.first] != size) {
                    reported_sizes[entry.first] = size;
                    std::cout << "实际预览 stream " << entry.first << ": " << entry.second.width << 'x'
                              << entry.second.height << "（每镜头请求 " << config.resolution << 'x' << config.resolution
                              << '@' << config.fps << "；SDK 合并请求 " << 2 * config.resolution << 'x'
                              << config.resolution << "，两个镜头均保留，以实际接收为准）\n";
                }
            }
            if (!stats.error.empty()) throw std::runtime_error(stats.error);
            if (interrupted) { reason = "signal"; break; }
            if (config.duration_s > 0 && (now - started) / 1e9 >= config.duration_s) break;
            constexpr int64_t timeout_ns = 10LL * 1000000000;
            if (now - started > timeout_ns && !native_size_seen) throw std::runtime_error("超过 10 秒未收到所设置的原生分辨率；不会以缩放替代");
            if (now - std::max(started, stats.last_video_received_ns) > timeout_ns) throw std::runtime_error("超过 10 秒未收到视频数据");
            if (now - started > timeout_ns && frame_count(stats) == 0) throw std::runtime_error("收到数据但超过 10 秒未解码出视频帧");
            if (config.imu && now - std::max(started, stats.last_imu_received_ns) > timeout_ns) throw std::runtime_error("超过 10 秒未收到已启用的 IMU 数据");
            if (now - last_report >= config.status_interval_s * 1e9) {
                double interval = (now - last_report) / 1e9;
                auto frames = frame_count(stats);
                auto device = monitor->status();
                preview_live.video_fps = (frames - previous_frames) / interval;
                preview_live.imu_hz = (stats.imu_samples - previous_imu) / interval;
                std::cout << std::fixed << std::setprecision(1)
                    << "视频 " << (frames - previous_frames) / interval << " fps | IMU "
                    << (stats.imu_samples - previous_imu) / interval << " samples/s | 曝光 ";
                if (stats.latest_exposure_s) std::cout << std::setprecision(6) << *stats.latest_exposure_s << " s";
                else std::cout << "暂无";
                std::cout << " | 电量 ";
                if (device.battery && device.battery->battery_scale) std::cout << std::setprecision(1)
                    << 100.0 * device.battery->battery_level / device.battery->battery_scale << '%';
                else if (device.battery) std::cout << device.battery->battery_level << "（原值，满量程未提供）";
                else std::cout << "未知";
                if (device.battery) std::cout << " | 电源 "
                    << (device.battery->power_type == ins_camera::PowerType::ADAPTER ? "外接" : "电池");
                if (device.storage) std::cout << " | 卡状态 " << static_cast<int>(device.storage->state)
                    << " 空间原值 " << device.storage->free_space << '/' << device.storage->total_space;
                else std::cout << " | 存储未知";
                std::cout << '\n';
                previous_frames = frames;
                previous_imu = stats.imu_samples;
                last_report = now;
            }
            bool closed = false;
            if (config.preview) {
                for (int index : windows) {
                    try {
                        if (cv::getWindowProperty("X5 stream " + std::to_string(index), cv::WND_PROP_VISIBLE) < 1) closed = true;
                    } catch (const cv::Exception&) {
                        // Some HighGUI backends destroy their receiver when the
                        // final window closes, instead of returning visible=0.
                        closed = true;
                    }
                }
                if (closed) { reason = "window_closed"; break; }
                if (now - last_preview >= 33333333) {
                    preview_live.capture = stats;
                    preview_live.elapsed_s = (now - started) / 1e9;
                    auto device = monitor->status();
                    preview_live.battery = "--";
                    preview_live.power = "--";
                    if (device.battery) {
                        preview_live.battery = device.battery->battery_scale ?
                            cv::format("%.1f%%", 100.0 * device.battery->battery_level / device.battery->battery_scale) :
                            std::to_string(device.battery->battery_level) + " (raw, scale unknown)";
                        preview_live.power = device.battery->power_type == ins_camera::PowerType::ADAPTER ? "External" : "Battery";
                    }
                    for (const auto& entry : pipeline->latest_frames()) {
                        preview_live.frame_received_ns = entry.second.received_ns;
                        preview_live.frame_decoded_ns = entry.second.decoded_ns;
                        std::string name = "X5 stream " + std::to_string(entry.first);
                        if (windows.insert(entry.first).second) {
                            cv::namedWindow(name, cv::WINDOW_NORMAL);
                            cv::resizeWindow(name, 1440, 820);
                        }
                        cv::imshow(name, x5::render_preview(entry.second.image, entry.first, preview_context, preview_live, now));
                    }
                    last_preview = now;
                }
                int key = cv::waitKey(10) & 0xff;
                if (key == 'q' || key == 'Q' || key == 27) { reason = "key"; break; }
            } else std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } catch (const std::exception& error) {
        reason = error.what();
        std::cerr << "采集失败：" << reason << '\n';
        result = 1;
    }
    if (monitor) monitor->stop();
    try { camera.stop(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; reason = error.what(); result = 1; }
    if (pipeline) pipeline->finish();
    if (config.preview) cv::destroyAllWindows();
    auto stats = pipeline ? pipeline->stats() : x5::CaptureStats{};
    if (!stats.error.empty()) { reason = stats.error; result = 1; }
    if (frame_count(stats) == 0 || (config.imu && stats.imu_samples == 0)) {
        if (result == 0) reason = "缺少可解码视频或已启用的 IMU 数据";
        result = 1;
    }
    for (const auto& entry : stats.streams) {
        if (entry.second.width != 2 * config.resolution || entry.second.height != config.resolution) {
            if (result == 0) reason = "结束时的实际原生分辨率与设置不一致";
            result = 1;
        }
    }
    if (stats.exposure_samples == 0) std::cerr << "警告：没有收到曝光元数据；已记入结束摘要。\n";
    double elapsed = (x5::steady_time_ns() - started) / 1e9;
    auto report = summary(stats, reason, elapsed, result == 0, codec, config.imu);
    std::cout << "采集结束：\n" << report << '\n';
    if (config.save) {
        try { write_yaml(directory / "summary.yaml", report); }
        catch (const std::exception& error) { std::cerr << "结束摘要写入失败：" << error.what() << '\n'; result = 1; }
    }
    return result;
}
}
int main(int argc, char** argv) {
    try {
        auto args = arguments(argc, argv);
        if (args.help) { help(); return 0; }
        auto config = x5::load_config(args.config);
        if (args.duration) config.duration_s = *args.duration;
        x5::validate_config(config);
        if (args.check) { std::cout << "配置校验通过（未访问相机）：\n" << config.to_yaml() << '\n'; return 0; }
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        return run(config);
    } catch (const std::exception& error) {
        std::cerr << "错误：" << error.what() << '\n';
        return 1;
    }
}
