# LAN MIC

LAN MIC 是 [suxitech](https://suxitech.cn) 的局域网双向音频桥，由 ESP32-S3 USB 音频设备与两个 JUCE VST3 插件组成。

设备通过 USB 连接手机，通过 Wi-Fi 连接电脑所在的局域网，让 DAW 的音频作为手机麦克风输入，同时把手机播放的音频送回 DAW，用于直播、通话、录音和音频处理。

## 主要功能

- **双向传输**：电脑到手机、手机到电脑可同时工作，也可独立使用。
- **USB 音频设备**：以 UAC2 耳麦形式提供麦克风输入和立体声播放通道。
- **两个独立插件**：`LAN MIC Send` 负责发送，`LAN MIC Return` 负责接收。
- **自动发现设备**：通过局域网广播发现，也可在插件中按设备名称选择。
- **双向 Opus 编解码**：固定 20 ms 帧长，两个方向都保留左右声道。
- **采样率转换**：网络和 USB 固定为 48 kHz，插件自动适配 DAW 工程采样率。
- **网页配网**：通过设备热点配置 Wi-Fi，按键可清除已保存的网络凭据。
- **跨平台插件**：提供 macOS 和 Windows 的 VST3 构建及安装包脚本。

## 系统架构

```text
电脑 DAW -> LAN MIC Send   -> 局域网 UDP -> ESP32-S3 -> USB IN  -> 手机麦克风
电脑 DAW <- LAN MIC Return <- 局域网 UDP <- ESP32-S3 <- USB OUT <- 手机播放
```

USB IN / OUT 以手机这一 USB Host 的视角命名：IN 是手机读取的麦克风音频，OUT 是手机送出的播放音频。

### 音频规格

| 项目 | 电脑到手机 | 手机到电脑 |
| --- | --- | --- |
| 插件 | LAN MIC Send | LAN MIC Return |
| 网络编码 | Opus，立体声 | Opus，立体声 |
| 码率 | 192 kbps CBR | 192 kbps CBR |
| 采样率 | 48 kHz | 48 kHz |
| 帧长 | 20 ms，每声道 960 个采样 | 20 ms，每声道 960 个采样 |
| USB 格式 | PCM16，保留左右通道 | PCM16，保留左右通道 |
| 接收端目标缓冲 | 设备端 300 ms | Return 插件 500 ms |

USB 两个方向均为 48 kHz、16 bit、双通道；USB IN 按每毫秒 192 字节供数。目标缓冲不是端到端延迟，实际延迟还包括 DAW、网络、USB 及手机应用处理。本项目优先考虑连续音频传输，不定位为超低延迟现场监听设备。

### 网络与设备

- 使用 IPv4 UDP，设备监听 `45678` 进行发现与订阅，监听 `45679` 接收电脑音频。
- 手机音频由设备从 `45679` 单播到 Return 插件登记的临时端口。
- 每个插件拥有独立的本地端口；每台设备同时支持一个 Send 和一个 Return 实例。
- 插件按设备身份保存选择，不依赖固定 IP。设备名称为 `LAN MIC-XXXXXX`。
- 固件和插件共用 SOS v9 协议，线上字段及音频常量以 [common/Protocol.h](common/Protocol.h) 为准。

## 快速使用

### 准备设备

- 一块带原生 USB 和 OPI PSRAM 的 ESP32-S3 开发板；当前固件配置使用 4 MB Flash 和 `qio_opi`。
- 支持 USB Host / UAC2 音频的手机，以及匹配的数据线或 USB 转接器。面向 Android 和 iPhone 使用，实际可用性取决于手机、转接器及应用的外置音频支持。
- 一台运行 VST3 宿主 DAW 的 macOS 或 Windows 电脑。
- 一个支持 2.4 GHz Wi-Fi 的局域网；电脑可以通过有线或无线接入。

手机应连接开发板的**原生 USB 接口**，而不是 USB 转串口接口。

### 连接 Wi-Fi

1. 为设备供电。首次使用或无法连接已保存网络时，设备启动配网热点 `LAN MIC-XXXXXX`。
2. 用手机或电脑连接该热点，在配网页面选择 Wi-Fi 并填写密码。默认配网页面地址为 `http://192.168.4.1`。
3. 配网完成后，将电脑接入同一局域网，再把设备的原生 USB 接口连接手机。
4. 更换网络时，长按 BOOT 键（GPIO0）至少 3 秒，待状态灯快速闪烁后松开，设备会清除 Wi-Fi 凭据并重启。

默认状态灯位于 GPIO18，高电平点亮；连接 Wi-Fi 时快闪，配网时慢闪，联网待机时常亮，USB 音频工作时每秒短闪一次。

### 在 DAW 中使用

1. 安装插件并让 DAW 扫描 VST3。
2. 在要发送到手机的音轨或总线上插入 **LAN MIC Send**。
3. 在另一条接收音轨上插入 **LAN MIC Return**，将其输出送到所需监听或录音通道。
4. 在两个插件的 `Device` 菜单中选择同一台设备，或使用 `Automatic (first discovered device)`。
5. 开启 `Network enabled`，通过 `Network gain` 调整网络音频电平，范围为 `-60 dB` 到 `+12 dB`。
6. 在手机应用中使用外置 USB 麦克风和播放设备。

Send **不改变 DAW 轨道的原始输出**，立体声音轨保留左右声道发送，单声道音轨复制到左右两个声道；其网络增益只影响发送信号。Return **用收到的音频替换轨道输出**，立体声音轨保留左右声道，单声道音轨输出左右平均值。

避免把 Return 音轨再次送入同一设备的 Send 路径，以免形成音频反馈。需要把手机音频纳入工程时，应先实时录制到音轨；插件在宿主标记的离线导出模式下不进行网络音频传输。

## 开发与构建

### ESP32-S3 固件

固件使用 PlatformIO 和 Arduino 框架，项目入口为 [esp32s3/platformio.ini](esp32s3/platformio.ini)。

| 依赖或配置 | 当前版本 |
| --- | --- |
| PlatformIO 平台 | pioarduino `53.03.11` |
| Arduino-ESP32 | `3.1.1` |
| arduino-audio-tools | 提交 `27547291d4a1514e64d37a97f1ce34636af86bd6` |
| codec-opus | `v1.6.1` |
| WiFiManager | `2.0.17` |
| C++ 标准 | C++17 |

在 VS Code 中用 PlatformIO 打开 `esp32s3/`，选择 `esp32-s3-devkitc-1` 环境进行构建和上传。命令行方式如下，在项目根目录执行：

```sh
pio run -d esp32s3 -e esp32-s3-devkitc-1
pio run -d esp32s3 -e esp32-s3-devkitc-1 -t upload
```

固件按采集、网络和编码分工：

| 任务 | 核心 | 优先级 | 栈 | 职责 |
| --- | --- | --- | --- | --- |
| `audioTask` | Core 0 | 4 | 8 KiB | USB 收发、音量处理与 PCM 组帧 |
| `networkTask` | Core 1 | 3 | 32 KiB | 设备发现、订阅、UDP 收发与上行解码 |
| `encoderTask` | Core 1 | 2 | 48 KiB | 手机播放音频的立体声 Opus 编码 |

PCM 播放和采集队列位于 PSRAM；编解码器状态及编码任务静态栈位于内部 RAM。设备使用非阻塞 socket 和任务通知传递音频，Opus 采用固定点与 Xtensa 优化。

### VST3 插件

构建入口为 [vst3/CMakeLists.txt](vst3/CMakeLists.txt)，需要 CMake 3.22 或更新版本、C++17 编译器、Git 和 Ninja。CMake 自动获取 JUCE `8.0.6` 和 Opus `v1.6.1`。

- macOS：安装 Xcode Command Line Tools；可构建本机架构或 `arm64;x86_64` Universal 插件。
- Windows：安装 Visual Studio C++ 构建工具与 Windows SDK，在 x64 开发者命令环境中使用 Ninja。
- VS Code：使用 CMake Tools，将源码目录设为 `vst3/`，选择所需预设。

在项目根目录构建本机架构的 Release 插件：

```sh
cmake -S vst3 -B vst3/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build vst3/build --target lan_mic_send_VST3 lan_mic_return_VST3
```

macOS Universal 预设以 macOS 11.0 为最低部署目标。在 `vst3/` 目录执行：

```sh
cmake --preset mac-universal
cmake --build --preset mac-universal
```

生成的插件名称为 `LAN MIC Send.vst3` 和 `LAN MIC Return.vst3`，位于构建目录下各自的 `lan_mic_*_artefacts/Release/VST3/` 中。

| CMake 选项 | 默认值 | 用途 |
| --- | --- | --- |
| `SOS_JUCE_PATH` | 空 | 使用已有的本地 JUCE 源码目录 |
| `SOS_COPY_VST3_AFTER_BUILD` | `ON` | 构建完成后自动复制到插件目录 |
| `SOS_VST3_DIR` | 空 | 自定义自动复制的目标目录 |

默认自动复制位置为 macOS 的 `~/Library/Audio/Plug-Ins/VST3` 和 Windows 的 `C:\Program Files\Common Files\VST3`。Windows 系统目录需要写入权限；可指定 `SOS_VST3_DIR`，或用 `-DSOS_COPY_VST3_AFTER_BUILD=OFF` 关闭自动复制。

## 安装与打包

`site/` 提供随仓库分发的 [macOS 安装包](site/LANMIC_0.1.0.pkg) 和 [Windows 安装包](site/LANMIC_Setup_0.1.0.exe)。开发时可从源码构建对应版本；固件与插件需要使用相同的协议版本。

已有构建产物可通过脚本安装。以下命令在项目根目录执行：

```sh
# macOS：安装到当前用户的插件目录
bash scripts/install_vst_macos.sh --user vst3/build
```

```powershell
# Windows：安装到当前用户的插件目录
.\scripts\install_vst_windows.ps1 -User .\vst3\build
```

不带 `--user` 或 `-User` 时，脚本安装到系统插件目录，并按需请求管理员权限。安装会替换目标目录中的同名插件；请先关闭 DAW，完成后重新扫描插件。

制作安装包：

```sh
# macOS：从指定构建目录生成 pkg
bash scripts/make_pkg.sh vst3/build-universal
```

```powershell
# Windows：需要 Inno Setup 7，兼容 Inno Setup 6.3+
.\scripts\make_installer.ps1 .\vst3\build
```

安装包输出到 `dist/`，版本号默认读取 `vst3/CMakeLists.txt`。macOS `.pkg` 安装到 `/Library/Audio/Plug-Ins/VST3`；Windows 安装器支持系统级或当前用户安装。macOS 打包脚本支持通过 `--sign` 指定安装器签名证书。

## 网络使用范围

自动发现要求电脑和设备处于允许广播及设备互访的同一局域网，防火墙需允许 DAW 的 UDP 通信。设备只需接入局域网，不依赖云端服务。

当前协议不提供音频加密或身份认证，会话 ID 仅用于区分设备启动和插件实例。请在可信网络中使用，不要将设备端口映射到公网。配网热点为开放热点，配置时应在可信环境中操作。

## 目录结构

| 目录或文件 | 内容 |
| --- | --- |
| [common/](common/) | 两端共用的协议、SPSC 队列与弹性播放缓冲 |
| [esp32s3/](esp32s3/) | PlatformIO 固件工程 |
| [esp32s3/src/main.cpp](esp32s3/src/main.cpp) | USB 音频、配网与各任务实现 |
| [esp32s3/src/NonBlockingUdp.h](esp32s3/src/NonBlockingUdp.h) | 非阻塞 UDP 封装 |
| [vst3/](vst3/) | JUCE Send / Return 插件与 CMake 预设 |
| [scripts/](scripts/) | macOS、Windows 插件安装及打包脚本 |
| [docs/](docs/) | 网络协议说明 |
| [site/](site/) | 产品展示页面、图片及发行安装包 |
