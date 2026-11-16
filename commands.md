# 构建与运行入口

以下命令从 `insta360_driver` 仓库根目录执行。依赖安装、USB 权限及 Android 手机准备分别见 [X5 操作手册](x5/commands.md)、[GO 3S 操作手册](go3s/commands.md)。

## X5

```bash
cmake -S x5 -B x5/build
cmake --build x5/build -j4
ctest --test-dir x5/build --output-on-failure
./x5/build/insta360_x5_client --config x5/config/x5.yaml --check-config
./x5/build/insta360_x5_client --config x5/config/x5.yaml
```

也可以先进入 `x5/`，沿用 `./build/insta360_x5_client --config config/x5.yaml`。输出目录仍相对于 YAML 文件解析，默认保存到 `x5/outputs`。

## GO 3S

先让手机连接 GO 3S 热点并启动桥接 App：

```bash
cmake -S go3s -B go3s/build
cmake --build go3s/build -j4
ctest --test-dir go3s/build --output-on-failure
./go3s/build/insta360_go3s_client --config go3s/config/go3s.yaml --check-config
./go3s/build/insta360_go3s_client --config go3s/config/go3s.yaml
```

进入 `go3s/` 后也可沿用该项目原命令；保存默认位于 `go3s/outputs`。电脑退出只结束本次采集，手机继续保持相机控制连接。

若需重新准备 Android SDK，显式指定用户持有的 ZIP，相关凭据不进入 Git：

```bash
cd go3s
python3 scripts/setup_android.py --sdk-zip /absolute/path/to/Android-SDK-2.2.6.zip
scripts/build_android.sh
```

## 共同 Python 环境

以下命令从仓库根目录执行，新建环境前先按两份型号手册准备系统依赖：

```bash
python3 -m venv .venv
.venv/bin/python -m pip install ./x5 ./go3s
.venv/bin/python x5/examples/read_frames.py --config x5/config/x5.yaml
.venv/bin/python go3s/examples/read_frames.py --config go3s/config/go3s.yaml
```

两条取帧命令分别使用相应相机，正常退出后再运行下一条。不要同时用 CLI 和 Python 独占同一相机。

本机迁入的 `.venv-python312` 也可继续使用，两个包已从新目录重新安装：

```bash
.venv-python312/bin/python -c 'import insta360_x5, insta360_go3s; print(insta360_x5.__version__, insta360_go3s.__version__)'
.venv-python312/bin/python x5/examples/read_frames.py --config x5/config/x5.yaml
.venv-python312/bin/python go3s/examples/read_frames.py --config go3s/config/go3s.yaml
```
