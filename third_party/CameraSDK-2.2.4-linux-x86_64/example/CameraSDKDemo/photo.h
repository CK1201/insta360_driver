// photo.h — photo sub-module for CameraSDKDemo
#pragma once
#include "../common/camera_setup.h"
#include "../common/file_utils.h"
#include "../common/i18n.h"
#include "demo_util.h"
#include <iostream>
#include <set>
#include <string>
#include <vector>

extern std::shared_ptr<ins_camera::Camera> g_cam;
extern ins_camera::CameraType g_cameraType;

namespace photo {

using ins_camera::CameraFunctionMode;
using ins_camera::SubPhotoMode;

struct PhotoModeDesc {
    const char* name;
    const char* en_name;
    SubPhotoMode psub;
    CameraFunctionMode fmode;
    bool is_timelapse;
    bool is_hdr;
};
static const std::vector<PhotoModeDesc> kPhotoModes = {
    {"普通拍照", "single photo", SubPhotoMode::PHOTO_SINGLE, CameraFunctionMode::FUNCTION_MODE_NORMAL_IMAGE, false,
     false},
    {"HDR拍照", "HDR photo", SubPhotoMode::PHOTO_HDR, CameraFunctionMode::FUNCTION_MODE_HDR_IMAGE, false, true},
    {"间隔拍照", "interval photo", SubPhotoMode::PHOTO_INTERVAL, CameraFunctionMode::FUNCTION_MODE_INTERVAL_SHOOTING,
     true, false},
    {"连拍", "burst photo", SubPhotoMode::PHOTO_BURST, CameraFunctionMode::FUNCTION_MODE_BURST, false, false},
    {"星空拍照", "starlapse", SubPhotoMode::PHOTO_STARLAPSE, CameraFunctionMode::FUNCTION_MODE_STARLAPSE_IMAGE, true,
     false},
};
// Virtual HDR mode for cameras that use hdr_photo_mode attr on PHOTO_SINGLE (X4Air/X5/X6)
static const PhotoModeDesc kVirtualHdrMode = {
    "HDR拍照", "HDR photo", SubPhotoMode::PHOTO_SINGLE, CameraFunctionMode::FUNCTION_MODE_NORMAL_IMAGE, false, true};

struct PhotoConfig {
    const PhotoModeDesc* mode = nullptr;
    ins_camera::PhotoSize photo_size = ins_camera::PhotoSize::Size_11968_5984;
    std::string raw_type = "PURESHOT";
    int self_timer = 0;
    std::string lapse_time;
    std::string record_duration;
    std::string hdr_mode;
    int aeb_num = 3;
    double aeb_ev = 1.0;
    bool has_self_timer = false;
    bool has_lapse_time = false;
    bool has_duration = false;
    bool has_raw = false;
    bool has_hdr = false;
};
static PhotoConfig g_cfg;
bool g_capturing = false;
static bool g_initialized = false;

static void populateDefaults();

static void autoDetectMode() {
    if (g_initialized && g_cfg.mode)
        return;
    g_initialized = true;
    auto fmode = g_cam->GetCurrentFunctionMode();
    for (auto& m : kPhotoModes) {
        if (m.fmode == fmode) {
            g_cfg.mode = &m;
            populateDefaults();
            return;
        }
    }
}

static const char* photoSizeName(ins_camera::PhotoSize sz) {
    switch (sz) {
    case ins_camera::PhotoSize::Size_15520_7760: return "120MP (15520x7760)";
    case ins_camera::PhotoSize::Size_11968_5984: return "72MP (11968x5984)";
    case ins_camera::PhotoSize::Size_7776_3888: return "30MP (7776x3888)";
    case ins_camera::PhotoSize::Size_7680_3840: return "29MP (7680x3840)";
    case ins_camera::PhotoSize::Size_5952_2976: return "18MP (5952x2976)";
    case ins_camera::PhotoSize::Size_6912_3456: return "18MP (6912x3456)";
    default: return "";
    }
}

static void populateDefaults() {
    if (!g_cfg.mode)
        return;
    auto fmode = g_cfg.mode->fmode;

    auto sizes = g_cam->GetSupportedPhotoSizes(fmode);
    if (!sizes.empty())
        g_cfg.photo_size = sizes[0];

    auto rawVals = g_cam->GetSupportedAttrValues(fmode, "raw_capture_type");
    g_cfg.has_raw = !rawVals.empty();
    if (g_cfg.has_raw)
        g_cfg.raw_type = rawVals[0];

    auto timerVals = g_cam->GetSupportedAttrValues(fmode, "photography_self_timer");
    g_cfg.has_self_timer = !timerVals.empty();

    auto lapseVals = g_cam->GetSupportedAttrValues(fmode, "lapse_time");
    g_cfg.has_lapse_time = !lapseVals.empty();
    if (g_cfg.has_lapse_time)
        g_cfg.lapse_time = lapseVals[0];

    // 拍照延时类(星空延时/间隔拍照)的 record_duration = 拍摄总时长(到点自停)。
    auto durVals = g_cam->GetSupportedAttrValues(fmode, "record_duration");
    g_cfg.has_duration = !durVals.empty();
    if (g_cfg.has_duration)
        g_cfg.record_duration = durVals[0];

    if (g_cfg.mode->is_hdr) {
        auto hdrVals = g_cam->GetSupportedAttrValues(fmode, "hdr_photo_mode");
        // 能力表第一项通常是 "OFF"，只有存在非 OFF 的取值才真正有 HDR 可开；
        // 否则 has_hdr 应为 false，避免「选了 HDR 模式反而下发 OFF」。
        g_cfg.hdr_mode.clear();
        for (const auto& v : hdrVals)
            if (v != "OFF") {
                g_cfg.hdr_mode = v;
                break;
            }
        g_cfg.has_hdr = !g_cfg.hdr_mode.empty();
    }
}

static void printConfig() {
    if (!g_cfg.mode) {
        std::cout << demo_common::L("(未选模式)", "(no mode)") << std::endl;
        return;
    }
    std::cout << demo_common::L(g_cfg.mode->name, g_cfg.mode->en_name) << " | "
              << demo_common::L("默认尺寸", "Default size") << ": " << photoSizeName(g_cfg.photo_size);
    if (g_cfg.has_raw)
        std::cout << " | " << demo_common::L("格式", "fmt") << ": " << g_cfg.raw_type;
    if (g_cfg.has_self_timer)
        std::cout << " | " << demo_common::L("倒计时", "timer") << ": " << g_cfg.self_timer << "s";
    if (g_cfg.has_lapse_time)
        std::cout << " | " << demo_common::L("间隔", "interval") << ": " << g_cfg.lapse_time << "s";
    if (g_cfg.has_duration)
        std::cout << " | " << demo_common::L("时长", "duration") << ": "
                  << demo_util::durationLabel(g_cfg.record_duration);
    if (g_cfg.has_hdr)
        std::cout << " | " << demo_common::L("HDR", "HDR") << ": " << g_cfg.hdr_mode << " | AEB " << g_cfg.aeb_num
                  << "x" << g_cfg.aeb_ev;
    std::cout << std::endl;
}

static ins_camera::CameraTimelapseMode currentTimelapseMode() {
    if (g_cfg.mode && g_cfg.mode->fmode == CameraFunctionMode::FUNCTION_MODE_INTERVAL_SHOOTING)
        return ins_camera::CameraTimelapseMode::TIMELAPSE_INTERVAL_SHOOTING;
    if (g_cfg.mode && g_cfg.mode->fmode == CameraFunctionMode::FUNCTION_MODE_STARLAPSE_IMAGE)
        return ins_camera::CameraTimelapseMode::TIMELAPSE_STARLAPSE_SHOOTING;
    return ins_camera::CameraTimelapseMode::MOBILE_TIMELAPSE_VIDEO;
}

// 设置即下发：延时类(间隔拍照/星空延时)的间隔走 SetTimeLapseOption，
// 拍摄时长走 SetCaptureDuration。
static bool applyTimelapseParams() {
    if (!g_cfg.mode || !g_cfg.mode->is_timelapse)
        return false;
    if (!demo_util::applyTimelapseParams(g_cam, currentTimelapseMode(), g_cfg.lapse_time, g_cfg.record_duration))
        return false;
    if (g_cfg.has_duration && !g_cfg.record_duration.empty()
        && !g_cam->SetCaptureDuration(g_cfg.mode->fmode,
                                      static_cast<uint32_t>(std::atof(g_cfg.record_duration.c_str())))) {
        std::cerr << demo_common::L("下发拍摄时长失败", "failed to apply capture duration") << std::endl;
        return false;
    }
    std::cout << demo_common::L("已下发到相机", "Applied to camera") << std::endl;
    return true;
}

// 能力表里 hdr_photo_mode 的取值名 → 协议枚举，必须查表，不能用菜单下标当枚举值。
static bool setHdrModeByName(const std::string& name) {
    const int v = g_cam->GetAttrValueByName("hdr_photo_mode", name);
    if (v < 0) {
        std::cerr << demo_common::L("未知HDR模式", "unknown HDR mode") << ": " << name << std::endl;
        return false;
    }
    return g_cam->SetPhotoHdrMode(g_cfg.mode->fmode, static_cast<ins_camera::PhotoHdrType>(v));
}

// --- menu commands ---
static void cmdSelectMode() {
    auto supported = g_cam->GetSupportedPhotoModes();
    std::vector<const PhotoModeDesc*> cand;
    if (supported.empty()) {
        for (auto& m : kPhotoModes)
            cand.push_back(&m);
    } else {
        std::set<int> supported_set;
        for (auto fm : supported)
            supported_set.insert(static_cast<int>(fm));
        for (auto& m : kPhotoModes)
            if (supported_set.count(static_cast<int>(m.fmode)))
                cand.push_back(&m);
        // If PHOTO_SINGLE has hdr_photo_mode, add virtual HDR mode (X4Air/X5/X6)
        if (supported_set.count(static_cast<int>(CameraFunctionMode::FUNCTION_MODE_NORMAL_IMAGE))) {
            auto hdrVals =
                g_cam->GetSupportedAttrValues(CameraFunctionMode::FUNCTION_MODE_NORMAL_IMAGE, "hdr_photo_mode");
            // 只有存在非 OFF 的取值才展示虚拟 HDR 入口，否则选了只能下发 OFF。
            bool has_hdr_on = false;
            for (const auto& v : hdrVals)
                if (v != "OFF") {
                    has_hdr_on = true;
                    break;
                }
            if (has_hdr_on)
                cand.push_back(&kVirtualHdrMode);
        }
    }
    if (cand.empty()) {
        std::cout << demo_common::L("无可用拍照模式", "no photo modes") << std::endl;
        return;
    }
    std::cout << demo_common::L("可选拍照模式", "Available photo modes") << ":" << std::endl;
    for (size_t i = 0; i < cand.size(); ++i)
        std::cout << "  [" << i << "] " << demo_common::L(cand[i]->name, cand[i]->en_name) << std::endl;
    int idx = 0;
    std::cout << demo_common::L("选择", "choose") << ": ";
    if (!(std::cin >> idx) || idx < 0 || idx >= (int)cand.size()) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    // 切换失败就不能把模式记为已选：后续尺寸/参数都按该模式下发，
    // 相机实际停在别的模式，启动拍摄会以无效状态失败。
    if (!g_cam->SetPhotoSubMode(cand[idx]->psub)) {
        std::cerr << demo_common::L("切换子模式失败", "failed to switch submode") << std::endl;
        return;
    }
    g_cfg = PhotoConfig{};
    g_cfg.mode = cand[idx];
    populateDefaults();
    // If virtual HDR mode, auto-select the first HDR-enabled value.
    // 能力表 hdr_photo_mode 的第一项是 "OFF"，直接取 [0] 会让「选了HDR模式反而关掉HDR」。
    // 虚拟 HDR 模式(X4Air/X5/X6 在 PHOTO_SINGLE 上用 hdr_photo_mode 表达 HDR)：
    // populateDefaults 已挑好第一个非 OFF 的取值，这里把它下发。
    if (cand[idx] == &kVirtualHdrMode) {
        if (g_cfg.has_hdr && !g_cfg.hdr_mode.empty()) {
            if (!setHdrModeByName(g_cfg.hdr_mode))
                std::cerr << demo_common::L("设置HDR模式失败", "Failed to set HDR mode") << std::endl;
        } else {
            std::cout << demo_common::L("该相机无可用HDR取值", "no HDR value available") << std::endl;
        }
    }
    std::cout << demo_common::L("已选", "Selected") << ": " << demo_common::L(cand[idx]->name, cand[idx]->en_name)
              << std::endl;
    // 选完模式即把该模式的默认参数下发，保证相机侧与 printConfig 显示一致。
    g_cam->SetPhotoSize(g_cfg.mode->fmode, g_cfg.photo_size);
    if (g_cfg.has_raw) {
        int rawType = g_cam->GetAttrValueByName("raw_capture_type", g_cfg.raw_type);
        if (rawType >= 0)
            g_cam->SetRawCaptureType(g_cfg.mode->fmode, static_cast<ins_camera::RawCaptureType>(rawType));
    }
    if (g_cfg.mode->is_timelapse)
        applyTimelapseParams();
    printConfig();
}

static void cmdSetPhotoSize() {
    if (!g_cfg.mode) {
        std::cout << demo_common::L("请先选模式", "select mode first") << std::endl;
        return;
    }
    auto sizes = g_cam->GetSupportedPhotoSizes(g_cfg.mode->fmode);
    if (sizes.empty()) {
        std::cout << demo_common::L("无可用尺寸", "no sizes") << std::endl;
        return;
    }
    std::cout << demo_common::L("可选尺寸", "Available sizes") << ":" << std::endl;
    for (size_t i = 0; i < sizes.size(); ++i)
        std::cout << "  [" << i << "] " << photoSizeName(sizes[i]) << std::endl;
    int idx = 0;
    std::cout << demo_common::L("选择", "choose") << ": ";
    if (!(std::cin >> idx) || idx < 0 || idx >= (int)sizes.size()) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    g_cfg.photo_size = sizes[idx];
    if (!g_cam->SetPhotoSize(g_cfg.mode->fmode, g_cfg.photo_size))
        std::cerr << demo_common::L("设置失败", "Set failed") << std::endl;
    else
        std::cout << demo_common::L("已选", "Selected") << ": " << photoSizeName(g_cfg.photo_size) << std::endl;
}

static void cmdSetRawType() {
    if (!g_cfg.mode || !g_cfg.has_raw) {
        std::cout << demo_common::L("不支持或未选模式", "not supported or no mode") << std::endl;
        return;
    }
    auto vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "raw_capture_type");
    std::cout << demo_common::L("可选格式", "Available formats") << ":" << std::endl;
    for (size_t i = 0; i < vals.size(); ++i)
        std::cout << "  [" << i << "] " << vals[i] << std::endl;
    int idx = 0;
    std::cout << demo_common::L("选择", "choose") << ": ";
    if (!(std::cin >> idx) || idx < 0 || idx >= (int)vals.size()) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    g_cfg.raw_type = vals[idx];
    int rawType = g_cam->GetAttrValueByName("raw_capture_type", vals[idx]);
    if (rawType >= 0)
        g_cam->SetRawCaptureType(g_cfg.mode->fmode, static_cast<ins_camera::RawCaptureType>(rawType));
    std::cout << demo_common::L("已设格式", "Format set") << ": " << vals[idx] << std::endl;
}

