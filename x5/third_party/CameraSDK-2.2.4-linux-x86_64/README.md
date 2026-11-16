# Insta360 CameraSDK 2.2.4 Linux x86_64

本目录来自用户提供的 `CameraSDK-2.2.4-20261104_140905-linux-x86_64` 发行包，随客户端仓库一起提供。当前仅保留 X5 客户端构建与运行需要的 8 个原始文件，内容保持不变；本说明为项目新增文件。

该包仅适用于 Linux x86_64，包含 CameraSDK，不含 MediaSDK 或 ARM 版本。客户端构建流程面向 Ubuntu 24.04 x86_64；SDK 官方标称环境为 Ubuntu 22.04。

| 路径 | 内容 |
|---|---|
| `include/camera` / `include/stream` | CameraSDK C++ 头文件 |
| `lib/libCameraSDK.so` | x86_64 动态库 |
| `bin/jsons/camera_conf_Insta360_X5.json` | X5 原始相机能力表 |

未使用的 SDK 演示程序、示例源码和其他机型能力表已移除。此桌面客户端仅选择 X5；GO 3S 使用另一套 Android SDK。这里不再提供完整 SDK 示例发行目录。

客户端默认从本目录查找 SDK，并将构建目录的 `jsons` 链接到这里的 `bin/jsons`。构建与运行步骤、可选外部 SDK 覆盖见 [commands.md](../../commands.md)；项目功能与数据合同见 [README.md](../../README.md)。运行时需保留所选 SDK 的动态库及能力表，客户端还依赖系统 OpenCV、FFmpeg 和 yaml-cpp 等库。

CameraSDK 由 Insta360 提供，其许可不由本项目重新授予；本目录不声明 MIT 或其他开源许可。接口参考 [官方 CameraSDK 文档](https://insta360develop.github.io/Insta360-Developer_Docs/ch/x/desktop/camera/)。客户端的 H.264 SPS helper 是独立组件，其来源及 MIT 许可见 [SPS helper 说明](../insta360_driver/README.md)，不适用于本 SDK。
