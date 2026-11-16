# 构建、运行与检查

除首次获取源码外，以下命令从 `x5/` 项目目录执行。配置合同、数据格式和时钟/标定限制见 [README.md](README.md)。Python 为可选接口，默认 C++ 构建与启动命令保持不变。统一仓库入口见 [根目录手册](../commands.md)。

## 1. 获取源码与依赖

Ubuntu 24.04 x86_64，先用 Git 克隆源码，再安装系统依赖。以下假设已安装 Git；未安装时先运行 `sudo apt update && sudo apt install -y git`：

```bash
git clone https://github.com/CK1201/insta360_driver.git
cd insta360_driver/x5
sudo apt update
sudo apt install -y build-essential cmake pkg-config git python3 \
  libyaml-cpp-dev libopencv-dev libavcodec-dev libavutil-dev libswscale-dev \
  ffmpeg usbutils
```

仓库内已包含 `third_party/CameraSDK-2.2.4-linux-x86_64`，默认构建直接使用它，无需另行申请、下载或解压 SDK，也无需 MediaSDK。SDK 来源与平台范围见 [SDK 说明](third_party/CameraSDK-2.2.4-linux-x86_64/README.md)。

## 2. 构建与离线检查

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/insta360_x5_client --help
./build/insta360_x5_client --config config/x5.yaml --check-config
readlink -f build/jsons
```

默认 CTest 应列出七项：`reader`、`config`、`capture`、`cli`、`preview`、`low_latency`、`geometry`；启用 Python 绑定时增加 `python_reader`。测试使用合成编码流和本地文件，不连接相机；FFmpeg 需提供 `libx264`、`libx265` 编码器。`--check-config` 不访问 USB、不创建采集目录。

CMake 默认将 `build/jsons` 链接到仓库内 SDK 的 `bin/jsons`，并为客户端设置该 SDK 的动态库路径。运行时保留构建目录、所选 SDK 及其 `jsons` 链接。可执行文件还依赖系统的 OpenCV、FFmpeg、yaml-cpp 等动态库；这些步骤不生成跨平台或单文件全静态程序，不能只复制可执行文件到任意机器运行。外部 SDK 覆盖及旧构建目录的切换见第 7 节。

## 3. 连接 X5 与 USB 权限

相机开机，用支持数据传输的 USB 线连接电脑，在相机屏幕选择 **Android 模式**，确认设备：

```bash
lsusb -d 2e1a:0002
```

普通用户需对设备有读写权限。以下规则只匹配 X5 的 `2e1a:0002`；安装规则和加入用户组需要管理员权限，程序不会自行安装。若已配置，可跳过：

```bash
sudo groupadd -f plugdev
sudo usermod -aG plugdev "$USER"
sudo tee /etc/udev/rules.d/99-insta360-x5.rules >/dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="2e1a", ATTR{idProduct}=="0002", GROUP="plugdev", MODE="0660"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger --action=change --subsystem-match=usb --attr-match=idVendor=2e1a --attr-match=idProduct=0002
```

首次加入 `plugdev` 后注销并重新登录，再重插相机并选择 Android 模式。可用 `id -nG` 确认组成员身份；根据 `lsusb` 显示的 Bus/Device 检查对应 `/dev/bus/usb/BBB/DDD` 节点应属于 `root:plugdev` 且组可读写。配置后用普通用户启动客户端。

## 4. 预览与定时采集

[config/x5.yaml](config/x5.yaml) 当前为手动 ISO 200、0.01 秒、4600 K、每镜头 960、30 fps，预览开启、保存关闭。按需编辑后先运行第 2 节的配置检查，再使用原启动命令：

```bash
./build/insta360_x5_client --config config/x5.yaml
```

只接受每镜头 `resolution: 960`、`1280` 或 `1920`，两个镜头始终保留，`fps` 必须为 30。合并尺寸依次为 1920×960、2560×1280、3840×1920。程序会读回原生尺寸/FPS、曝光与白平衡，并检查实际帧尺寸；拒绝不支持值，不自动降级。

定时运行使用 `--duration`，覆盖 YAML 中的秒数；0 表示持续运行：

```bash
./build/insta360_x5_client --config config/x5.yaml --duration 10
```

开启 `output.save` 后，程序打印完整保存目录。`output.directory` 相对于 YAML 所在目录解析；当前 `config/x5.yaml` 的 `../outputs` 对应仓库根目录的 `outputs`，每次保存使用新的时间子目录。修改配置后需重启程序。

按 q/Esc、关闭预览窗口或 Ctrl+C 正常退出，等待结束摘要与文件写完。参数设置可能保留在相机中，退出不会恢复旧曝光。运行期间避免其他程序同时控制相机。

## 5. 后台保存

保留原示例，复制一份采集配置：

```bash
cp config/x5.yaml config/x5_record.yaml
```

在 `config/x5_record.yaml` 中把 `preview.enabled` 改为 false、`output.save` 改为 true，其余值按采集要求填写并保持 `fps: 30`。无桌面环境必须明确关闭预览。以下示例后台保存 60 秒：

```bash
./build/insta360_x5_client --config config/x5_record.yaml --check-config
nohup ./build/insta360_x5_client --config config/x5_record.yaml --duration 60 >x5_capture.log 2>&1 &
echo "$!" > x5_capture.pid
```

查看进度和本次目录：

```bash
tail -f x5_capture.log
```

`tail` 的 Ctrl+C 只停止日志查看。需要提前结束仍在运行的本次采集时，向保存的进程号发送正常退出信号，再查看日志中的结束摘要：

```bash
kill -INT "$(cat x5_capture.pid)"
```

不要强制终止进程，以免来不及排空队列和写入摘要。

## 6. 检查保存文件

将 `capture_dir` 替换为程序打印的实际目录，`video_file` 按实际流编号与 `.h264` / `.h265` 扩展名填写：

```bash
capture_dir=/absolute/path/to/insta360_driver/outputs/实际采集子目录
video_file="$capture_dir/video_stream_0.h264"
cat "$capture_dir/summary.yaml"
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,r_frame_rate \
  -of default=noprint_wrappers=1 "$video_file"