static void cmdSetSelfTimer() {
    if (!g_cfg.mode || !g_cfg.has_self_timer) {
        std::cout << demo_common::L("不支持", "not supported") << std::endl;
        return;
    }
    auto vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "photography_self_timer");
    if (!vals.empty()) {
        std::cout << demo_common::L("可选", "Available") << ":";
        for (auto& v : vals)
            std::cout << " " << v;
        std::cout << std::endl;
    }
    std::cout << demo_common::L("倒计时秒数(0=关)", "timer seconds (0=off)") << ": ";
    if (!(std::cin >> g_cfg.self_timer)) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
    }
    if (!g_cam->SetPhotoSubModeTimer(g_cfg.mode->psub, g_cfg.self_timer)) {
        std::cerr << demo_common::L("设置倒计时失败", "failed to set self timer") << std::endl;
        return;
    }
    std::cout << demo_common::L("倒计时", "Timer") << ": " << g_cfg.self_timer << "s" << std::endl;
}

static void cmdSetLapseTime() {
    if (!g_cfg.mode || !g_cfg.has_lapse_time) {
        std::cout << demo_common::L("不支持", "not supported") << std::endl;
        return;
    }
    auto vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "lapse_time");
    if (vals.empty()) {
        std::cout << demo_common::L("无数据", "no data") << std::endl;
        return;
    }
    std::cout << demo_common::L("可选间隔(秒)", "Available intervals") << ":" << std::endl;
    for (size_t i = 0; i < vals.size(); ++i)
        std::cout << "  [" << i << "] " << vals[i] << std::endl;
    int idx = 0;
    std::cout << demo_common::L("选择", "choose") << ": ";
    if (!(std::cin >> idx) || idx < 0 || idx >= (int)vals.size()) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    g_cfg.lapse_time = vals[idx];
    std::cout << demo_common::L("间隔", "Interval") << ": " << g_cfg.lapse_time << "s" << std::endl;
    applyTimelapseParams();
}

