# 构建、连接与采集

以下命令在 GO 3S 项目根目录执行。功能和时间戳限制见 [README.md](README.md)。

## 1. 电脑依赖

Ubuntu 24.04：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config python3 adb openjdk-21-jdk \
  libyaml-cpp-dev nlohmann-json3-dev libopencv-dev \
  libavcodec-dev libavutil-dev libswscale-dev ffmpeg
```

## 2. Android 工具链和 APK

准备用户提供的 `Android-SDK-2.2.6.zip`，通过必填的 `--sdk-zip` 指定其位置：

```bash
python3 scripts/setup_android.py --sdk-zip /absolute/path/to/Android-SDK-2.2.6.zip
scripts/build_android.sh
```

脚本从 ZIP 读取 SDK Maven 配置并存入被忽略的 `android/sdk-credentials.properties`，权限为0600，不打印凭据。Android 编译工具、平台及 Gradle 缓存均在项目 `.local/`，不修改系统 SDK。首次运行需要联网下载，SDK 库也从 ZIP 指定的仓库获取。

网络需要 HTTP 代理时，在准备工具链时显式提供本机代理地址，例如：

```bash
python3 scripts/setup_android.py --sdk-zip /absolute/path/to/Android-SDK-2.2.6.zip \
  --proxy http://127.0.0.1:7890
scripts/build_android.sh
```

生成的 APK 位于 `build/apk/go3s-bridge-debug.apk`。仅替换 Maven 凭据文件可使用 `--credentials-only`，该选项不安装工具链。

## 3. 手机准备与 APK 安装

小米 6 开启开发者选项和 USB 调试，用数据线连接 Ubuntu，并在手机上允许此电脑调试。检查：

```bash
adb devices -l
```

把变量替换为 **手机** 的 ADB 序列号，不要选择 GO 3S 自身的 ADB 设备：

```bash
go3s_phone_serial=PHONE_ADB_SERIAL
adb -s "$go3s_phone_serial" shell getprop ro.product.model
adb -s "$go3s_phone_serial" shell getprop ro.build.version.sdk
adb -s "$go3s_phone_serial" shell getprop ro.product.cpu.abilist
adb -s "$go3s_phone_serial" install -r build/apk/go3s-bridge-debug.apk
adb -s "$go3s_phone_serial" shell am start -n com.starlab.go3sbridge/.MainActivity
```

要求系统 API 至少28，并包含 `arm64-v8a`。MIUI 可能要求开启“USB 安装”或在手机上确认安装；按系统提示完成。若 ADB 显示 unauthorized，解锁手机允许调试；若显示 no permissions，先解决该手机 USB 节点的普通用户访问权限，不给 GO 3S 的 ADB 节点套用手机选择逻辑。

在手机 Wi-Fi 设置中连接 GO 3S 热点，打开桥接 App，授予其请求的必要权限并点击“连接”。需要时在相机上确认第三方连接授权。停止其他正在控制该相机的应用后再开始采集。

MIUI 建议允许此 App 自启动、将省电策略设为无限制，并在最近任务中锁定 App。桥接使用前台服务；第一轮测试保持手机解锁，稳定后再验证锁屏行为。手机不解码视频，电脑负责预览。

## 4. 构建电脑客户端

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/insta360_go3s_client --help
./build/insta360_go3s_client --config config/go3s.yaml --check-config
```

四组电脑端离线测试包括 low_latency、synthetic_unit、synthetic_adb_selection、synthetic_bridge_integration，使用合成编码流和模拟桥接端，不连接相机；测试通过不能替代实机验收。`--check-config` 不调用 ADB 或访问设备。

## 5. 实时预览与参数

手机 App 准备就绪后：

```bash
./build/insta360_go3s_client --config config/go3s.yaml
```

v0.2.0 的相同命令会为 GO 3S 的完整 I/P-only H.264 回调启用低延迟解码，无需新的 YAML 参数或重装手机 App。原始录制内容不变；在窗口的 host decode delay 和保存的 frame_timing.csv 中查看电脑收包到解码就绪耗时，不把它当作相机到电脑画面的总延迟。摘要新增 preview_decode_mode、preview_sps_patches。

客户端自动选择唯一符合要求的已授权手机，并建立 ADB 本地转发；存在多个手机时，在 YAML 的 `bridge.adb_serial` 指定目标。电脑本地端口由 `bridge.port` 决定，手机端口固定为18765。无需 Wi-Fi 路由转发或 RTMP 服务器。

省略字段时默认自动曝光和自动白平衡，开启 IMU/预览但关闭保存；当前实际参数以你编辑的 YAML 为准。手动曝光需同时设置 `mode: manual`、ISO、`shutter_s`；优先模式只填对应的一项。支持值取决于相机实际能力，启动时重新读回确认。更改 YAML 后重启，不在采集中热更新。

画面中的实际尺寸和帧率来自接收数据，GO 3S 预览不一定等于机内录像分辨率。不要套用 X5 的双鱼眼正方形分辨率或500Hz假设，也不要自动扫描其他模式。

## 6. 保存与实机验收

复制配置，保持源示例不变：

```bash
cp config/go3s.yaml config/go3s_record.yaml
```

将副本的 `output.save` 改为 true；后台运行时另将 `preview.enabled` 改为 false。然后：

```bash
./build/insta360_go3s_client --config config/go3s_record.yaml --duration 20
```

验收使用20秒墙钟目标，为参数初始化留出时间；最终应从保存时间戳确认视频与相机IMU的有效数据跨度均至少10秒，而不能只看运行时长。

