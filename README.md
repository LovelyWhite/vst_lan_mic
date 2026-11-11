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
| 接收端目标缓冲 | 设备端 300 ms | Return 插件 300 ms |

USB 两个方向均为 48 kHz、16 bit、双通道；USB IN 按每毫秒 192 字节供数。目标缓冲不是端到端延迟，实际延迟还包括 DAW、网络、USB 及手机应用处理。本项目优先考虑连续音频传输，不定位为超低延迟现场监听设备。

### 网络与设备

- 使用 IPv4 UDP，设备监听 `45678` 进行发现与订阅，监听 `45679` 接收电脑音频。
- 手机音频由设备从 `45679` 单播到 Return 插件登记的临时端口。
- 每个插件拥有独立的本地端口；每台设备同时支持一个 Send 和一个 Return 实例。
- 插件按设备标识（`LAN MIC-XXXXXX`，来自 eFuse MAC，出厂固定）保存选择，不依赖固定 IP。设备名称为用户可改的显示名称（默认等于设备标识），用于配网热点、插件下拉框和网页顶栏，不参与识别。
- 固件和插件共用 SOS v10 协议，线上字段及音频常量以 [common/Protocol.h](common/Protocol.h) 为准。

## 快速使用

### 准备设备

- 一块带原生 USB 和 OPI PSRAM 的 ESP32-S3 开发板；当前固件配置使用 4 MB Flash 和 `qio_opi`。
- 支持 USB Host / UAC2 音频的手机，以及匹配的数据线或 USB 转接器。面向 Android 和 iPhone 使用，实际可用性取决于手机、转接器及应用的外置音频支持。
- 一台运行 VST3 宿主 DAW 的 macOS 或 Windows 电脑。
- 一个支持 2.4 GHz Wi-Fi 的局域网；电脑可以通过有线或无线接入。

手机应连接开发板的**原生 USB 接口**，而不是 USB 转串口接口。

### 连接 Wi-Fi

1. 为设备供电。首次使用或无法连接已保存网络时，设备启动配网热点，热点名即设备名称，默认为 `LAN MIC-XXXXXX`。
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
| arduino-audio-tools | `v1.3.0` |
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
| `networkTask` | Core 0 | 3 | 32 KiB | 设备发现、订阅、UDP 收发与上行解码 |
| `encoderTask` | Core 1 | 2 | 48 KiB | 手机播放音频的立体声 Opus 编码 |

PCM 播放和采集队列位于 PSRAM；编解码器状态及编码任务静态栈位于内部 RAM。设备使用非阻塞 socket 和任务通知传递音频，Opus 采用固定点与 Xtensa 优化。

### 串口控制台