static void cmdSetDuration() {
    if (!g_cfg.mode || !g_cfg.has_duration) {
        std::cout << demo_common::L("不支持", "not supported") << std::endl;
        return;
    }
    auto vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "record_duration");
    if (vals.empty()) {
        std::cout << demo_common::L("无数据", "no data") << std::endl;
        return;
    }
    std::cout << demo_common::L("可选时长(秒)", "Available durations") << ":" << std::endl;
    for (size_t i = 0; i < vals.size(); ++i)
        std::cout << "  [" << i << "] " << (vals[i] == "0" ? "∞" : vals[i]) << std::endl;
    int idx = 0;
    std::cout << demo_common::L("选择", "choose") << ": ";
    if (!(std::cin >> idx) || idx < 0 || idx >= (int)vals.size()) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    g_cfg.record_duration = vals[idx];
    std::cout << demo_common::L("时长", "Duration") << ": "
              << (g_cfg.record_duration == "0" ? "∞" : g_cfg.record_duration + "s") << std::endl;
    if (!applyTimelapseParams())
        return;
    // 下发后回读一次，让菜单显示相机的真实值。
    g_cam->SyncPhotographyOptions(g_cfg.mode->fmode);
}

static void cmdSetHdrMode() {
    if (!g_cfg.mode || !g_cfg.has_hdr) {
        std::cout << demo_common::L("仅HDR模式支持", "HDR mode only") << std::endl;
        return;
    }
    auto vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "hdr_photo_mode");
    if (vals.empty()) {
        std::cout << demo_common::L("无数据", "no data") << std::endl;
        return;
    }
    std::cout << demo_common::L("可选HDR模式", "HDR modes") << ":" << std::endl;
    for (size_t i = 0; i < vals.size(); ++i)
        std::cout << "  [" << i << "] " << vals[i] << std::endl;
    int idx = 0;
    std::cout << demo_common::L("选择", "choose") << ": ";
    if (!(std::cin >> idx) || idx < 0 || idx >= (int)vals.size()) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    g_cfg.hdr_mode = vals[idx];
    if (!setHdrModeByName(g_cfg.hdr_mode)) {
        std::cerr << demo_common::L("设置HDR模式失败", "Failed to set HDR mode") << std::endl;
        return;
    }
    if (g_cfg.hdr_mode == "AEB") {
        std::cout << demo_common::L("AEB张数", "AEB count") << " [" << g_cfg.aeb_num << "]: ";
        if (!(std::cin >> g_cfg.aeb_num)) {
            std::cin.clear();
            std::cin.ignore(0x7fffffff, '\n');
        }
        g_cam->SetAebCaptureNum(g_cfg.mode->fmode, g_cfg.aeb_num);
        std::cout << demo_common::L("AEB步长(EV)", "AEB step (EV)") << " [" << g_cfg.aeb_ev << "]: ";
        if (!(std::cin >> g_cfg.aeb_ev)) {
            std::cin.clear();
            std::cin.ignore(0x7fffffff, '\n');
        }
        g_cam->SetAebExposureBias(g_cfg.mode->fmode, g_cfg.aeb_ev);
    }
    std::cout << demo_common::L("HDR已设", "HDR set") << ": " << g_cfg.hdr_mode << std::endl;
}

