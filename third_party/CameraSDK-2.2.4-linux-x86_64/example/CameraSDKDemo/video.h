// video.h — video recording sub-module for CameraSDKDemo
#pragma once
#include "../common/camera_setup.h"
#include "../common/i18n.h"
#include "demo_util.h"
#include <algorithm>
#include <camera/photography_settings.h>
#include <cmath> // std::fabs
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

extern std::shared_ptr<ins_camera::Camera> g_cam;
extern ins_camera::CameraType g_cameraType;

namespace video {

using ins_camera::CameraFunctionMode;
using ins_camera::CameraTimelapseMode;
using ins_camera::SubVideoMode;
using ins_camera::VideoResolution;

// --- mode descriptors ---
struct VideoModeDesc {
    const char* name;
    const char* en_name;
    SubVideoMode vsub;
    CameraFunctionMode fmode;
};
static const std::vector<VideoModeDesc> kVideoModes = {
    {"普通录像", "normal video", SubVideoMode::VIDEO_NORMAL, CameraFunctionMode::FUNCTION_MODE_NORMAL_VIDEO},
    {"夜景录像", "night video", SubVideoMode::VIDEO_PURE, CameraFunctionMode::FUNCTION_MODE_PURE_VIDEO},
    {"HDR视频", "HDR video", SubVideoMode::VIDEO_HDR, CameraFunctionMode::FUNCTION_MODE_HDR_VIDEO},
    {"延时摄影", "timelapse", SubVideoMode::VIDEO_TIMELAPSE, CameraFunctionMode::FUNCTION_MODE_MOBILE_TIMELAPSE},
    {"移动延时", "timeshift", SubVideoMode::VIDEO_TIMESHIFT, CameraFunctionMode::FUNCTION_MODE_TIMESHIFT},
    {"子弹时间", "bullet time", SubVideoMode::VIDEO_BULLETTIME, CameraFunctionMode::FUNCTION_MODE_BULLETTIME},
    {"循环录影", "loop recording", SubVideoMode::VIDEO_LOOPRECORDING,
     CameraFunctionMode::FUNCTION_MODE_LOOP_RECORDING_VIDEO},
    {"行车记录仪", "dashcam", SubVideoMode::VIDEO_DASH_CAM, CameraFunctionMode::FUNCTION_MODE_DASH_CAM},
};

// --- config ---
struct VideoConfig {
    const VideoModeDesc* mode = nullptr;
    VideoResolution resolution = VideoResolution::RES_3840_1920P30;
    std::string res_name;
    std::string scene;
    std::string lapse_time;
    std::string record_duration;
    int accelerate = 0;
    bool is_timelapse = false;
    bool has_accel = false;
    bool has_duration = false; // 模式是否支持 record_duration（如循环录影、延时）
};
static VideoConfig g_cfg;
bool g_running = false;
static bool g_is_timelapse = false;
static bool g_initialized = false;
static CameraTimelapseMode g_last_tmode = CameraTimelapseMode::TIMELAPSE_MIXED;

static void populateDefaults();

static void autoDetectMode() {
    if (g_initialized && g_cfg.mode)
        return;
    g_initialized = true;
    auto fmode = g_cam->GetCurrentFunctionMode();
    for (auto& m : kVideoModes) {
        if (m.fmode == fmode) {
            g_cfg.mode = &m;
            populateDefaults();
            return;
        }
    }
}

static bool modeHasAttr(CameraFunctionMode fmode, const std::string& attr) {
    auto vals = g_cam->GetSupportedAttrValues(fmode, attr);
    return !vals.empty();
}

static std::string resLabel(int w, int h) {
    if (w == 15360 && h == 7680)
        return "16K (15360x7680)";
    if (w == 7680 && h == 3840)
        return "8K (7680x3840)";
    if (w == 6016 && h == 3008)
        return "6K (6016x3008)";
    if (w == 3840 && h == 1920)
        return "4K (3840x1920)";
    if (w == 3840 && h == 3840)
        return "8K (3840x3840)";
    if (w == 3008 && h == 3008)
        return "6K (3008x3008)";
    if (w == 5760 && h == 1440)
        return "5.7K (5760x1440)";
    if (w == 7680 && h == 1920)
        return "BulletTime (7680x1920)";
    if (w == 6016 && h == 1504)
        return "BulletTime (6016x1504)";
    if (w == 3840 && h == 960)
        return "BulletTime (3840x960)";
    return std::to_string(w) + "x" + std::to_string(h);
}

// 某个延时场景推荐的间隔(秒)。能力表 record_duration 的分支键形如 "cloud|10"，
// 即"场景|间隔"；custom 可自选间隔，返回空串。
static std::string sceneLapseTime(CameraFunctionMode fmode, const std::string& scene) {
    if (scene.empty() || scene == "custom")
        return "";
    for (const auto& key : g_cam->GetAttrDependOnKeys(fmode, "record_duration")) {
        const auto bar = key.find('|');
        if (bar == std::string::npos)
            continue; // "default" 这种无分支键
        if (key.substr(0, bar) == scene)
            return key.substr(bar + 1);
    }
    return "";
}

// 把推荐间隔落到当前分辨率的合法间隔上：16K 没有 0.5/1/2，8K 没有 3，
// 两边不是包含关系，所以推荐值不可用时取数值上最接近的一档。
static std::string nearestLapseTime(const std::string& want, const std::vector<std::string>& avail) {
    if (avail.empty())
        return want;
    if (std::find(avail.begin(), avail.end(), want) != avail.end())
        return want;
    const double w = std::atof(want.c_str());
    if (w <= 0.0)
        return avail[0];
    std::string best = avail[0];
    // 用 fabs 而非 std::abs：GCC 5.4 的 <cstdlib> 只声明整数重载(int/long/long long)，
    // std::abs(double) 在 <cmath> 里，仅包 <cstdlib> 时 double 实参会报 ambiguous。
    double best_diff = std::fabs(std::atof(avail[0].c_str()) - w);
    for (const auto& v : avail) {
        const double diff = std::fabs(std::atof(v.c_str()) - w);
        if (diff < best_diff) {
            best_diff = diff;
            best = v;
        }
    }
    return best;
}

// 依赖链：record_resolution → lapse_time → record_duration。上游变了要重查下游合法值。
// 依赖 key 的拼法与能力表 depend_on 的字段顺序一致：
//   lapse_time.depend_on      = ["record_resolution"]             → key "7680_3840_30"
//   record_duration.depend_on = ["timelapse_scene","lapse_time"]  → key "cloud|10"
// 只查本地能力表，不与相机通信。
static void refreshDependentDefaults() {
    if (!g_cfg.mode)
        return;
    const auto fmode = g_cfg.mode->fmode;
    if (g_cfg.is_timelapse) {
        auto lv = g_cam->GetSupportedAttrValues(fmode, "lapse_time", g_cfg.res_name);
        if (lv.empty())
            lv = g_cam->GetSupportedAttrValues(fmode, "lapse_time");
        // 场景声明了固定间隔(非 custom)就跟着场景走，换分辨率时也重新贴合当前档位；
        // custom 场景没有分支键，保留用户已选的间隔，仅在当前分辨率下非法时才重置。
        const auto scene_lapse = sceneLapseTime(fmode, g_cfg.scene);
        if (!scene_lapse.empty()) {
            g_cfg.lapse_time = nearestLapseTime(scene_lapse, lv);
        } else if (std::find(lv.begin(), lv.end(), g_cfg.lapse_time) == lv.end()) {
            g_cfg.lapse_time = lv.empty() ? "" : lv[0];
        }
    }
    if (g_cfg.has_duration) {
        auto dv = g_cfg.is_timelapse
                      ? g_cam->GetSupportedAttrValues(fmode, "record_duration", g_cfg.scene + "|" + g_cfg.lapse_time)
                      : std::vector<std::string>{};
        if (dv.empty())
            dv = g_cam->GetSupportedAttrValues(fmode, "record_duration");
        // 优先显示相机当前真实值，而不是能力表候选列表的第一项(机身上改过时两者会不符)。
        uint32_t real_duration = 0;
        if (g_cam->GetRecordDuration(fmode, real_duration)) {
            g_cfg.record_duration = std::to_string(real_duration);
        } else if (std::find(dv.begin(), dv.end(), g_cfg.record_duration) == dv.end()) {
            g_cfg.record_duration = dv.empty() ? "0" : dv[0];
        }
    } else {
        g_cfg.record_duration = "0";
    }
}

static void populateDefaults() {
    if (!g_cfg.mode)
        return;
    auto fmode = g_cfg.mode->fmode;
    g_cfg.is_timelapse = modeHasAttr(fmode, "lapse_time");
    g_cfg.has_accel = !g_cam->GetSupportedAccelerateFrequencies(fmode).empty();
    // 是否有时长设置项，按能力表判断。循环录影是"单段循环片段长度"(滚动覆盖，录制不停)，
    // 延时摄影是"拍摄时长"(到点自停)，语义不同但都用这一项。
    g_cfg.has_duration = modeHasAttr(fmode, "record_duration");

    auto resolutions = g_cam->GetSupportedVideoResolutions(fmode);
    if (!resolutions.empty()) {
        g_cfg.resolution = resolutions[0];
        g_cfg.res_name = g_cam->GetVideoResolutionName(resolutions[0]);
    }
    if (g_cfg.is_timelapse) {
        auto scenes = g_cam->GetSupportedAttrValues(fmode, "timelapse_scene");
        g_cfg.scene = scenes.empty() ? "" : scenes[0];
    }
    refreshDependentDefaults();
    if (g_cfg.has_accel) {
        auto accels = g_cam->GetSupportedAccelerateFrequencies(fmode);
        g_cfg.accelerate = accels.empty() ? 0 : accels[0];
    }
}

static void printConfig() {
    if (!g_cfg.mode) {
        std::cout << demo_common::L("(未选模式)", "(no mode)") << std::endl;
        return;
    }
    std::cout << demo_common::L("模式", "Mode") << ": " << demo_common::L(g_cfg.mode->name, g_cfg.mode->en_name)
              << " | " << demo_common::L("默认分辨率", "Default res") << ": " << g_cfg.res_name;
    if (g_cfg.is_timelapse) {
        std::cout << " | " << demo_common::L("场景", "scene") << ": " << g_cfg.scene << " | "
                  << demo_common::L("间隔", "interval") << ": " << g_cfg.lapse_time << "s";
    }
    if (g_cfg.has_duration) {
        std::cout << " | " << demo_common::L("时长", "duration") << ": "
                  << demo_util::durationLabel(g_cfg.record_duration);
    }
    if (g_cfg.has_accel)
        std::cout << " | " << demo_common::L("倍速", "speed") << ": " << g_cfg.accelerate;
    std::cout << std::endl;
}

// 延时下发用的 TimelapseMode。SetTimeLapseOption / StartTimelapse / StopTimelapse 共用。
static CameraTimelapseMode currentTimelapseMode() {
    return CameraTimelapseMode::MOBILE_TIMELAPSE_VIDEO;
}

// 设置即下发：每改一项就发到相机，不必真的开始录像就能验证效果。
// 两条通道：分辨率/时长/倍速走 SetVideoCaptureParams；延时的间隔走 SetTimeLapseOption。
static bool applyVideoParams() {
    if (!g_cfg.mode)
        return false;

    // —— 上层拦截：值必须在该模式能力表内，非法值不进 SDK ——
    // SDK 只按模式原样下发、不做值域校验，所以合法性由 Demo 在这里把关。
    const auto fmode = g_cfg.mode->fmode;
    const auto resols = g_cam->GetSupportedVideoResolutions(fmode);
    if (!resols.empty() && std::find(resols.begin(), resols.end(), g_cfg.resolution) == resols.end()) {
        std::cerr << demo_common::L("分辨率不在能力表内，已拦截", "resolution not supported, blocked") << std::endl;
        return false;
    }
    if (g_cfg.has_accel) {
        const auto freqs = g_cam->GetSupportedAccelerateFrequencies(fmode);
        if (!freqs.empty() && std::find(freqs.begin(), freqs.end(), g_cfg.accelerate) == freqs.end()) {
            std::cerr << demo_common::L("倍速不在能力表内，已拦截", "accelerate not supported, blocked") << std::endl;
            return false;
        }
    }
    // 时长的合法值校验：延时按 场景|间隔 分支查，其它模式(循环录影)查全量。
    if (g_cfg.has_duration && !g_cfg.record_duration.empty()) {
        auto durs = g_cfg.is_timelapse ? g_cam->GetSupportedAttrValues(fmode, "record_duration",
                                                                      g_cfg.scene + "|" + g_cfg.lapse_time)
                                       : std::vector<std::string>{};
        if (durs.empty())
            durs = g_cam->GetSupportedAttrValues(fmode, "record_duration");
        if (!durs.empty() && std::find(durs.begin(), durs.end(), g_cfg.record_duration) == durs.end()) {
            std::cerr << demo_common::L("时长不在能力表内，已拦截", "duration not supported, blocked") << std::endl;
            return false;
        }
    }

    ins_camera::RecordParams rp;
    rp.resolution = g_cfg.resolution;
    if (g_cfg.has_accel)
        rp.accelerate_fequency = g_cfg.accelerate;
    // 时长走 SetVideoCaptureParams(SDK 内部按模式选对应字段)。
    if (g_cfg.has_duration && !g_cfg.record_duration.empty())
        rp.record_duration = static_cast<uint32_t>(std::atof(g_cfg.record_duration.c_str()));
    if (!g_cam->SetVideoCaptureParams(rp, g_cfg.mode->fmode)) {
        std::cerr << demo_common::L("下发录像参数失败", "Failed to apply video params") << std::endl;
        return false;
    }
    // 延时的间隔走 SetTimeLapseOption(时长不走这里)。
    if (g_cfg.is_timelapse
        && !demo_util::applyTimelapseParams(g_cam, currentTimelapseMode(), g_cfg.lapse_time, g_cfg.record_duration))
        return false;
    std::cout << demo_common::L("已下发到相机", "Applied to camera") << std::endl;
    return true;
}

// --- menu commands ---
static void cmdSelectMode() {
    // Query capability table for supported modes; fall back to static list if unavailable.
    auto supported = g_cam->GetSupportedVideoModes();
    std::vector<const VideoModeDesc*> cand;
    if (supported.empty()) {
        for (auto& m : kVideoModes)
            cand.push_back(&m);
    } else {
        std::set<int> supported_set;
        for (auto fm : supported)
            supported_set.insert(static_cast<int>(fm));
        for (auto& m : kVideoModes)
            if (supported_set.count(static_cast<int>(m.fmode)))
                cand.push_back(&m);
    }
    if (cand.empty()) {
        std::cout << demo_common::L("无可用录像模式", "no video modes") << std::endl;
        return;
    }
    std::cout << demo_common::L("可选录像模式", "Available video modes") << ":" << std::endl;
    for (size_t i = 0; i < cand.size(); ++i)
        std::cout << "  [" << i << "] " << demo_common::L(cand[i]->name, cand[i]->en_name) << std::endl;
    int idx = 0;
    std::cout << demo_common::L("选择", "choose") << ": ";
    if (!(std::cin >> idx) || idx < 0 || idx >= (int)cand.size()) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    // 切换失败就不能把模式记为已选：后续参数按该模式下发、启动也按它调用，
    // 相机实际停在别的模式时只会得到相机侧的无效状态错误。
    if (!g_cam->SetVideoSubMode(cand[idx]->vsub)) {
        std::cerr << demo_common::L("切换录像子模式失败", "failed to switch video submode") << std::endl;
        return;
    }
    g_cfg = VideoConfig{};
    g_cfg.mode = cand[idx];
    populateDefaults();
    std::cout << demo_common::L("已选", "Selected") << ": " << demo_common::L(cand[idx]->name, cand[idx]->en_name)
              << std::endl;
    applyVideoParams();
    printConfig();
}

static void cmdSetResolution() {
    if (!g_cfg.mode) {
        std::cout << demo_common::L("请先选模式", "select mode first") << std::endl;
        return;
    }
    auto resolutions = g_cam->GetSupportedVideoResolutions(g_cfg.mode->fmode);
    if (resolutions.empty()) {
        std::cout << demo_common::L("无可用分辨率", "no resolutions") << std::endl;
        return;
    }

    // Build tiered list: group by resolution, show FPS options under each
    std::map<std::string, std::vector<std::pair<std::string, VideoResolution>>> tierMap;
    for (auto res : resolutions) {
        std::string name = g_cam->GetVideoResolutionName(res);
        if (name.empty())
            continue;
        auto p1 = name.find('_');
        auto p2 = (p1 == std::string::npos) ? std::string::npos : name.find('_', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos)
            continue;
        int w = std::atoi(name.substr(0, p1).c_str());
        int h = std::atoi(name.substr(p1 + 1, p2 - p1 - 1).c_str());
        if (w <= 0 || h <= 0)
            continue;
        std::string fps = name.substr(p2 + 1);
        std::string key = resLabel(w, h);
        tierMap[key].push_back({fps, res});
    }

    // Flatten to indexed list
    std::vector<std::pair<std::string, VideoResolution>> flat;
    std::cout << demo_common::L("可选分辨率", "Available resolutions") << ":" << std::endl;
    for (auto& kv : tierMap) {
        std::cout << "  " << kv.first << ":" << std::endl;
        for (auto& fp : kv.second) {
            int idx = (int)flat.size();
            std::cout << "    [" << idx << "] " << fp.first << "fps" << std::endl;
            flat.push_back(fp);
        }
    }
    std::cout << demo_common::L("选择(-1跳过)", "choose (-1 skip)") << ": ";
    int idx = -1;
    if (!(std::cin >> idx)) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    if (idx >= 0 && idx < (int)flat.size()) {
        g_cfg.resolution = flat[idx].second;
        g_cfg.res_name = g_cam->GetVideoResolutionName(flat[idx].second);
        // 分辨率是 lapse_time 的依赖项，lapse_time 又是 record_duration 的依赖项，
        // 逐级重查，避免留下当前分辨率下非法的间隔/时长。
        refreshDependentDefaults();
        std::cout << demo_common::L("已选", "Selected") << ": " << g_cfg.res_name << std::endl;
        applyVideoParams();
    }
}

static void cmdSetScene() {
    if (!g_cfg.mode || !g_cfg.is_timelapse) {
        std::cout << demo_common::L("仅延时摄影支持", "timelapse only") << std::endl;
        return;
    }
    auto scenes = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "timelapse_scene");
    // 每个场景后面标出它固定的间隔，custom 标"可自选"，让选择前就能看出差别。
    std::vector<std::string> labels;
    for (const auto& s : scenes) {
        const auto lt = sceneLapseTime(g_cfg.mode->fmode, s);
        labels.push_back(lt.empty() ? s + " (" + demo_common::L("间隔可自选", "custom interval") + ")"
                                    : s + " (" + lt + "s)");
    }
    int idx = demo_util::chooseFromList(labels, "可选场景", "Available scenes");
    if (idx < 0)
        return;
    g_cfg.scene = scenes[idx];
    // 场景要先下发：间隔和时长的可选值依赖它。
    if (!g_cam->SetTimelapseScene(g_cfg.mode->fmode, g_cfg.scene))
        std::cerr << demo_common::L("下发场景失败", "failed to apply scene") << std::endl;
    // 场景决定间隔(非 custom)，间隔又是 record_duration 的依赖项，逐级重查。
    refreshDependentDefaults();
    std::cout << demo_common::L("场景", "Scene") << ": " << g_cfg.scene << " | " << demo_common::L("间隔", "interval")
              << ": " << g_cfg.lapse_time << "s | " << demo_common::L("时长", "duration") << ": "
              << demo_util::durationLabel(g_cfg.record_duration) << std::endl;
    applyVideoParams();
}

