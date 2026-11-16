#include "x5/session.hpp"
#include <csignal>
#include <iostream>
#include <stdexcept>
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
        return x5::run_session(config, [] { return interrupted != 0; }).code;
    } catch (const std::exception& error) {
        std::cerr << "错误：" << error.what() << '\n';
        return 1;
    }
}
