# insta360_x5_client

Linux 上的 Insta360 X5 C++17 客户端，通过 USB 获取原始双鱼眼视频、六轴 IMU、曝光和设备状态。启动时读取 YAML，设置并读回相机参数；支持桌面预览、后台保存和定时退出。

安装、构建、USB 权限、运行与文件检查统一见 [commands.md](commands.md)。配置示例见 [config/x5.yaml](config/x5.yaml)，版本记录见 [CHANGELOG.md](CHANGELOG.md)。

## 功能与依赖

- 两个镜头始终启用（`SENSOR_DEVICE_ALL`），每镜头原生正方形边长支持 960、1280、1920，帧率固定为 30 fps。
- 预览面板显示双鱼眼画面、六轴 IMU、请求与启动读回参数、实际尺寸、实时曝光、电量和接收速率。预览不在原始视频或 CSV 上叠字。
- 按流保存相机原始 H.264/H.265 字节和包索引，另存 IMU、曝光、状态、主机处理时间与结束摘要。
- X5 完整 I/P-only H.264 包使用低延迟直解路径，只在预览副本中修正 SPS；通用字节流、分片 H.264 和 H.265 保留 FFmpeg parser 路径。预览从可初始化关键帧开始，前置旧帧仍原样保存。
- 视频/IMU/曝光回调进入 64 MiB 队列，工作线程保存并解码，主线程显示最新画面，独立线程查询状态。正常退出会排空已接受数据；解码失败后仍在存储可写时保存已接受数据，运行标记为失败。
- Python `insta360_x5.Reader` 提供 NumPy 最新帧拉取，复用同一相机配置、读回、解码、原始保存与退出流程。

仓库已内置 [CameraSDK 2.2.4 Linux x86_64](third_party/CameraSDK-2.2.4-linux-x86_64/README.md)，克隆后安装系统依赖即可按 [commands.md](commands.md) 构建和运行，无需另行下载 SDK。系统依赖为 CMake、C++17 编译器、yaml-cpp、OpenCV、FFmpeg 开发库及 Python 3（离线测试）；使用 CPU 解码，无需 MediaSDK 或 GPU。

构建流程面向 Ubuntu 24.04 x86_64。既往实测环境为 Ubuntu 24.04.5 x86_64、GCC 13、CameraSDK 2.2.4、yaml-cpp 0.8、FFmpeg 6、OpenCV 4.6，X5 固件为 v1.13.21。CameraSDK 官方标称环境为 Ubuntu 22.04；其他 SDK、系统或固件组合需自行验证。本仓库不提供 ARM、Windows 或 macOS 的构建包。

## 模块结构

| 位置 | 职责 |
|---|---|
| `src/main.cpp` | CLI 与进程信号处理 |
| `src/session.cpp` / `include/x5/session.hpp` | CLI 与 Python 共用的采集流程、状态查询、会话与摘要文件 |
| `src/reader.cpp` / `python/bindings.cpp` | 后台会话、每流最新帧槽与 NumPy 接口 |
| `src/config.cpp` / `include/x5/config.hpp` | YAML 解析、默认值与本地校验 |
| `src/camera.cpp` / `include/x5/camera.hpp` | USB 会话、双镜头模式、参数下发与读回 |
| `src/capture.cpp` / `include/x5/capture.hpp` | 受限队列、原始保存、包索引与几何检查 |
| `src/decoder.cpp` / `include/x5/decoder.hpp` | H.264/H.265 解码及低延迟路径 |
| `src/preview.cpp` / `include/x5/preview.hpp` | 预览面板合成 |
| [config](config/x5.yaml) / [tests](tests) | 配置示例、六组既有回归与新增 Reader/Python 回归 |
| [third_party/CameraSDK-2.2.4-linux-x86_64](third_party/CameraSDK-2.2.4-linux-x86_64/README.md) | 内置 CameraSDK 动态库、必需头文件和 X5 能力表 |
| [third_party/insta360_driver](third_party/insta360_driver/README.md) | SPS helper 来源与 MIT 许可 |