static void cmdStartCapture() {
    if (!g_cfg.mode) {
        std::cout << demo_common::L("请先选模式", "select mode first") << std::endl;
        return;
    }
    // Check SD card before capture
    ins_camera::StorageStatus st;
    if (g_cam->GetStorageState(st)) {
        if (st.state == ins_camera::STOR_CS_NOCARD) {
            std::cerr << demo_common::L("无SD卡，无法拍摄", "No SD card, cannot capture") << std::endl;
            return;
        }
        if (st.state == ins_camera::STOR_CS_NOSPACE) {
            std::cerr << demo_common::L("SD卡已满，无法拍摄", "SD card full, cannot capture") << std::endl;
            return;
        }
    }
    // 相机模式可能已被别的操作切走（例如在拍照菜单里又选过一次模式），
    // 启动前按当前选择核对一次；不一致就切回来并重发延时参数（切模式可能复位它们）。
    if (g_cam->GetCurrentFunctionMode() != g_cfg.mode->fmode) {
        if (!g_cam->SetPhotoSubMode(g_cfg.mode->psub)) {
            std::cerr << demo_common::L("相机模式不一致且切换失败", "camera mode mismatch, switch failed") << std::endl;
            return;
        }
        applyTimelapseParams();
    }
    // 拍摄参数（尺寸/RAW/延时间隔时长等）已在各设置项里即时下发，此处不重复下发，只负责启动。
    if (g_cfg.mode->is_timelapse) {
        g_capturing = true;
        auto tmode = currentTimelapseMode();
        bool ok = g_cam->StartTimeLapse(tmode);
        std::cout << (ok ? demo_common::L("已开始(选9停止)", "Started (9 to stop)")
                         : demo_common::L("启动失败", "Start failed"))
                  << std::endl;
        if (!ok)
            g_capturing = false;
    } else if (g_cfg.mode->is_hdr && g_cfg.hdr_mode == "AEB") {
        bool is_aeb = (g_cfg.hdr_mode == "AEB");
        auto urls =
            g_cam->StartHDRCapture(g_cfg.photo_size, is_aeb, g_cfg.aeb_num, static_cast<int>(g_cfg.aeb_ev * 10));
        if (!urls.Empty())
            std::cout << demo_common::L("HDR(AEB)已拍摄", "HDR(AEB) taken") << ": " << urls.GetSingleOrigin()
                      << std::endl;
        else
            std::cerr << demo_common::L("HDR(AEB)拍摄失败", "HDR(AEB) failed") << std::endl;
    } else if (g_cfg.mode->is_hdr) {
        auto urls = g_cam->StartHDRCapture(g_cfg.photo_size);
        if (!urls.Empty())
            std::cout << demo_common::L("HDR已拍摄", "HDR taken") << ": " << urls.GetSingleOrigin() << std::endl;
        else
            std::cerr << demo_common::L("HDR拍摄失败", "HDR failed") << std::endl;
    } else {
        auto url = g_cam->TakePhoto();
        if (!url.Empty())
            std::cout << demo_common::L("拍照成功", "OK") << ": " << url.GetSingleOrigin() << std::endl;
        else
            std::cerr << demo_common::L("拍照失败", "Failed") << std::endl;
    }
}