输出路径相对于 YAML 所在目录，默认位于项目 `outputs/`。检查终端与窗口中的视频 FPS、相机 IMU 样本速率、曝光、参数读回和退出摘要。没有已启用的六轴数据时不能记录为完整验收；曝光缺失单独告警。

按 q/Esc、关闭窗口、Ctrl+C 或达到时长后正常结束，等待手机停止预览及已接受的数据排空。手机继续保持与 GO 3S 的控制连接和心跳，不主动断开相机，避免 GO 3S 关闭 Wi-Fi。下一次直接运行同一电脑命令即可，无需再次点击手机“连接”。

只有明确需要释放相机时，才点击手机“断开相机”。USB 断开也只结束电脑采集，不主动释放仍正常的相机 Wi-Fi 会话。真正的相机掉线或重启仍报错停止，不自动重连相机或尝试其他模式。

## 7. 文件与故障检查

按程序打印的目录填写实际视频路径和编码扩展名：

```bash
go3s_capture=/absolute/path/to/go3s/outputs/actual_run
cat "$go3s_capture/summary.yaml"
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height \
  -of default=noprint_wrappers=1 "$go3s_capture/video.h264"
ffmpeg -v error -i "$go3s_capture/video.h264" -f null -
```

- 找不到手机：先检查 ADB 授权、系统版本、架构及 `bridge.adb_serial`；相机自身 ADB 不作为手机使用。
- 无法连接桥接：确认 App 已点击连接、手机进程未被 MIUI 停止、本地转发端口未被占用。
- 无法连接 GO 3S：确认手机连接相机热点、相机已授权且没有其他应用独占会话。
- 参数不支持：按相机返回的能力与读回修正 YAML，不自动降级参数。
- 无 GYRO：保留诊断和原始数据，启用 IMU 时本次验收失败；若明确只使用视频，可自行设置 `stream.imu: false`，不能用手机 IMU 替代。
- 重开预览时开头缺 PPS：查看摘要的 `startup_units_skipped_for_preview`；已观察到缓存 P 帧先于 SPS/PPS/IDR，原始包完整保留，初始化后的视频仍可解码。不要重写原始采集数据。
- 队列、序号、写盘或解码错误：本次采集失败，保留已有数据；先查看摘要和日志，不把部分文件当成完整采集。

低延迟路径如果报告 B-frame 或完整包输入错误，本次采集会失败并保留已接受的原始数据；此优化仅适用于已验证的 GO3S 完整 I/P H.264 回调。H.265和通用分片接口仍使用parser，不向它们应用SPS修正。

## 8. Python 安装与取帧

Python 绑定需要开发头文件和虚拟环境工具；Ubuntu 上先安装：

```bash
sudo apt install -y python3-dev python3-venv
```

先完成电脑系统依赖与手机准备。Python 3.9+ 环境中安装本项目；构建时仍需上文列出的 C++/FFmpeg/OpenCV/yaml-cpp 开发库，wheel 不打包这些系统动态库：

```bash
python3 -m venv .venv
.venv/bin/python -m pip install .
.venv/bin/python examples/read_frames.py --config config/go3s.yaml
```

示例通过 `with Reader(...)` 取新帧，YAML 的 `output.save` 和 `runtime.duration_s` 继续生效。Python 始终不打开 OpenCV 窗口，因此无需修改 `preview.enabled`。Ctrl+C 由 Python 调用方处理，退出上下文后排空电脑采集，手机仍保持相机连接。需要单次立即查看时使用 `reader.latest()`；需要新帧时使用 `reader.read(timeout=1.0)`。图像为 BGR，交给期望 RGB 的库时由调用方明确转换。

开发绑定及完整离线测试：

```bash
.venv/bin/python -m pip install '.[test]' pybind11
cmake -S . -B build-python -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGO3S_BUILD_PYTHON=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python" \
  -Dpybind11_DIR="$(.venv/bin/python -m pybind11 --cmakedir)"
cmake --build build-python -j4
ctest --test-dir build-python --output-on-failure
```

启用绑定时新增 `python_reader` 组，使用模拟 ADB/手机和真实 H.264/H.265 编码流验证取帧、超时、跳帧、游标、GIL/停止唤醒、错误、重启、图像所有权及原始保存。测试不调用真实 ADB、不接触手机或相机。未安装 Python 绑定时仍只有原来的四组离线测试。

## 9. 图像时间补偿

示例 YAML 已包含以下字段，当前保持 0：

```yaml
timing:
  sensor_delay_s: 0.0  # 秒，0 不补偿
```

需要按独立测量结果修正时再改成对应秒数，例如 120 ms 对应 `0.120`。修改后重新启动，Python 的 `frame.timestamp_ns` 与 `frame_timing.csv` 末列使用“电脑完整收包时间减去延迟”的结果，原始收包、解码、手机与设备时间戳保留。该配置只在电脑生效，无需重装桥接 APK。

源代码更新后，更新原 CLI 和当前 Python 环境的安装包，再离线核对配置：

```bash
cmake -S . -B build
cmake --build build -j4
.venv/bin/python -m pip install --force-reinstall --no-deps .
./build/insta360_go3s_client --config config/go3s.yaml --check-config
```

已有旧 YAML 可省略 `timing`，默认按 0 处理；更改补偿值只需正常退出并重新启动电脑程序，手机保持 GO3S 连接。CSV 新列加在末尾，原列名与含义保留。

仅打包 wheel：

```bash
.venv/bin/python -m pip wheel --no-deps . -w dist
```