## 配置合同

配置只在启动时读取，更改后需重启。`--config` 必填；`--duration` 覆盖 `runtime.duration_s`；`--check-config` 只校验并显示有效配置，不访问相机或创建采集目录。未知或重复 YAML 字段会报错。

| 配置 | 省略时默认值与约束 |
|---|---|
| `camera.serial` | 空字符串；此时必须恰好有一台可访问的 X5，多台时指定序列号 |
| `camera.exposure.mode` | `auto`；还支持 `full_auto`、`manual`、`iso_priority`、`shutter_priority` |
| `camera.exposure.iso` / `shutter_s` | 默认均为空；自动模式均省略或设为 `null`，manual 两项必填，两种优先模式只填对应项。ISO 为正数，快门为正秒数且不长于 `1/fps`；设备支持值在启动时核对 |
| `camera.exposure.ev` | 0；范围 [-4, 4]、步长 0.05，只有自动模式允许非零 EV |
| `camera.white_balance_kelvin` | 0（自动）；手动范围 2000–10000 K，具体支持值由设备能力决定 |
| `stream.resolution` / `fps` | 960 / 30；分辨率是两个镜头各自的原生边长，只接受 960/1280/1920 和 30 fps |
| `stream.bitrate_bps` / `imu` | 8000000 bit/s / true；码率须为正 32 位无符号整数，音频和 LRV 关闭 |
| `preview.enabled` | true；false 时无窗口，仍解码并检查实际尺寸 |
| `output.save` / `directory` | true / `../outputs`；相对路径以 YAML 所在目录为基准，每次保存创建独立子目录 |
| `runtime.duration_s` / `status_interval_s` | 0 / 1 秒；时长 0 表示持续运行，状态间隔须为正数 |
| `timing.sensor_delay_s` | 默认 0.0 秒；有限非负值，转换为整数纳秒后用于图像时间标签补偿 |

上表是 `Config` 的默认值。仓库当前示例明确使用手动 ISO 200、快门 0.01 秒、白平衡 4600 K、每镜头 960、30 fps、预览开启、保存关闭；这些值优先于默认值。

双镜头合并传输尺寸分别为 1920×960、2560×1280、3840×1920。程序设置 X5 直播预览子模式和原生编码尺寸，再同步读回并逐帧核对实际几何；原始采集不缩放，预览面板单独缩放显示。720 未验证生效；SDK 列出的 1080 档在本机输出 2160×1088 且无裁剪信息，因此两者均拒绝。其他帧率在访问相机前拒绝。

参数或原生尺寸/FPS 枚举读回不一致时停止，不替换请求值。确认目标尺寸后再出现尺寸偏离会持续判为失败；超过 10 秒没有目标尺寸、视频、可解码帧或已启用的 IMU 也会失败。曝光回调缺失单独告警并记入摘要。相机设置可能在退出后保留，程序不恢复旧曝光；运行期间请保持相机由本程序独占。

## 数据文件与时钟

`output.save: true` 时，在程序打印的采集子目录保存以下文件：

| 文件 | 内容 |
|---|---|
| `config_source.yaml` / `config_effective.yaml` | 原始 YAML / 含默认值、CLI 覆盖及绝对输出路径的有效配置 |
| `session.yaml` | 型号、序列号、固件、SDK、参数读回、SDK 返回的标定字段及主机时钟锚点 |
| `video_stream_<index>.h264` / `.h265` | 每路相机原始编码字节，扩展名按实际编码选择 |
| `video_packets.csv` | 包序号、流编号、设备时间戳、主机回调接收时间、文件偏移与长度 |
| `imu.csv` | 样本序号、设备时间戳、主机接收时间、ax/ay/az/gx/gy/gz；同批样本共用回调接收时间 |
| `exposure.csv` | 设备时间戳、主机接收时间及曝光秒数；不含完整逐帧 ISO/WB |
| `status.csv` | 连接、电量、电源、存储原值、解码帧率、IMU 到达率和最新曝光；查询失败字段留空 |
| `frame_timings.csv` | 帧序号、流编号及主机单调时钟下的回调接收、解码/色彩转换完成时刻（ns）；末列 `timestamp_ns` 为补偿后的图像估计时间 |
| `summary.yaml` | 退出原因、错误、`capture_valid`、数据计数、尺寸检查、启动跳过预览单元及主机解码耗时 |