static void cmdStopCapture() {
    if (!g_cam->CaptureCurrentStatus()) {
        // 同 video：相机已不在拍摄时本地标志要归位，否则主菜单会一直按"拍照延时中"挡住录像。
        g_capturing = false;
        std::cout << demo_common::L("未在拍摄", "not capturing") << std::endl;
        return;
    }
    auto url = g_cam->StopTimeLapse(currentTimelapseMode());
    g_capturing = false;
    if (!url.Empty())
        std::cout << demo_common::L("已停止", "Stopped") << ": " << url.GetSingleOrigin() << std::endl;
    else
        std::cout << demo_common::L("已停止(可能任务已结束)", "Stopped (task may have ended)") << std::endl;
}

static void cmdTakePhotoAndDownload() {
    if (!g_cfg.mode) {
        std::cout << demo_common::L("请先选模式", "select mode first") << std::endl;
        return;
    }
    // Check SD card before capture
    ins_camera::StorageStatus st;
    if (g_cam->GetStorageState(st) && st.state == ins_camera::STOR_CS_NOCARD) {
        std::cerr << demo_common::L("无SD卡，无法拍摄", "No SD card, cannot capture") << std::endl;
        return;
    }
    auto url = g_cam->TakePhoto();
    if (url.Empty()) {
        std::cerr << demo_common::L("拍照失败", "Failed") << std::endl;
        return;
    }
    std::cout << demo_common::L("拍照成功", "OK") << ": " << url.GetSingleOrigin() << std::endl;
    std::string dir;
    // 提示独占一行：异步日志可能插在提示行之后，单行提示会被冲掉。
    std::cout << "\n" << demo_common::L("保存目录(留空=当前目录)", "save directory (empty = current dir)") << ": "
              << std::flush;
    // 只吃掉上一项输入残留的那一个换行；用 ignore 会把管道里已经排队的下一行整个吞掉。
    if (std::cin.peek() == '\n') {
        std::cin.get();
    }
    std::getline(std::cin, dir);
    if (dir.empty()) {
        dir = ".";
    }
    std::replace(dir.begin(), dir.end(), '\\', '/');
    if (!demo_common::fileIsExist(dir)) {
        std::cout << demo_common::L("目录不存在", "dir not found") << std::endl;
        return;
    }
    if (!demo_common::endsWithSlash(dir))
        dir += "/";
    std::string remote = url.GetSingleOrigin();
    std::string fname = demo_common::GetFileName(remote);
    demo_common::downloadFileFromCamera(g_cam, remote, dir + fname);
}