static void cmdSetLapseTime() {
    if (!g_cfg.mode || !g_cfg.is_timelapse) {
        std::cout << demo_common::L("仅延时摄影支持", "timelapse only") << std::endl;
        return;
    }
    // 非 custom 场景的间隔由场景固定(相机行为)，改间隔请先把场景切到 custom。
    const auto fixed = sceneLapseTime(g_cfg.mode->fmode, g_cfg.scene);
    if (!fixed.empty()) {
        std::cout << demo_common::L("场景", "Scene") << " " << g_cfg.scene << " "
                  << demo_common::L("的间隔固定为", "has a fixed interval of") << " " << fixed << "s"
                  << demo_common::L("，如需自选请先把场景设为 custom", "; switch scene to custom to choose freely")
                  << std::endl;
        return;
    }
    // lapse_time 的 depend_on 只有 record_resolution，key 就是分辨率名本身。
    auto vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "lapse_time", g_cfg.res_name);
    if (vals.empty())
        vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "lapse_time");
    int idx = demo_util::chooseFromList(vals, "可选间隔(秒)", "Available intervals (sec)");
    if (idx < 0)
        return;
    g_cfg.lapse_time = vals[idx];
    // 间隔是 record_duration 的依赖项。
    refreshDependentDefaults();
    std::cout << demo_common::L("间隔", "Interval") << ": " << g_cfg.lapse_time << "s | "
              << demo_common::L("时长", "duration") << ": " << demo_util::durationLabel(g_cfg.record_duration)
              << std::endl;
    applyVideoParams();
}