ffmpeg -v error -i "$video_file" -f null -
```

检查退出码、`capture_valid`、实际尺寸、可解码帧、已启用的 IMU 样本及曝光回调。曝光缺失会单独告警，不能记为全部传感器通过。码流标称帧率不等于实际接收帧率；用 `video_packets.csv`、`imu.csv` 和主机接收速率分析时序。

既往 Ubuntu 24.04 / CameraSDK 2.2.4 / X5 v1.13.21 曾实测约 30 fps 视频与约 500 Hz IMU，本轮离线验证不构成新的实机验收。启动码流可能含缺少初始化头的旧 P 帧，外部解码器可能报告 PPS 警告；原始文件不裁剪，预览跳过数量记在 `startup_units_not_decoded`。其他解码错误仍需核查。

## 7. 可选：使用外部 CameraSDK

仅在需要替换内置 SDK 时使用此流程。将路径改为 Linux x86_64 CameraSDK 根目录；该目录必须包含 `include/camera/camera.h`、`lib/libCameraSDK.so` 和 `bin/jsons/camera_conf_Insta360_X5.json`：

```bash
camera_sdk_root=/absolute/path/to/CameraSDK-2.2.4-linux-x86_64
cmake -S . -B build -DCAMERASDK_ROOT="$camera_sdk_root"
cmake --build build -j4
ctest --test-dir build --output-on-failure
readlink -f build/jsons
```

重新配置会按所选根目录刷新 SDK 头文件、动态库及 `build/jsons` 链接。旧构建目录可能缓存了外部 `CAMERASDK_ROOT`；要恢复仓库内 SDK，清除该缓存项并重新构建：

```bash
cmake -S . -B build -U CAMERASDK_ROOT
cmake --build build -j4
ctest --test-dir build --output-on-failure
readlink -f build/jsons
```

恢复后链接目标应为当前仓库的 `third_party/CameraSDK-2.2.4-linux-x86_64/bin/jsons`。外部 SDK、系统或固件组合的实际相机行为需自行验证。

## 8. 常见问题

- **找不到相机或权限不足**：检查数据线、Android 模式、USB 节点读写权限和 `camera.serial`；`lsusb` 可见不代表 SDK 有访问权限。
- **SDK 库或能力表缺失**：检查 SDK 动态库、头文件和 X5 能力表是否齐全；使用外部 SDK 时检查第 7 节列出的文件。确认 `build/jsons` 指向所选 SDK 的 `bin/jsons`，重新配置后再构建。
- **参数拒绝或读回不一致**：检查 YAML、相机当前状态及固件能力，不绕过读回检查；正式客户端固定 30 fps，不运行额外 FPS 探测。
- **没有桌面显示环境**：将 `preview.enabled` 设为 false；删除显示环境变量不会自动关闭预览。
- **无视频/IMU、尺寸偏离、队列/写盘/解码失败**：检查终端错误和 `summary.yaml`；保留已有数据，该次运行按失败处理。客户端不会自动重连。
- **低延迟路径报告 B 帧**：此路径要求完整 I/P-only H.264，不能忽略该错误继续采集；通用分片/H.265 解码属于独立输入路径。

## 9. Python 接口

先完成第 1 节的系统依赖与 USB 权限；Python 绑定另需 Python 开发头文件和虚拟环境工具。以下在仓库的 `x5/` 目录执行：

```bash
sudo apt install -y python3-dev python3-venv
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install .
python -c 'import insta360_x5; print(insta360_x5.__version__)'
python examples/read_frames.py --config config/x5.yaml --frames 300
```

安装在本机编译扩展并带入选定 CameraSDK 的动态库，仍依赖系统 FFmpeg、OpenCV、yaml-cpp。示例默认等待 300 张消费帧，或按 YAML 时长提前结束；按 Ctrl+C 会退出并排空保存队列。Python 不打开项目自带预览窗口，取出的 `frame.image` 可直接交给 NumPy、OpenCV 或策略。不要同时运行 CLI 和 Python 争用同一相机。

可以在同一个虚拟环境再安装另一项目：进入 GO3S 目录执行 `python -m pip install .`。两个 import 名分别是 `insta360_x5` 与 `insta360_go3s`，接口中的超时均为秒。

要在 CMake 中连同所有离线回归构建可选绑定：

```bash
python -m pip install pybind11 pytest numpy
cmake -S . -B build-python -DBUILD_PYTHON=ON -DBUILD_TESTING=ON \
  -DPython3_EXECUTABLE="$(command -v python)" \
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
cmake --build build-python -j4
ctest --test-dir build-python --output-on-failure
```

默认 `BUILD_PYTHON=OFF`，原来的 `./build/insta360_x5_client --config config/x5.yaml` 无需 Python 新依赖。CMake 构建的 Python 模块在 `build-python/python`；使用该产物时先执行 `export PYTHONPATH="$PWD/build-python/python${PYTHONPATH:+:$PYTHONPATH}"`。六组原有测试、C++ Reader 测试和 Python 接口测试均为离线测试，不占用相机。

## 10. 图像时间补偿

示例 YAML 已包含以下字段，当前保持 0：

```yaml
timing:
  sensor_delay_s: 0.0  # 秒，0 不补偿
```

需要按独立测量结果修正时再改成对应秒数，例如 120 ms 对应 `0.120`。修改后重新启动，Python 的 `frame.timestamp_ns` 与 `frame_timings.csv` 末列使用“原始主机收包时间减去延迟”的结果；`frame.host_receive_ns` 和 `frame.host_decode_ns` 保持原值。它只修正时间标签，数据格式和限制见 README。

源代码更新后，更新原 CLI 和当前 Python 环境的安装包，再离线核对有效配置：

```bash
cmake -S . -B build
cmake --build build -j4
python -m pip install --force-reinstall --no-deps .
./build/insta360_x5_client --config config/x5.yaml --check-config
```

已有旧配置可省略 `timing`，默认按 0 处理。按列号读取 CSV 的程序需留意末尾新增列；按原列名读取仍可使用原来的时间字段。