static void cmdBrowseAttrs() {
    if (!g_cfg.mode) {
        std::cout << demo_common::L("请先选模式", "select mode first") << std::endl;
        return;
    }
    auto attrs = g_cam->GetSupportedAttrNames(g_cfg.mode->fmode);
    if (attrs.empty()) {
        std::cout << demo_common::L("无属性", "no attrs") << std::endl;
        return;
    }
    std::cout << attrs.size() << " " << demo_common::L("个属性", "attrs") << ":" << std::endl;
    for (auto& a : attrs) {
        std::cout << "  " << a;
        auto deps = g_cam->GetAttrDependOn(g_cfg.mode->fmode, a);
        if (!deps.empty()) {
            std::cout << " (";
            for (size_t i = 0; i < deps.size(); ++i) {
                if (i)
                    std::cout << ", ";
                std::cout << deps[i];
            }
            std::cout << ")";
        }
        auto vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, a);
        if (!vals.empty()) {
            std::cout << " [";
            int n = (std::min)((int)vals.size(), 6);
            for (int i = 0; i < n; ++i) {
                if (i)
                    std::cout << " ";
                std::cout << vals[i];
            }
            if ((int)vals.size() > 6)
                std::cout << " ...";
            std::cout << "]";
        }
        std::cout << std::endl;
    }
}