固件在 USB CDC（原生 USB 串口）和 UART0 上轮询（约 1 s 一次）接收 `命令 [参数]` 文本行（可打印 ASCII，CR/LF 分帧，大小写不敏感），每行回复一条 `|` 分隔的短文本。CDC 回复走整行限时的写通道：按主机（手机）当时腾出的 FIFO 空间分块发送，整行 3 秒内没发完就整行丢弃，不会只送出半行而打乱对端分帧，其他任务的 `[D] ` 诊断行同样处理。回复以 `OK|`（成功）、`OKR|`（成功，重启后生效）或 `ERR|`（失败）开头，字段按命令固定排序、不带键名，值中的 `|` 与 `\` 用反斜杠转义；脚本据此把回复与其他 `[D] ` 前缀的诊断行区分开。配置写入 NVS（命名空间 `lanmic`）。

| 命令 | 说明 |
| --- | --- |
| `help` | 返回支持的命令列表 |
| `info` | 返回设备、网络与运行状态（字段见下表） |
| `name` | 读取设备名称（配网热点名，插件端显示名） |
| `name <名称>` | 设置设备名称（1–32 字节 ASCII，不含空格和 `\|`；热点名重启后生效） |
| `epin [0\|1\|on\|off\|true\|false]` | 读取／使能 USB IN（麦克风）端点 |
| `epout [0\|1\|on\|off\|true\|false]` | 读取／使能 USB OUT（播放）端点 |
| `erase` | 清除设备保存的 Wi-Fi 凭据，设备名称与端点配置不变 |
| `reboot` | 回复后立即重启设备 |

各命令的回复格式（第一个字段固定为回显的命令名）：

| 回复 | 说明 |
| --- | --- |
| `OK\|help\|命令列表` | 命令名以逗号分隔 |
| `OK\|info\|…` | 字段见下表 |
| `OK\|name\|名称` / `OKR\|name\|名称` | 读取 / 设置成功 |
| `OK\|epin\|0或1` / `OKR\|epin\|0或1` | `epout` 同形；读取 / 设置成功 |
| `OKR\|erase` | 凭据已清除 |
| `OK\|reboot` | 回复后设备立即重启 |
| `ERR\|命令\|原因` | 失败回复 |

`info` 回复字段（按以下顺序 `|` 分隔；未连接 Wi-Fi 时 `wip`–`wgw` 四个字段为空）：

| 字段 | 含义 |
| --- | --- |
| `dev` / `sn` / `ses` | 设备标识 / USB 序列号 / 本次启动会话 ID |
| `ver` | SOS 协议版本 |
| `up` | 启动至今毫秒数 |
| `wip` / `wrssi` / `wssid` / `wgw` | 本机 IP / 信号 dBm / 已连接网络 SSID / 网关 |
| `name` | 设备名称（配网热点名，默认等于设备标识） |
| `ein` / `eout` | USB IN / OUT 端点当前启用状态，`1`/`0` |

```text
> info
OK|info|LAN MIC-ABCDEF|00112233445566|123|10|45678|192.168.1.5|-43|mywifi|192.168.1.1|LAN MIC-ABCDEF|1|1
> name my-mic
OKR|name|my-mic
```

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

`site/` 提供随仓库分发的 [macOS 安装包](site/LANMIC_{VERSION}.pkg) 和 [Windows 安装包](site/LANMIC_Setup_{VERSION}.exe)。开发时可从源码构建对应版本；固件与插件需要使用相同的协议版本。

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

## PCB 外壳模型

`docs/3D_PCB1_2026-11-09.obj` 是 EasyEDA 导出的 PCB 三维模型（单位 mm，板面 `z = 0`，板厚 `z -0.800..0.000`）。外壳为**底盒 + 上盖、分型面免螺钉无痕粘接、带板定位台阶**，由脚本 [docs/enc_build.py](docs/enc_build.py) 在 FreeCAD 中参数化生成，所有坐标都沿用 PCB 模型坐标系。当前版本按 **FDM 0.4 喷嘴**优化：所有打印实体特征 ≥ `0.40`（恰好一条线），止口模型单边间隙 `0.05`，叠加打印切宽后成品约 `0.08`/边（见下表）。

模型中已确认的元件（坐标为实测包络）：

| 元件 | 位置 | 说明 |
| --- | --- | --- |
| PCB 板体 | `x 0.015..28.414`，`y -16.061..-0.010` | 板底 `z -0.800`，板顶 `z 0.000` |
| USB Type-C（公头） | 金属壳口 `y -12.173..-3.923`，`z -1.515..0.775`，轴线 `z -0.43` | 端头伸出到 `x -10.485`，**插向外部设备**，金属壳必须露在壳体外面 |
| 按键 | 按压头 `x 3.768..5.714`，`y 0.449`，`z 0.444..1.282` | 越过板边，`+Y` 侧壁开窗后手指直接按板上按键 |
| 最高元件 | 屏蔽罩 `z 2.400` | 决定型腔顶隙 |
| 最低元件 | Type-C 贴片焊脚 `z -2.000` | 决定型腔底面 |

关键设计参数：

| 参数 | 取值 | 作用 |
| --- | --- | --- |
| 型腔间隙 / 壁厚 / 分型面 | `0.70` / `2.00` / `z 0.05` | 型腔单边 0.70 = `0.40` 定位筋 + `0.30` 打印余量；壁厚 2.00 = 5 条线；分型面略高于板顶 |
| 外部圆角 | 竖棱 `R2.0`，顶底周边 `R1.5` | 周边圆角要求上下板厚大于 1.5 |
| Type-C 端口 | 单一光滑椭圆，`y -12.373..-3.723`，`z -1.840..0.980`，`r 1.39` | 按 2.42 端口高 + 每边 0.20 空气（0.08 滑配打印不出来）；壳体上下对称于连接器轴，端口在端面与金属壳上都居中 |
| 端口包裹 | `-X` 端在型腔外再包 `0.40`，公头露出 `7.40` | 端面 `x -3.085`、椭圆孔穿过 `2.00` 壁厚；连接器尾部在 `x -2.85` 之后向 `z` 张开到 `-2.000..1.130`，孔内面一侧开让位槽，端面外侧 `0.23` 仍是纯椭圆唇，公头插进去后有台阶定位 |
| 按键窗 | `2.00 × 1.00 mm`，`x 3.741..5.741`，`z 0.363..1.363` | 只开按压部分，开关本体仍被壁遮住 |
| 横向限位 | 底盒台阶上四周一圈 `0.40` 宽定位筋（一条线），筋内面距板边 `0.30`，高 `z -0.800..0.000` | 板子只能沿 `z` 取出，不会在型腔里平移；打印收缩吃掉部分余量后仍有滑配间隙；`-X` 一侧落在端口让位槽内的部分自动让开 |
| 粘接止口 | 分型面内侧一圈：平整边 `0.40` → 止口槽 `1.20 宽 × 1.60 深`（上盖中央凸筋 `1.10 宽 × 1.20 高`，模型单边间隙 `0.05`）→ 内墙 `0.40`；筋底距槽底留 `0.40` | 真实装配间隙 = 模型间隙 + 打印切宽：实测 `0.4` 喷嘴把 `1.20` 槽切到 `1.26`、筋成型准确，成品单边约 `0.08`——比线对线（约 0.03，已实测可装配但偏紧）略松，仍无晃动感，胶膜也留出余量。合盖贴合靠平整边与槽口，不靠胶水；无挡胶隔墙与泄胶孔，胶条须薄于约 `0.08`/边；外侧平整光滑，粘接后不可拆 |

成品外形 `34.20 × 21.45 × 9.80 mm`，底盒 2342 mm³、上盖 1774 mm³，各为单一实体。

在 FreeCAD 控制台（或 Python 环境内已加载 Part/Mesh 时）执行脚本即可重建，输出覆盖 `enclosure/` 下的模型：

```python
exec(compile(open("docs/enc_build.py").read(), "enc_build.py", "exec"), {})
```

产物：`enclosure/PCB1_enclosure.FCStd`（含 `Base`、`Lid` 与半透明的 `PCB_ref` 对照网格），脚本只保存 FreeCAD 文档，不导出 STEP、不渲染预览图。脚本末尾内置静态干涉检查（把 PCB 网格顶点逐点做体内判断，限位立边在型腔内、单独直测），不依赖任何编译步骤。

已知限制：

- 端口让位槽横穿 `-X` 侧分型面（`y -12.373..-3.723`），该处止口槽与胶道被打断成开口，胶条两端要在端口处收口，不能指望这一段继续挡胶。
- 止口成品旷量 = 模型 `0.05` + 你这台机器实测切宽（约 `0.03`/边）≈ `0.08`；换机器/换耗材后按实测槽宽重估，切宽不足也不会插不进，若旷动回潮就把 `FIT` 调小。胶仍建议低粘度毛细型（间隙 `0.08` 已宽容一些，稠胶仍可能顶盖）。
- 端壁孔是单边 `0.20 mm` 的滑配，靠 `2.00 mm` 长的孔壁导向，不是抱紧结构；若插入后端口晃动再考虑收紧。
- 公头露出 `7.40 mm` 后端壁 `2.00 mm`，端口尾部让位槽把孔内侧掏空，端面唇口只有 `0.23 mm` 是完整椭圆；对插时若母座外壳比端口更宽，可能先顶到壳体端面，需实测确认。
- 限位筋距板边 `0.30 mm`，打印收缩误差超过这个值就压到板子，按实测内腔尺寸调 `CLEAR`/`LOC`。
- 模型未做切片、打印、实插与粘接强度验证。

## 目录结构

| 目录或文件 | 内容 |
| --- | --- |
| [common/](common/) | 两端共用的协议、SPSC 队列与弹性播放缓冲 |
| [esp32s3/](esp32s3/) | PlatformIO 固件工程 |
| [esp32s3/src/main.cpp](esp32s3/src/main.cpp) | USB 音频、配网与各任务实现 |
| [esp32s3/src/NonBlockingUdp.h](esp32s3/src/NonBlockingUdp.h) | 非阻塞 UDP 封装 |
| [esp32s3/src/Console.cpp](esp32s3/src/Console.cpp) | 串口管道控制台与 NVS 配置 |
| [vst3/](vst3/) | JUCE Send / Return 插件与 CMake 预设 |
| [scripts/](scripts/) | macOS、Windows 插件安装及打包脚本 |
| [docs/](docs/) | 网络协议说明、PCB 三维模型与外壳建模脚本 |
| [enclosure/](enclosure/) | 外壳 FreeCAD 模型 |
| [site/](site/) | 产品展示页面、图片及发行安装包 |
