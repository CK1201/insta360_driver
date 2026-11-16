# Insta360 相机驱动

本仓库统一放置 X5 和 GO 3S 驱动，是 CoMM 的 `realworld/driver/insta360_driver` Git 子模块。两套驱动独立构建和运行，各自保留 YAML、采集输出、依赖及 Python 包。操作步骤集中在 [commands.md](commands.md)。

| 目录 | 连接与功能 | 文档 |
|---|---|---|
| [x5](x5) | X5 → USB → Ubuntu；双鱼眼、六轴、曝光/白平衡、低延迟解码、原始保存 | [README](x5/README.md)、[commands](x5/commands.md) |
| [go3s](go3s) | GO 3S → Wi-Fi → Android 手机 → USB/ADB → Ubuntu；单镜头视频、相机六轴、曝光/白平衡和原始保存 | [README](go3s/README.md)、[commands](go3s/commands.md) |

两端均提供 Python `Reader.latest()`、`read(timeout=秒)` 和独立可写的 NumPy BGR 图像。包名分别为 `insta360_x5`、`insta360_go3s`，可安装在同一 Python 环境。图像时间补偿由各自 YAML 的 `timing.sensor_delay_s` 设置，当前均为 0；原始设备与主机时间戳保留。

当前代码版本为 X5 0.8.1、GO 3S 0.4.1。从各自目录启动时，可执行文件名和 `--config config/<型号>.yaml` 的用法保持不变。

构建目录、虚拟环境、Android 工具链/私有凭据、APK 和采集输出均被忽略，迁移保留其本地内容。X5 的 x86_64 CameraSDK 是有意纳入源码的依赖，位置及许可见 [SDK 说明](x5/third_party/CameraSDK-2.2.4-linux-x86_64/README.md)。GO 3S 的 Maven 访问配置仍只保存在忽略文件中。第三方 SPS helper 的 MIT 归属分别随两套驱动保留，本仓库不另行授予 SDK 许可。

Python 消费允许跳过旧画面；原始保存仍按有界队列排空已接受数据。补偿时间是主机时钟下的图像时间估计，不能代表已标定曝光时刻或硬同步。相机能力、采样单位、分辨率和既往实机验收范围见各型号文档。