// --- main menu ---
inline void run() {
    // 诊断:进入拍照前打印当前模式与能力表可用性,便于定位"进拍照即退出"类问题
    // (如能力表为空时各查询返回空,便于区分是数据缺失还是逻辑问题)。
    {
        auto fmode = g_cam->GetCurrentFunctionMode();
        auto attrs = g_cam->GetSupportedAttrNames(fmode);
        std::cout << demo_common::L("[诊断] 当前功能模式=", "[diag] function mode=") << static_cast<int>(fmode)
                  << demo_common::L(" 能力表属性数=", " capability attr count=") << attrs.size() << std::endl;
        if (attrs.empty())
            std::cout << demo_common::L("[诊断] 能力表为空: 可能未匹配到 JSON 或加载失败,拍照配置将取默认值",
                                        "[diag] capability empty: JSON not matched or load failed; using defaults")
                      << std::endl;
    }
    autoDetectMode();
    while (true) {
        std::cout << "\n=== " << demo_common::L("拍照", "Photo") << " ===\n";
        printConfig();
        std::cout << "  1: " << demo_common::L("选择模式", "select mode") << "\n"
                  << "  2: " << demo_common::L("设照片尺寸", "set photo size") << "\n";
        if (g_cfg.has_raw)
            std::cout << "  3: " << demo_common::L("设照片格式", "set photo format") << "\n";
        if (g_cfg.has_self_timer)
            std::cout << "  4: " << demo_common::L("设倒计时", "set self-timer") << "\n";
        if (g_cfg.has_lapse_time)
            std::cout << "  5: " << demo_common::L("设间隔", "set interval") << "\n";
        if (g_cfg.has_duration)
            std::cout << "  6: " << demo_common::L("设时长", "set duration") << "\n";
        if (g_cfg.has_hdr)
            std::cout << "  7: " << demo_common::L("HDR参数", "HDR settings") << "\n";
        std::cout << "  8: " << demo_common::L("开始拍摄", "start capture") << "\n"
                  << "  9: " << demo_common::L("停止(间隔/星空)", "stop (interval/starlapse)") << "\n"
                  << " 10: " << demo_common::L("拍照并下载", "take photo + download") << "\n"
                  << " 11: " << demo_common::L("浏览属性", "browse attrs") << "\n"
                  << "  0: " << demo_common::L("返回", "back") << "\n"
                  << demo_common::L("输入", "enter") << ": ";
        int opt = -1;
        if (!(std::cin >> opt)) {
            std::cin.clear();
            std::cin.ignore(0x7fffffff, '\n');
            continue;
        }
        if (opt == 0)
            break;
        switch (opt) {
        case 1: cmdSelectMode(); break;
        case 2: cmdSetPhotoSize(); break;
        case 3: cmdSetRawType(); break;
        case 4: cmdSetSelfTimer(); break;
        case 5: cmdSetLapseTime(); break;
        case 6: cmdSetDuration(); break;
        case 7: cmdSetHdrMode(); break;
        case 8: cmdStartCapture(); break;
        case 9: cmdStopCapture(); break;
        case 10: cmdTakePhotoAndDownload(); break;
        case 11: cmdBrowseAttrs(); break;
        default: std::cout << demo_common::L("无效", "invalid") << std::endl; break;
        }
    }
}

} // namespace photo