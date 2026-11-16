# 构建、运行与检查

除首次安装和克隆外，以下命令从仓库根目录执行。配置合同、数据格式和时钟/标定限制见 [README.md](README.md)。本轮 v0.5.2 发布仅做构建与离线验证；第 3–6 节供用户需要实机采集时操作。

## 1. 获取源码与依赖

Ubuntu 24.04 x86_64：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config git python3 \
  libyaml-cpp-dev libopencv-dev libavcodec-dev libavutil-dev libswscale-dev \
  ffmpeg usbutils
git clone https://github.com/CK1201/insta360_driver.git
cd insta360_driver
```

向 Insta360 申请 Linux x86_64 CameraSDK，在仓库外解压。将下列通用绝对路径替换为 SDK 根目录；其中应包含 `include/camera/camera.h`、`lib/libCameraSDK.so` 和 `bin/jsons`。既往本机使用 CameraSDK 2.2.4；无需安装 MediaSDK。

```bash
camera_sdk_root=/absolute/path/to/CameraSDK-2.2.4-linux-x86_64
```

## 2. 构建与离线检查

```bash
cmake -S . -B build -DCAMERASDK_ROOT="$camera_sdk_root"
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/insta360_x5_client --help
./build/insta360_x5_client --config config/x5.yaml --check-config
readlink -f build/jsons
```

CTest 应列出六项：`config`、`capture`、`cli`、`preview`、`low_latency`、`geometry`。测试使用合成编码流和本地文件，不连接相机；FFmpeg 需提供 `libx264`、`libx265` 编码器。`--check-config` 不访问 USB、不创建采集目录。

CMake 将 `build/jsons` 链接到外部 SDK 的 `bin/jsons`，并为客户端设置 SDK 库路径。运行时保留构建目录和 SDK，勿只复制可执行文件。更换 SDK 时使用新的构建目录并指定 `CAMERASDK_ROOT`，再检查该目录内 `jsons` 的链接目标，避免沿用旧 SDK 的缓存路径和链接。

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

## 7. 常见问题

- **找不到相机或权限不足**：检查数据线、Android 模式、USB 节点读写权限和 `camera.serial`；`lsusb` 可见不代表 SDK 有访问权限。
- **SDK 库或能力表缺失**：确认 `CAMERASDK_ROOT` 的三个目录完整、`build/jsons` 指向该 SDK 的 `bin/jsons`；重新配置后再构建。
- **参数拒绝或读回不一致**：检查 YAML、相机当前状态及固件能力，不绕过读回检查；正式客户端固定 30 fps，不运行额外 FPS 探测。
- **没有桌面显示环境**：将 `preview.enabled` 设为 false；删除显示环境变量不会自动关闭预览。
- **无视频/IMU、尺寸偏离、队列/写盘/解码失败**：检查终端错误和 `summary.yaml`；保留已有数据，该次运行按失败处理。客户端不会自动重连。
- **低延迟路径报告 B 帧**：此路径要求完整 I/P-only H.264，不能忽略该错误继续采集；通用分片/H.265 解码属于独立输入路径。