设备时间戳和 IMU 值保持 CameraSDK 原值，不套用 MediaSDK 的微秒或 SI 单位注释。既往 X5 v1.13.21 稳定段约为 1000 tick/s，IMU 相邻样本约 2 tick、约 500 Hz，加速度模长约 1；物理量纲、轴方向、gyro 单位及相机–IMU 时差尚未标定。启动视频可能带 0 或上次取流的旧时间戳，记录不作隐式修正。

`host_receive_steady_ns` 是主机回调到达时间，时钟锚点仅供近似关联。面板的 `SDK -> decoded` / `SDK -> GUI prep` 只覆盖主机接收后的处理阶段，不是曝光到屏幕总延迟。码流标称帧率可能与实际约 30 fps 不同，应结合记录的时间戳和接收速率分析时序。`capture_valid` 只表示采集的数据与保存检查通过，不保证精确达到请求帧率或硬同步。

## Python 拉帧接口

`insta360_x5.Reader(config=...)` 构造时只校验 YAML；`start()` 异步启动，进入 `with` 时自动启动，退出时 `stop()`。Python 固定不创建 OpenCV 窗口、不读取 stdin、不接管进程信号。相机参数、保存开关、保存目录及运行时长仍按 YAML 生效，原始配置文件不修改。

| 接口 / 字段 | 合同 |
|---|---|
| `latest(stream_index=0)` | 立即返回最新 `Frame`；尚无帧时为 `None`，读取会推进该流游标 |
| `read(timeout=1.0, stream_index=0)` | 等待比游标新的帧；秒为单位，`None` 无限等待，`0` 立即检查 |
| `Frame.image` | 独立可写 NumPy `uint8[H,W,3]` BGR 副本；后续帧和关闭不改变已返回图像 |
| `Frame.index` / `stream_index` | 每流序号从 1 起，慢消费者可能跳号；重新启动后归 1；保留 SDK 流编号 |
| `host_receive_ns` / `host_decode_ns` | 本机单调时钟纳秒：关联输入的接收时间 / BGR 就绪时间；通用分块流无法关联 PTS 时接收时间为 `0` |
| `timestamp_ns` | `host_receive_ns - round(timing.sensor_delay_s × 1e9)`；收包时间未知时为 `0` |
| `running` / `error` | 后台会话是否运行 / 原始错误字符串，无错误时 `None` |
| `stop()` / `close()` | 幂等；唤醒等帧者，停止取流并排空已接受的保存数据；可再次 `start()` |

超时抛 `TimeoutError`；正常结束且没有未读帧时抛 `StopIteration`；后台失败由 `read/latest` 抛 `RuntimeError`，也可查 `error`。启动异步错误在等帧时报告。等待与停止释放 GIL，原生生产线程不执行 Python 回调。每个 Reader 每流一个消费游标；多个线程共享它，一个物理相机应只由一个 Reader/CLI 会话使用。

每流仅缓存最新图像，原始视频、IMU 与曝光仍经完整保存队列。合并画面为 `H=resolution, W=2×resolution`，两镜头均保留；接口不缩放或裁剪。Python 帧序号和 CSV 中的索引分别定义，不能假定二者相同。接口不把主机时间当曝光时间；设备原始时间戳保留在包索引，尚未建立逐解码帧的可靠设备时间关联。