static void cmdSetDuration() {
    if (!g_cfg.mode || !g_cfg.has_duration) {
        std::cout << demo_common::L("该模式不支持时长设置", "duration not supported") << std::endl;
        return;
    }
    // 延时按 场景|间隔 依赖查询；其它模式（如循环录影）直接查该属性全部可选值。
    auto vals = g_cfg.is_timelapse ? g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "record_duration",
                                                                   g_cfg.scene + "|" + g_cfg.lapse_time)
                                   : std::vector<std::string>{};
    if (vals.empty())
        vals = g_cam->GetSupportedAttrValues(g_cfg.mode->fmode, "record_duration");
    std::vector<std::string> labels;
    for (auto& v : vals)
        labels.push_back(demo_util::durationLabel(v));
    int idx = demo_util::chooseFromList(labels, "可选时长(秒)", "Available durations (sec)");
    if (idx < 0)
        return;
    g_cfg.record_duration = vals[idx];
    std::cout << demo_common::L("时长", "Duration") << ": " << demo_util::durationLabel(g_cfg.record_duration)
              << std::endl;
    if (!applyVideoParams())
        return;
    // 下发后回读一次，让菜单显示相机的真实值。
    g_cam->SyncPhotographyOptions(g_cfg.mode->fmode);
}

static void cmdSetAccelerate() {
    if (!g_cfg.mode || !g_cfg.has_accel) {
        std::cout << demo_common::L("不支持加速", "accel not supported") << std::endl;
        return;
    }
    auto freqs = g_cam->GetSupportedAccelerateFrequencies(g_cfg.mode->fmode);
    if (freqs.empty()) {
        std::cout << demo_common::L("无数据", "no data") << std::endl;
        return;
    }
    std::cout << demo_common::L("可选倍速", "Available speeds") << ":" << std::endl;
    for (size_t i = 0; i < freqs.size(); ++i)
        std::cout << "  [" << i << "] " << (freqs[i] == 0 ? demo_common::L("自动", "Auto") : std::to_string(freqs[i]))
                  << std::endl;
    int idx = 0;
    std::cout << demo_common::L("选择", "choose") << ": ";
    if (!(std::cin >> idx) || idx < 0 || idx >= (int)freqs.size()) {
        std::cin.clear();
        std::cin.ignore(0x7fffffff, '\n');
        return;
    }
    g_cfg.accelerate = freqs[idx];
    std::cout << demo_common::L("倍速", "Speed") << ": " << g_cfg.accelerate << std::endl;
    applyVideoParams();
}

