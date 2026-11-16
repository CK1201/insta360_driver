// preview.h — live preview sub-module for CameraSDKDemo
#pragma once
#include "../common/camera_setup.h"
#include "../common/i18n.h"
#include <camera/camera.h>
#include <camera/photography_settings.h>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stream/stream_delegate.h>
#include <string>
#include <thread>

extern std::shared_ptr<ins_camera::Camera> g_cam;

namespace preview {

static std::shared_ptr<ins_camera::StreamDelegate> g_delegate;
static bool g_previewing = false;
// 当前预览是否为带音频模式(VIDEO_LIVEVIEW 子模式)。停止时据此切回普通子模式。
static bool g_audio_preview = false;
// 预览+录像模式下录像是否真的起来了(录像可能单独失败)。
static bool g_recording = false;

class FileStreamDelegate : public ins_camera::StreamDelegate {
public:
    void OnVideoData(const uint8_t* data, size_t size, int64_t timestamp, uint8_t, int stream_index) override {
        if (!file_.is_open()) {
            std::string name = "preview_stream_" + std::to_string(stream_index) + ".h264";
            file_.open(name, std::ios::binary | std::ios::trunc);
            std::cout << demo_common::L("录制到", "Recording to") << ": " << name << std::endl;
        }
        file_.write(reinterpret_cast<const char*>(data), size);
    }
    // 音频回调：懒打开文件，首次收到数据才创建 preview_audio.aac（ADTS 裸流，ffplay 可直接播）。
    // 没收到音频时文件不会生成——以此区分「相机没发」和「发了没写盘」。
    void OnAudioData(const uint8_t* data, size_t size, int64_t) override {
        if (!audio_.is_open()) {
            audio_.open("preview_audio.aac", std::ios::binary | std::ios::trunc);
            std::cout << demo_common::L("录制到", "Recording to") << ": preview_audio.aac" << std::endl;
        }
        audio_.write(reinterpret_cast<const char*>(data), size);
        // 诊断：第 1 包及此后每 100 包报一次，确认音频包在持续到达
        // if ((++audio_packets_ % 100) == 1)
        //     std::cout << "audio pkt #" << audio_packets_ << " size=" << size << std::endl;
    }
    void OnGyroData(const std::vector<ins_camera::GyroData>&) override {}
    void OnExposureData(const ins_camera::ExposureData&) override {}
    ~FileStreamDelegate() {
        if (file_.is_open())
            file_.close();
        if (audio_.is_open())
            audio_.close();
    }

private:
    std::ofstream file_;
    std::ofstream audio_;
    uint64_t audio_packets_ = 0;
};

inline void run() {
    while (true) {
        std::cout << "\n=== " << demo_common::L("实时预览", "Preview") << " ===\n"
                  << "  1: " << demo_common::L("开始预览", "start preview") << "\n"
                  << "  2: " << demo_common::L("停止预览", "stop preview") << "\n"
                  << "  3: " << demo_common::L("预览+录像", "preview+record") << "\n"
                  << "  4: " << demo_common::L("停止预览+录像", "stop preview+record") << "\n"
                  << "  5: " << demo_common::L("开始预览(带音频)", "start preview (with audio)") << "\n"
                  << "  6: " << demo_common::L("停止带音频预览", "stop preview with audio") << "\n"
                  << "  0: " << demo_common::L("返回", "back") << "\n"
                  << demo_common::L("输入", "enter") << ": ";
        int opt = -1;
        if (!(std::cin >> opt)) {
            std::cin.clear();
            std::cin.ignore(0x7fffffff, '\n');
            continue;
        }
        if (opt == 0) {
            if (g_previewing) {
                g_cam->StopLiveStreaming();
                if (g_audio_preview) {
                    g_cam->SetVideoSubMode(ins_camera::SubVideoMode::VIDEO_NORMAL);
                    g_audio_preview = false;
                }
                g_previewing = false;
            }
            break;
        }
        switch (opt) {
        case 1: {
            if (g_previewing) {
                std::cout << demo_common::L("已在预览中", "already previewing") << std::endl;
                break;
            }
            g_delegate = std::make_shared<FileStreamDelegate>();
            g_cam->SetStreamDelegate(g_delegate);
            ins_camera::LiveStreamParam param;
            param.video_resolution = ins_camera::VideoResolution::RES_1920_960P30;
            param.lrv_video_resulution = ins_camera::VideoResolution::RES_1920_960P30;
            param.video_bitrate = 1024 * 1024 / 2;
            param.enable_audio = false;
            param.using_lrv = false;
            if (g_cam->StartLiveStreaming(param)) {
                g_previewing = true;
                std::cout << demo_common::L("预览已开启", "Preview started") << std::endl;
            } else
                std::cout << demo_common::L("失败", "Failed") << std::endl;
            break;
        }
        case 2: {
            if (!g_previewing) {
                std::cout << demo_common::L("未在预览", "not previewing") << std::endl;
                break;
            }
            if (g_audio_preview) {
                std::cout << demo_common::L("当前是带音频预览，请用选项 6 停止",
                                            "Audio preview is active, use option 6 to stop")
                          << std::endl;
                break;
            }
            g_cam->StopLiveStreaming();
            g_previewing = false;
            std::cout << demo_common::L("已停止", "Stopped") << std::endl;
            break;
        }
        case 3: {
            if (g_previewing) {
                std::cout << demo_common::L("已在预览中", "already previewing") << std::endl;
                break;
            }
            g_delegate = std::make_shared<FileStreamDelegate>();
            g_cam->SetStreamDelegate(g_delegate);
            ins_camera::LiveStreamParam param;
            param.video_resolution = ins_camera::VideoResolution::RES_1920_960P30;
            param.lrv_video_resulution = ins_camera::VideoResolution::RES_1920_960P30;
            param.video_bitrate = 1024 * 1024 / 2;
            param.enable_audio = false;
            param.using_lrv = false;
            if (g_cam->StartLiveStreaming(param)) {
                g_previewing = true;
                // 录像可能单独失败(如无卡)，此时只有预览在跑，case 4 不应再去 StopRecording。
                g_recording = g_cam->StartRecording();
                std::cout << (g_recording ? demo_common::L("预览+录像已开启", "Preview+Record started")
                                          : demo_common::L("预览已开启，但启动录像失败",
                                                           "Preview started, but recording failed"))
                          << std::endl;
            } else
                std::cout << demo_common::L("失败", "Failed") << std::endl;
            break;
        }
        case 4: {
            if (!g_previewing) {
                std::cout << demo_common::L("未在预览", "not previewing") << std::endl;
                break;
            }
            if (g_audio_preview) {
                std::cout << demo_common::L("当前是带音频预览，请用选项 6 停止",
                                            "Audio preview is active, use option 6 to stop")
                          << std::endl;
                break;
            }
            if (g_recording) {
                auto url = g_cam->StopRecording();
                if (!url.Empty())
                    std::cout << demo_common::L("录像文件", "Recorded") << ": " << url.GetSingleOrigin() << std::endl;
                g_recording = false;
            }
            g_cam->StopLiveStreaming();
            g_previewing = false;
            std::cout << demo_common::L("已停止", "Stopped") << std::endl;
            break;
        }
        case 5: {
            // 参考开启带音频预览的完整组合：
            // 1) 相机先切到"APP直播"子模式 VIDEO_LIVEVIEW=12 —— 固件仅在该模式下才向预览流下发
            //    音频；只设置 StartLiveStream 的音频字段是拿不到音频的(X5/X6 实测)。
            // 2) StartLiveStream: enable_audio=true + is_for_live=true + ADTS/48000/128000(LiveStreamParam 默认值)。
            // 3) 停止时切回 VIDEO_NORMAL 恢复相机模式。
            if (g_previewing) {
                std::cout << demo_common::L("已在预览中", "already previewing") << std::endl;
                break;
            }
            g_delegate = std::make_shared<FileStreamDelegate>();
            g_cam->SetStreamDelegate(g_delegate);
            ins_camera::LiveStreamParam param;
            param.video_resolution = ins_camera::VideoResolution::RES_1920_960P30;
            param.lrv_video_resulution = ins_camera::VideoResolution::RES_1920_960P30;
            param.video_bitrate = 1024 * 1024 / 2;
            param.enable_audio = true;
            param.is_for_live = true;
            param.using_lrv = false;
            if (!g_cam->SetVideoSubMode(ins_camera::SubVideoMode::VIDEO_LIVEVIEW)) {
                std::cout << demo_common::L("切换直播子模式失败", "Failed to switch to live submode") << std::endl;
                break;
            }
            if (g_cam->StartLiveStreaming(param)) {
                g_previewing = true;
                g_audio_preview = true;
                std::cout << demo_common::L("带音频预览已开启", "Preview with audio started") << std::endl;
            } else {
                std::cout << demo_common::L("失败", "Failed") << std::endl;
                g_cam->SetVideoSubMode(ins_camera::SubVideoMode::VIDEO_NORMAL);
            }
            break;
        }
        case 6: {
            if (!g_previewing) {
                std::cout << demo_common::L("未在预览", "not previewing") << std::endl;
                break;
            }
            if (!g_audio_preview) {
                std::cout << demo_common::L("当前不是带音频预览，请用选项 2 停止",
                                            "Not an audio preview, use option 2 to stop")
                          << std::endl;
                break;
            }
            g_cam->StopLiveStreaming();
            g_previewing = false;
            g_audio_preview = false;
            g_cam->SetVideoSubMode(ins_camera::SubVideoMode::VIDEO_NORMAL);
            std::cout << demo_common::L("已停止", "Stopped") << std::endl;
            break;
        }
        default: std::cout << demo_common::L("无效", "invalid") << std::endl; break;
        }
    }
}

} // namespace preview