安装、示例调用与测试步骤见 [commands.md 的 Python 部分](commands.md#9-python-接口)，示例代码见 [read_frames.py](examples/read_frames.py)。该版本 Python 返回图像及元数据；六轴和曝光继续按原有 CSV 合同保存，尚未提供 Python 传感器查询接口。

图像时间补偿与参考驱动的 [arrival − sensor_delay](https://github.com/specialist-ai/insta360_driver/blob/22641e241c18857f345fbbf8643592194beefb7f/python/insta360_driver/latency.py#L176) 同向：正值使图像时间向过去移动。默认 0 不补偿，旧配置省略该项也按 0 处理。CLI 保存与 Python `read/latest` 使用同一个补偿结果，不在消费时再次减去延迟；原始设备、收包、解码、IMU 和曝光时间均保留。该字段是主机单调时钟下的图像时间估计，不能当作已标定曝光时刻或用于证明硬同步，也不会减少实际传输延迟。

## 验证记录

六组离线回归为 `config`、`capture`、`cli`、`preview`、`low_latency`、`geometry`，覆盖配置/路径/模式、CLI、本地文件、真实编码流解码、色彩、原样保存、包索引、多路隔离、队列与退出排空、解码/写盘故障、低延迟及持续尺寸检查。它们不连接相机。

v0.7.0 额外覆盖 C++ Reader 流隔离、等待/停止/重启/错误与 Python NumPy 所有权、GIL、真实 H.264/H.265 分块解码和索引保存；开启绑定共八组 CTest。2026-11-17 的 Python 真机会话保存 389 帧 `1920×960`、6512 条六轴样本、385 条曝光数据；视频完整解码，原始包偏移/长度与解码计数一致。新帧序号、慢消费者跳帧、图像独立性和结束后的图像有效性均已检查。

安装后的 wheel 也在独立会话中采集并核对通过，确认运行加载安装目录内的 CameraSDK 动态库。自动释放最后一个 Reader 引用时的 GIL 行为另有延迟退出回归覆盖；此检查不访问相机。

既往在上述本机环境实测三档原生尺寸、约 30 fps 视频和约 500 Hz IMU，以及手动参数读回、预览、后台保存和正常退出。v0.6.0 的验证范围为本机与干净克隆中的构建和离线检查，没有新增实机采集。

本项目面向单台 X5 USB 原始采集，不提供机内 SD 卡录像、全景拼接、FlowState、自动重连、网络服务、ROS 或 YAML 热更新。双鱼眼数据用于 VIO/SLAM 前仍需相机与 camera–IMU 标定；已有 SDK 标定字段和时间戳不等于完成同步精度验收。

## 内置 SDK 与第三方归属

CMake 默认使用仓库内的 `third_party/CameraSDK-2.2.4-linux-x86_64`，也支持通过 `CAMERASDK_ROOT` 显式选择外部 SDK，操作见 [commands.md](commands.md)。构建目录的 `jsons` 软链接指向所选 SDK 的配套能力表；运行时需保留该 SDK 的动态库、能力表及此链接。程序同时依赖目标系统的 OpenCV、FFmpeg、yaml-cpp 等动态库，构建结果不是可随意复制到其他系统的单文件静态程序。

CameraSDK 来自用户提供的 Insta360 2.2.4 Linux x86_64 发行包，原始文件保持不变，其许可不由本项目重新授予。来源和目录说明见 [SDK 说明](third_party/CameraSDK-2.2.4-linux-x86_64/README.md)，接口参考 [官方 CameraSDK 文档](https://insta360develop.github.io/Insta360-Developer_Docs/ch/x/desktop/camera/)。

H.264 SPS helper 派生自固定版本的 specialist-ai/insta360_driver，Copyright (c) 2026 Hybrid Robotics，按 MIT 许可保留归属和原文，见 [第三方说明](third_party/insta360_driver/README.md) 与 [MIT 许可](third_party/insta360_driver/LICENSE)。
