#include "x5/camera.hpp"
#include <camera/device_discovery.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace x5 {
namespace {
using namespace ins_camera;
// The normal-video table supplies sensor exposure limits. Its live table omits
// ISO/shutter controls, although the public live setters and readback work on X5.
constexpr auto validation_mode = CameraFunctionMode::FUNCTION_MODE_NORMAL_VIDEO;
constexpr auto settings_mode = CameraFunctionMode::FUNCTION_MODE_LIVE_STREAM;
struct Mode { const char* name; const char* capability; PhotographyOptions_ExposureMode value; };
const Mode modes[] = {
    {"auto", "AUTO", AUTO}, {"full_auto", "FULL_AUTO", FULL_AUTO},
    {"manual", "MANUAL", MANUAL}, {"iso_priority", "ISO_PRIORITY", ISO_PRIORITY},
    {"shutter_priority", "SHUTTER_PRIORITY", SHUTTER_PRIORITY}
};
const Mode& exposure_mode(const std::string& name) {
    for (const auto& mode : modes) if (name == mode.name) return mode;
    throw std::runtime_error("invalid exposure mode: " + name);
}
std::string mode_name(PhotographyOptions_ExposureMode value) {
    for (const auto& mode : modes) if (value == mode.value) return mode.name;
    return "sdk_mode_" + std::to_string(static_cast<int>(value));
}
double number(const std::string& value) {
    auto slash = value.find('/');
    if (slash != std::string::npos) return std::stod(value.substr(0, slash)) / std::stod(value.substr(slash + 1));
    return std::stod(value);
}
bool close(double actual, double wanted) {
    return std::isfinite(actual) && std::abs(actual - wanted) <= std::max(1e-7, std::abs(wanted) * 1e-4);
}
std::vector<std::string> capability(Camera& camera, const std::string& key) {
    auto values = camera.GetSupportedAttrValues(validation_mode, key);
    if (values.empty()) throw std::runtime_error("相机能力表缺少 " + key + "，请检查 SDK 与 build/jsons 链接");
    return values;
}
void check_number(Camera& camera, const std::string& key, double requested) {
    auto values = capability(camera, key);
    for (const auto& value : values) if (close(number(value), requested)) return;
    throw std::runtime_error("相机能力表不支持 " + key + "=" + std::to_string(requested));
}
void check_usb_permissions() {
    // Discovery suppresses inaccessible devices, so identify the known Linux
    // permission failure before it becomes a misleading "no camera" message.
    for (const auto& entry : std::filesystem::directory_iterator("/sys/bus/usb/devices")) {
        std::ifstream vendor_file(entry.path() / "idVendor");
        std::string vendor;
        if (!(vendor_file >> vendor) || vendor != "2e1a") continue;
        int bus = 0, device = 0;
        std::ifstream bus_file(entry.path() / "busnum"), device_file(entry.path() / "devnum");
        if (!(bus_file >> bus) || !(device_file >> device)) continue;
        std::ostringstream path;
        path << "/dev/bus/usb/" << std::setw(3) << std::setfill('0') << bus
             << '/' << std::setw(3) << device;
        if (::access(path.str().c_str(), R_OK | W_OK) != 0) {
            throw std::runtime_error("USB 访问权限不足：" + path.str() +
                "。相机已枚举，请按 commands.md 配置 X5 的 USB 权限后重试。");
        }
    }
}
VideoResolution preview_resolution(const Config& config) {
    // SDK names the combined 2:1 transport image; YAML names each square lens.
    return static_cast<VideoResolution>(config.sdk_resolution_enum());
}
}
CameraSession::CameraSession(const Config& config) {
    check_usb_permissions();
    ins_camera::SetLogLevel(ins_camera::LogLevel::WARNING);
    ins_camera::DeviceDiscovery discovery;
    auto devices = discovery.GetAvailableDevices();
    try {
        std::vector<size_t> matches;
        for (size_t i = 0; i < devices.size(); ++i) {
            if (devices[i].camera_type == ins_camera::CameraType::Insta360X5 &&
                (config.serial.empty() || devices[i].serial_number == config.serial)) matches.push_back(i);
        }
        if (matches.empty()) throw std::runtime_error("没有找到可访问的目标 X5；检查 USB/Android 模式、序列号和 USB 权限");
        if (matches.size() != 1) throw std::runtime_error("连接了多台 X5，请在 camera.serial 中指定序列号");
        const auto& descriptor = devices[matches.front()];
        model_ = descriptor.camera_name;
        serial_ = descriptor.serial_number;
        firmware_ = descriptor.fw_version;
        camera_ = std::make_shared<ins_camera::Camera>(descriptor.info);
        camera_->SetTimeout(3000);
        if (!camera_->Open()) throw std::runtime_error("打开 X5 会话失败");
    } catch (...) {
        if (camera_) camera_->Close();
        discovery.FreeDeviceDescriptors(devices);
        throw;
    }
    discovery.FreeDeviceDescriptors(devices);
}
CameraSession::~CameraSession() {
    try { stop(); } catch (const std::exception& e) { std::cerr << "停止预览失败：" << e.what() << '\n'; }
    if (camera_) camera_->Close();
}
YAML::Node CameraSession::identity() const {
    YAML::Node value;
    value["model"] = model_;
    value["serial"] = serial_;
    value["firmware"] = firmware_;
    value["sdk_version"] = ins_camera::GetSDKVersion();
    return value;
}
Codec CameraSession::codec() const {
    return camera_->GetVideoEncodeType() == ins_camera::VideoEncodeType::H264 ? Codec::h264 : Codec::h265;
}
YAML::Node CameraSession::configure(const Config& config) {
    if (!camera_->SetVideoSubMode(ins_camera::SubVideoMode::VIDEO_LIVEVIEW)) throw std::runtime_error("相机拒绝切换原生尺寸可控的直播预览模式");
    if (!camera_->SetActiveSensor(ins_camera::SensorDevice::SENSOR_DEVICE_ALL)) throw std::runtime_error("相机拒绝双镜头模式");
    ins_camera::RecordParams native{};
    native.resolution = preview_resolution(config);
    if (!camera_->SetVideoCaptureParams(native, settings_mode)) throw std::runtime_error("相机拒绝原生预览分辨率设置");
    const auto& requested_mode = exposure_mode(config.exposure_mode);
    auto values = capability(*camera_, "exposure_program");
    if (std::find(values.begin(), values.end(), requested_mode.capability) == values.end()) {
        throw std::runtime_error("相机能力表不支持曝光模式 " + config.exposure_mode);
    }
    if (config.iso) check_number(*camera_, "exposure_iso", *config.iso);
    if (config.shutter_s) check_number(*camera_, "exposure_shutter_speed", *config.shutter_s);
    if (config.exposure_mode == "auto" || config.exposure_mode == "full_auto") check_number(*camera_, "exposure_bias", config.ev);
    check_number(*camera_, "white_balance", config.white_balance_kelvin);

    auto exposure = camera_->GetExposureSettings(settings_mode);
    if (!exposure) throw std::runtime_error("读取曝光参数失败");
    exposure->SetExposureMode(requested_mode.value);
    if (config.iso) exposure->SetIso(*config.iso);
    if (config.shutter_s) exposure->SetShutterSpeed(*config.shutter_s);
    if (config.exposure_mode == "auto" || config.exposure_mode == "full_auto") exposure->SetEVBias(config.ev_bias());
    if (!camera_->SetExposureSettings(settings_mode, exposure)) {
        throw std::runtime_error("相机拒绝曝光参数：" + YAML::Dump(config.to_yaml()["camera"]["exposure"]) +
            "；检查当前拍摄状态和 p3_switch 等能力依赖，不自动替换配置");
    }
    auto capture = camera_->GetCaptureSettings(settings_mode);
    if (!capture) throw std::runtime_error("读取白平衡参数失败");
    capture->SetValue(ins_camera::CaptureSettings::CaptureSettings_WhiteBalance, config.white_balance_kelvin);
    if (!camera_->SetCaptureSettings(settings_mode, capture)) throw std::runtime_error("相机拒绝 white_balance_kelvin=" + std::to_string(config.white_balance_kelvin));

    if (!camera_->SyncPhotographyOptions(settings_mode)) throw std::runtime_error("无法刷新参数读回；不将 SDK 缓存当作设置成功");
    exposure = camera_->GetExposureSettings(settings_mode);
    capture = camera_->GetCaptureSettings(settings_mode);
    if (!exposure || !capture) throw std::runtime_error("参数读回失败");
    if (exposure->ExposureMode() != requested_mode.value) throw std::runtime_error("曝光模式读回不一致：请求 " + config.exposure_mode + "，实际 " + mode_name(exposure->ExposureMode()));
    if (config.iso && exposure->Iso() != *config.iso) throw std::runtime_error("ISO 读回不一致");
    if (config.shutter_s && !close(exposure->ShutterSpeed(), *config.shutter_s)) throw std::runtime_error("shutter_s 读回不一致");
    if ((config.exposure_mode == "auto" || config.exposure_mode == "full_auto") && exposure->EVBias() != config.ev_bias()) throw std::runtime_error("EV 读回不一致");
    auto wb = capture->GetIntValue(ins_camera::CaptureSettings::CaptureSettings_WhiteBalance);
    if (wb != config.white_balance_kelvin) throw std::runtime_error("白平衡读回不一致");
    int actual_resolution = capture->GetIntValue(ins_camera::CaptureSettings::CaptureSettings_RecordResolution);
    const bool matches_request = actual_resolution == static_cast<int>(native.resolution);
    if (!matches_request) throw std::runtime_error("原生分辨率/FPS读回不一致：请求30fps枚举 " +
        std::to_string(static_cast<int>(native.resolution)) + "，实际 " + std::to_string(actual_resolution));

    YAML::Node result;
    result["exposure"]["mode"] = mode_name(exposure->ExposureMode());
    result["exposure"]["iso"] = exposure->Iso();
    result["exposure"]["shutter_s"] = exposure->ShutterSpeed();
    result["exposure"]["ev"] = exposure->EVBias() / 20.0;
    result["white_balance_kelvin"] = wb;
    result["preview"]["control_mode"] = "VIDEO_LIVEVIEW / FUNCTION_MODE_LIVE_STREAM";
    result["preview"]["requested_native_resolution"] = std::to_string(2 * config.resolution) + "x" +
        std::to_string(config.resolution) + "@" + std::to_string(config.fps);
    auto actual_name = camera_->GetVideoResolutionName(static_cast<ins_camera::VideoResolution>(actual_resolution));
    auto first = actual_name.find('_');
    auto second = first == std::string::npos ? first : actual_name.find('_', first + 1);
    if (second != std::string::npos) { actual_name[first] = 'x'; actual_name[second] = '@'; }
    else actual_name = "SDK enum " + std::to_string(actual_resolution);
    result["preview"]["native_resolution"] = actual_name;
    result["preview"]["native_resolution_enum"] = actual_resolution;
    result["preview"]["requested_native_resolution_enum"] = static_cast<int>(native.resolution);
    result["preview"]["native_resolution_matches_request"] = matches_request;
    auto preview = camera_->GetPreviewParam();
    result["preview"]["codec"] = codec_name(codec());
    result["preview"]["delay_timestamp"] = preview.delay_timestamp;
    result["preview"]["sweep_time"] = preview.sweep_time;
    result["preview"]["acceleration_range"] = preview.acceleration_range;
    result["preview"]["gyro_range"] = preview.gyro_range;
    for (size_t i = 0; i < preview.GetCalibrationCount(); ++i) result["preview"]["calibration"].push_back(preview.GetCalibration(i));
    return result;
}
void CameraSession::start(const Config& config, std::shared_ptr<ins_camera::StreamDelegate>& delegate) {
    camera_->SetStreamDelegate(delegate);
    ins_camera::LiveStreamParam parameters{};
    parameters.video_resolution = preview_resolution(config);
    parameters.lrv_video_resulution = parameters.video_resolution;
    parameters.video_bitrate = config.bitrate_bps;
    parameters.enable_video = true;
    parameters.enable_audio = false;
    parameters.enable_gyro = config.imu;
    parameters.using_lrv = false;
    parameters.is_for_live = true;
    if (!camera_->StartLiveStreaming(parameters)) throw std::runtime_error("X5 拒绝启动预览流，请检查请求分辨率及相机状态");
    streaming_ = true;
}
void CameraSession::stop() {
    if (!streaming_) return;
    bool ok = camera_->StopLiveStreaming();
    streaming_ = false;
    if (!ok) {
        // Closing the USB session also prevents callbacks from outliving shutdown.
        camera_->Close();
        throw std::runtime_error("StopLiveStreaming 返回失败，已关闭相机会话");
    }
}
}
