// CameraSDKDemo: unified entry point for all camera SDK demos.
// Modules are in separate .h files for code organization.
// 运行: CameraSDKDemo [--debug [级别]] [--log_cat 场景[:级别]]... [--log_file [path]] [--en|--cn]
//   --log_cat <场景>[:级别]  单独打开某个日志场景（可重复），场景：usb/tunnel/download/capture/options
//                            all = 全部场景；all:verbose = 全量明细
//                            diag 也可用环境变量：CAMERASDK_LOG="download:info,usb:verbose"
//   不加 --debug            日志级别 = ERROR（仅错误）
//   --debug                 日志级别 = INFO（默认调试级别：含下载采样点、write_some 卡顿告警）
//   --debug <级别>          指定级别，<级别> ∈ {verbose|info|warning|error}（debug 等价 verbose）
//                           verbose 会额外打印每帧收发明细，刷屏量大，仅深度排查用

#include "../common/camera_setup.h"
#include "../common/i18n.h"
#include "capture_settings.h"
#include "file.h"
#include "photo.h"
#include "preview.h"
#include "status.h"
#include "system.h"
#include "video.h"
#include <cctype>
#include <iostream>
#include <utility>
#include <vector>

// Global state shared across all modules
std::shared_ptr<ins_camera::Camera> g_cam;
ins_camera::CameraType g_cameraType = ins_camera::CameraType::Unknown;
// Extern from photo.h / video.h
namespace photo {
extern bool g_capturing;
}
namespace video {
extern bool g_running;
}

int main(int argc, char* argv[]) {
    std::string log_file_arg;
    // 默认 ERROR；--debug 不带级别时用 INFO；--debug <级别> 用指定级别。
    ins_camera::LogLevel log_level = ins_camera::LogLevel::WARNING;
    // --log_cat 解析即应用（连接过程本身就会产生日志）。
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--debug") {
            // 后面若紧跟一个非选项 token，当作级别名解析；否则用默认 INFO
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                log_level = demo_common::parseLogLevel(argv[++i]);
            } else {
                log_level = ins_camera::LogLevel::INFO;
            }
        } else if (a == "--log_cat") {
            if (i + 1 < argc) {
                std::string spec = argv[++i];
                std::string name = spec;
                std::string level = "info";
                const auto colon = spec.find(':');
                if (colon != std::string::npos) {
                    name = spec.substr(0, colon);
                    level = spec.substr(colon + 1);
                }
                std::string lower = name;
                for (auto& c : lower)
                    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                // 立即应用：连接过程本身就会产生日志，等连接完再设就晚了。
                // 显式设置的场景优先级高于环境变量，先设不会被覆盖。
                const auto cat_level = demo_common::parseLogLevel(level);
                if (lower == "all") {
                    // all 展开为全部场景；--log_cat all:verbose 即全量明细。
                    for (const char* cat : {"usb", "tunnel", "download", "capture", "options"})
                        ins_camera::SetLogCategory(cat, cat_level);
                } else {
                    ins_camera::SetLogCategory(name, cat_level);
                }
            }
        } else if (a == "--log_file") {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                log_file_arg = argv[++i];
            else
                log_file_arg = "*";
        } else if (a == "--en")
            demo_common::lang() = demo_common::Lang::EN;
        else if (a == "--cn")
            demo_common::lang() = demo_common::Lang::CN;
    }

    g_cam = demo_common::openCamera(log_level, &g_cameraType,
                                    demo_common::resolveLogPath(log_file_arg, demo_common::exeDir(argv[0])));
    if (!g_cam)
        return -1;

    while (true) {
        std::cout << "\n========================================" << std::endl;
        std::cout << demo_common::L("  CameraSDK Demo 主菜单", "  CameraSDK Demo Menu") << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  1: " << demo_common::L("拍照 (Photo)", "Photo") << std::endl;
        std::cout << "  2: " << demo_common::L("录像 (Video)", "Video") << std::endl;
        std::cout << "  3: " << demo_common::L("曝光与设置 (CaptureSettings)", "Capture Settings") << std::endl;
        std::cout << "  4: " << demo_common::L("运行状态 (Status)", "Status") << std::endl;
        std::cout << "  5: " << demo_common::L("文件与存储 (File)", "File") << std::endl;
        std::cout << "  6: " << demo_common::L("实时预览 (Preview)", "Preview") << std::endl;
        std::cout << "  7: " << demo_common::L("系统 (System)", "System") << std::endl;
        std::cout << "  8: " << demo_common::L("重连相机 (Reconnect)", "Reconnect Camera") << std::endl;
        std::cout << "  0: " << demo_common::L("退出", "Exit") << std::endl;
        std::cout << demo_common::L("输入", "Enter") << ": ";

        int opt = -1;
        if (!(std::cin >> opt)) {
            // EOF(stdin 关闭/管道结束)时 clear+ignore 解不掉 eofbit，会变成疯狂重印
            // 菜单的死循环；只有非 EOF 的格式错误才值得清状态重试。
            if (std::cin.eof()) {
                break;
            }
            std::cin.clear();
            std::cin.ignore(0x7fffffff, '\n');
            continue;
        }
        if (opt == 0)
            break;

        switch (opt) {
        case 1:
            // 与 case 2 一致：本地标志 + 相机实际状态双重确认。只信本地标志的话，
            // 相机侧自己停了(录满/过温/机身操作)会让这里永久挡住拍照。
            if (video::g_running && g_cam->CaptureCurrentStatus()) {
                std::cout << demo_common::L("录像中，请先停止", "Stop recording first") << std::endl;
                break;
            }
            video::g_running = false; // 相机已不在拍摄，顺手纠正过期标志
            photo::run();
            break;
        case 2:
            if (photo::g_capturing && g_cam->CaptureCurrentStatus()) {
                std::cout << demo_common::L("拍照延时中，请先停止", "Stop photo timelapse first") << std::endl;
                break;
            }
            photo::g_capturing = false; // 同上，纠正过期标志
            video::run();
            break;
        case 3: capture_settings::run(); break;
        case 4: status::run(); break;
        case 5: file_demo::run(); break;
        case 6: preview::run(); break;
        case 7: system_demo::run(); break;
        case 8: {
            std::cout << demo_common::L("正在断开并重新检测设备...", "Disconnecting and rediscovering...") << std::endl;
            g_cam->Close();
            auto cam = demo_common::openCamera(log_level, &g_cameraType,
                                               demo_common::resolveLogPath(log_file_arg, demo_common::exeDir(argv[0])));
            // 重连失败时不要把 g_cam 置空——后续菜单项全都会解引用它。
            if (!cam) {
                std::cerr << demo_common::L("重连失败，退出", "Reconnect failed, exiting") << std::endl;
                g_cam.reset(); // 同下方正常退出：不把销毁留到静态析构期
                return -1;
            }
            g_cam = cam;
            break;
        }
        default: std::cout << demo_common::L("无效", "invalid") << std::endl; break;
        }
    }

    g_cam->Close();
    g_cam.reset();
    return 0;
}