static void cmdStartCapture() {
    if (!g_cfg.mode) {
        std::cout << demo_common::L("请先选模式", "select mode first") << std::endl;
        return;
    }
    if (g_cam->CaptureCurrentStatus()) {
        std::cout << demo_common::L("已在拍摄中", "already capturing") << std::endl;
        return;
    }
    // Check SD card
    ins_camera::StorageStatus st;
    if (g_cam->GetStorageState(st) && st.state == ins_camera::STOR_CS_NOCARD) {
        std::cerr << demo_common::L("无SD卡，无法拍摄", "No SD card, cannot capture") << std::endl;
        return;
    }
    // 相机模式可能已被别的操作切走（例如拍照菜单里的模式选择），启动前按当前选择核对；
    // 不一致就切回并重发录像参数（切模式可能复位分辨率/间隔等）。
    if (g_cam->GetCurrentFunctionMode() != g_cfg.mode->fmode) {
        if (!g_cam->SetVideoSubMode(g_cfg.mode->vsub)) {
            std::cerr << demo_common::L("相机模式不一致且切换失败", "camera mode mismatch, switch failed") << std::endl;
            return;
        }
        applyVideoParams();
    }
    // 录像与延时参数均已在各设置项里即时下发（applyVideoParams），此处不重复下发，只负责启动。
    if (g_cfg.is_timelapse) {
        auto tmode = currentTimelapseMode();
        if (g_cam->StartTimeLapse(tmode)) {
            g_running = true;
            g_is_timelapse = true;
            g_last_tmode = tmode;
            std::cout << demo_common::L("延时已开始", "Timelapse started") << std::endl;
        } else
            std::cerr << demo_common::L("启动延时失败", "Failed to start timelapse") << std::endl;
    } else {
        if (g_cam->StartRecording()) {
            g_running = true;
            g_is_timelapse = false;
            std::cout << demo_common::L("录像已开始", "Recording started") << std::endl;
        } else
            std::cout << demo_common::L("失败", "Failed") << std::endl;
    }
}

