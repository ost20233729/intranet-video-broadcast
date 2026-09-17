# 面向信创环境的内网视频广播系统

基于 Linux、C++、Qt 的内网视频广播系统，面向信创终端环境下的电子教室、政企培训室、内网会议室等场景：一台发送端将摄像头画面、内置测试画面或本地视频通过 RTP/UDP 分发到多台接收端，同网段终端可实时收看。系统使用 OpenCV 采集画面、FFmpeg 编码与传输、Qt 提供界面与显示。

## 两端分工

- **发送端 broadcaster**：OpenCV 采集（摄像头 / 内置测试画面 / 本地视频文件）→ libswscale 转 YUV420P → libx264 软编码 H.264 → FFmpeg rtp muxer 推 UDP 流（支持单播与组播）。
- **接收端 viewer**：读取发送端生成的 `broadcast.sdp` → FFmpeg rtp demuxer 收流 → H.264 解码 → Qt 显示。

编码、传输相关参数统一收在 `common/params.h`（分辨率 640×480、30fps、2 Mbps、GOP 60、端口 5004、PT 96、组播地址 239.255.0.1），作为两端的唯一参数源。

## 设计要点

- **编码侧**：libx264 `preset=ultrafast` 且不编 B 帧，编码耗时最小化；GOP 60（约 2 秒一个关键帧）让中途加入的接收端最多等一个关键帧间隔即可出画面，断流后 2 秒内自恢复。
- **传输侧解耦**：UDP 组播分发，发送端负载与接收端数量解耦——无论 10 台还是 100 台接收端，发送端只推一份流。
- **参数集设计**：SPS/PPS 经 SDP `sprop-parameter-sets` 传递而不随 RTP 流重复发送，减少带宽占用；接收端通过 FFmpeg SDP demuxer 自动完成解码器初始化。

## 目录

```
common/      两端共享参数（header-only 库 vb_common）
broadcaster/ 发送端源码（Qt GUI + 命令行两种形态）
viewer/      接收端源码（Qt GUI）
CMakeLists.txt
```

## 依赖

viewer 只需要 Qt 与 FFmpeg；broadcaster 额外依赖 OpenCV。

```bash
# 编译器与构建
sudo apt install g++ cmake pkg-config

# FFmpeg 开发库（两端都要）
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev

# OpenCV（仅发送端）
sudo apt install libopencv-dev

# Qt，任选一个大版本（只需 Widgets 模块）
sudo apt install qt6-base-dev          # Ubuntu 22.04+
# 或 qtbase5-dev（Ubuntu 20.04）；也支持手动安装的 Qt6（CMake 会搜索 ~/Qt）
```

只编接收端 viewer 的机器可以不装 OpenCV。

## 构建

```bash
cmake -S . -B build
cmake --build build -j4
# 产物：build/broadcaster/broadcaster、build/viewer/viewer
```

若 Qt 装在非默认路径，需要在配置阶段指定前缀：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt
```

## 单机自测（无摄像头也可）

```bash
# 端 A —— 推流（二选一）
./build/broadcaster/broadcaster                                   # GUI，点"开始"即推组播测试画面
./build/broadcaster/broadcaster -s test -f rtp://239.255.0.1:5004 -t 60   # CLI

# 发送端运行约 2 秒后，会在工作目录生成 broadcast.sdp；随后在端 B 收流
./build/viewer/viewer broadcast.sdp
```

无显示环境（SSH）自测：

```bash
QT_QPA_PLATFORM=offscreen ./build/viewer/viewer -t 10 broadcast.sdp
# 终端打印 "[viewer] 已解码 N 帧"、N>0，即收流-解码链路正常
```

发送端 CLI 常用选项：

| 选项 | 说明 |
|---|---|
| `-s 0` / `-s test` / `-s 文件` | 采集源：摄像头 / 内置测试画面 / 视频文件（默认摄像头） |
| `-f out.mp4` / `-f rtp://IP:端口` | 输出：录制到文件 / 推 RTP/UDP 流（默认 `out.mp4`） |
| `-t 秒` | 运行时长；`0` 为持续，Ctrl+C 停止 |

发端 GUI 无头自测：`QT_QPA_PLATFORM=offscreen ./build/broadcaster/broadcaster --autostart -t 10`。

## 两台虚拟机组播联调

前提：两台 VM 二层互通，且所在虚拟网络支持组播转发。

| 环境 | 推荐配置 |
|---|---|
| VMware | 仅主机(Host-only) / 自定义 VMnet；NAT 同网段一般也可 |
| VirtualBox | 仅主机 / 内部网络 / Bridge；NAT 不转发组播，避开 |

先 `ping` 确认网络互通。

发送端推流（GUI 或 CLI，一次操作）:

```bash
./build/broadcaster/broadcaster -s test -f rtp://239.255.0.1:5004 -t 600
```

运行约 2 秒后当前目录生成 `broadcast.sdp`（含组播地址与 H.264 参数集），把这份文件拷到各接收端：

```bash
scp <发送端IP>:<path>/broadcast.sdp ./
```

接收端收流：

```bash
./build/viewer/viewer broadcast.sdp
```

判定：接收端数秒内出现发送端测试画面即成功。因为组播/ UDP 没有回执，中途加入的接收端要等下一个关键帧（约 2 秒）才开始出画面，属正常。

虚拟网络不支持组播时，可改用单播验证：发送端改用 `-f rtp://<接收端IP>:5004` 并把对应的 SDP 拷给接收端即可，无需改动接收端程序。

## 常见问题

- viewer 一直"等待信号"、打不开 SDP：先确认正在运行的是重新编译后的二进制（旧版本会因为没有 `protocol_whitelist` 而报 `Protocol 'rtp' not on whitelist`），再核对组播/单播是否与 SDP 中地址一致。
- 组播收到不到画面：多为虚拟网络不支持组播，换 Host-only / Bridged，或临时用单播验证。
- UDP 被防火墙拦截：`sudo ufw allow 5004/udp`。
- 两端可分别用 Qt5 / Qt6，程序均支持；同一台机器别混装两套 Qt。

## 参考笔记

- FFmpeg API 使用：https://www.cnblogs.com/linuxAndMcu/category/1613476.html
- FFmpeg 推流示例：https://blog.csdn.net/ihungry/article/details/136742898