static void cmdStopCapture() {
    if (!g_cam->CaptureCurrentStatus()) {
        // 相机已不在拍摄(录满/过温/机身上停了/启动其实没成功)，本地标志必须跟着归位，
        // 否则 g_running 会一直卡在 true，主菜单按"以录像中为由"挡住拍照，只能重启 demo。
        g_running = false;
        g_is_timelapse = false;
        std::cout << demo_common::L("未在拍摄", "not capturing") << std::endl;
        return;
    }
    if (g_is_timelapse) {
        auto url = g_cam->StopTimeLapse(g_last_tmode);
        if (!url.Empty())
            std::cout << demo_common::L("延时已停止", "Timelapse stopped") << ": " << url.GetSingleOrigin()
                      << std::endl;
    } else {
        auto url = g_cam->StopRecording();
        if (!url.Empty()) {
            std::cout << demo_common::L("录像已停止", "Recording stopped") << ": ";
            for (auto& u : url.OriginUrls())
                std::cout << u << " ";
            std::cout << std::endl;
        }
    }
    g_running = false;
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
    autoDetectMode();
    while (true) {
        std::cout << "\n=== " << demo_common::L("录像", "Video") << " ===\n";
        printConfig();
        std::cout << "  1: " << demo_common::L("选择模式", "select mode") << "\n"
                  << "  2: " << demo_common::L("设置分辨率", "set resolution") << "\n";
        if (g_cfg.is_timelapse) {
            std::cout << "  3: " << demo_common::L("设场景", "set scene") << "\n"
                      << "  4: " << demo_common::L("设间隔", "set interval") << "\n";
        }
        if (g_cfg.has_duration)
            std::cout << "  5: " << demo_common::L("设时长", "set duration") << "\n";
        if (g_cfg.has_accel)
            std::cout << "  6: " << demo_common::L("设加速倍数", "set accelerate") << "\n";
        std::cout << "  7: " << demo_common::L("开始拍摄", "start capture") << "\n"
                  << "  8: " << demo_common::L("停止拍摄", "stop capture") << "\n"
                  << "  9: " << demo_common::L("浏览属性", "browse attrs") << "\n"
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
        case 2: cmdSetResolution(); break;
        case 3: cmdSetScene(); break;
        case 4: cmdSetLapseTime(); break;
        case 5: cmdSetDuration(); break;
        case 6: cmdSetAccelerate(); break;
        case 7: cmdStartCapture(); break;
        case 8: cmdStopCapture(); break;
        case 9: cmdBrowseAttrs(); break;
        default: std::cout << demo_common::L("无效", "invalid") << std::endl; break;
        }
    }
}

} // namespace video