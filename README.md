# LAN MIC

## 项目目标

使用 `ESP32-S3` 作为 Android 手机的外置 USB 耳麦，实现电脑与手机之间的双向音频传输。

音频源来自电脑端 DAW（通过 VST3 插件），手机播放的音频也可回传到电脑 DAW。

- ESP32-S3 枚举为 USB 立体声耳麦（USB IN 麦克风 + USB OUT 播放）
- 上行链路：电脑 → ESP32 → 手机麦克风
- 下行链路：手机播放 → ESP32 → 电脑 DAW
- 两个 VST3 插件：`LAN MIC Send`（发送，构建目标 `lan_mic_send`）和
  `LAN MIC Return`（接收，构建目标 `lan_mic_return`）
- 无需固定 IP，自动发现 ESP32 设备
- 网络与 USB 固定 48 kHz，VST 自动转换 DAW 工程采样率；下行（手机→DAW）网络立体声，上行（DAW→手机）网络单声道，立体声 USB 接口
- 使用 `arduino-audio-tools` 库实现音频处理
- 代码精简，避免重复代码
- 音频需要稳定传输，不出现卡顿或延迟
- 支持 iphone 和 android 手机

## 目录结构

- `/esp32s3`：ESP32-S3 代码目录
- `/vst3`：VST3 插件代码目录
- `/python`：Python 代码目录

## 代码结构

- ESP32-S3：PlatformIO + Arduino framework + arduino-audio-tools
- VST3：JUCE + CMake
- Python：3.x + sounddevice + numpy（可选）

## 当前实现

已补齐 Arduino 固件、两个 JUCE VST3 插件、共享协议和 Python 发现工具。
当前交付为源码，尚未编译、刷机、加载 DAW 或验证手机兼容性。

```text
DAW -> LAN MIC Send -> Wi-Fi UDP -> ESP32-S3 -> USB IN -> 手机麦克风
DAW <- LAN MIC Return <- Wi-Fi UDP <- ESP32-S3 <- USB OUT <- 手机播放
```

- 网络与 USB 固定 48 kHz，VST 自动转换 DAW 工程采样率；上行网络单声道，
  下行网络真立体声（保留手机左右分离，插件按 L/R 输出），
  USB PCM16 双通道。
- 两个方向均使用 20 ms Opus：电脑到 ESP32 为 128 kbps CBR 单声道，
  ESP32 到电脑为 192 kbps CBR 交错立体声。
  两个网络接收方向均使用约 300 ms 起播缓冲；
  小幅速率校正补偿两台设备的时钟差。
- 独立网络线程、原子同步有界队列（电脑端要求无锁）、短丢包平滑补偿、
  长缺口和欠载淡出、延迟积累裁剪。
- 广播发现，单播订阅与音频传输；每个插件使用系统分配的本地端口，不抢占固定电脑端口。
- 每台 ESP32 同时接受一个发送插件和一个接收插件，两个方向可以独立使用。
- 插件支持设备选择、增益、启停、状态统计和工程内配置保存。
- `LAN MIC Return` 声明为可自主产生输出的 VST3 Generator，并在处理器准备和工程状态
  恢复阶段启动接收租约；无需先打开插件编辑器即可接收手机音频。
- 固件通过 `arduino-audio-tools` 的 `USBAudioStream` 提供 USB Audio Class 2。
- 固件省电策略：USB 音频停止 5 秒后重新启用 Wi-Fi modem sleep（`kWifiIdleSleepMs`），
  音频流恢复时立即关闭以保障传输；流式状态 LED 改为每秒短闪以消除常亮电流；
  USB 空闲时音频任务降为 200 Hz 轮询，Wi-Fi 未就绪时网络任务降为 20 Hz。
  空闲状态下发现设备/恢复音频可能多出几十毫秒响应延迟，流式期间行为不变。

“不卡顿、无延迟”是优化目标，不是无线传输保证。现有缓冲本身就会带来延迟，
实际端到端延迟还包括 DAW 缓冲、Wi-Fi、USB 和手机应用处理，必须测量。
初始配置更偏向稳定性，不建议用它代替要求极低延迟的有线现场监听。

## 快速开始

### 1. 硬件与网络

1. 当前配置沿用 **ESP32-S3、16 MB Flash、8 MB OPI PSRAM** 的板卡假设；其他型号先调整 `esp32s3/platformio.ini`。
2. 电脑与 ESP32 接入同一局域网。ESP32 使用 2.4 GHz Wi-Fi，电脑可用有线或同网段 Wi-Fi。
3. 手机接 ESP32 的原生 USB OTG 接口，不是 USB-UART 下载口。S3 原生 USB 为 GPIO19/D-、GPIO20/D+。
4. 手机必须充当 USB Host，使用支持数据的 OTG 线或转接器；供电不足时使用合规的有源 USB Hub。
5. 不要用普通 USB 线把两个 Host 端互接，也不要将两路未经隔离的 5 V 电源并联。
6. 红色状态 LED 使用 GPIO18，高电平点亮；建议 GPIO18 经 330 Ω～1 kΩ
   限流电阻连接 LED 正极，LED 负极接 GND。若实物采用低电平点亮，将
   `kStatusLedActiveHigh` 改为 `false`。

### 2. 固件

固件使用手机网页配网，不再读取或编译 `WifiConfig.h`：

1. 首次启动、没有已保存凭据或连接失败时，ESP32 创建名为
   `LAN MIC-<设备编号>` 的开放热点，无需密码。
2. 手机连接该热点后使用自动弹出的配网页面选择 2.4 GHz Wi-Fi 并输入密码；
   未自动弹出时访问 `http://192.168.4.1`。
   配网页面使用 `LAN MIC` 白色主题，直接进入 Wi-Fi 扫描和密码输入，
   不显示首页菜单、返回、退出、ESP 芯片信息、固件更新或网页清除按钮。
3. 未连接 Wi-Fi 时，设备会一直保持配网热点和页面，不再自动超时。
   配置成功后凭据保存到 ESP32 NVS，后续启动自动连接。
4. 需要更换网络时，在固件正常运行后长按开发板 **BOOT** 键约 3 秒，
   看到提示后松开。设备会清除已保存凭据并重启进入配网；不要按住 BOOT
   键执行硬复位，否则芯片会进入下载模式。

GPIO18 状态灯含义：

- 快闪（约 125 ms 翻转）：正在连接 Wi-Fi。
- 慢闪（约 500 ms 翻转）：开放配网热点，等待配置。
- 常亮：Wi-Fi 已连接，等待音频。
- 每秒短闪一次（亮约 100 ms）：USB 音频正在传输。
- 极快闪：BOOT 已长按 3 秒，可以松开并清除凭据。
- 每秒双闪：USB 初始化或 FreeRTOS 任务创建失败。

配网热点仅在需要配置时出现。由于热点不设密码，不要在不可信场所长时间停留在
配网模式；正式部署时还应考虑启用 Flash Encryption 保护 NVS 凭据。

使用 VS Code 的 PlatformIO 扩展打开 `esp32s3`，环境为 `esp32-s3-devkitc-1`。
以下命令由开发者按需执行，本次未执行：

```sh
pio run -d esp32s3
pio run -d esp32s3 -t upload
pio device monitor -d esp32s3 -b 115200
```

当前 `platform = espressif32` 使用本机安装的平台；本次编译日志为平台 55.3.312、
Arduino 3.3.12、ESP-IDF 5.5.5。音频库固定为
`arduino-audio-tools@27547291d4a1514e64d37a97f1ce34636af86bd6`。
下行压缩库固定为 `codec-opus@v1.6.1`。编码器状态当前显式放在内部 RAM，
需关注剩余连续块和 Wi-Fi/lwIP 可用堆空间。
Wi-Fi 网页配网固定使用 `WiFiManager@2.0.17`。
平台未锁定版本，其他机器的解析结果可能不同，应核对构建日志。
首次构建需要下载依赖。Arduino 3.3.12 使用原生 `AUDIO` 接口槽；
较早的 Arduino 2/3 版本保留 `CUSTOM` 接口槽适配。
音频库自带类驱动，因此**不要额外启用 `CFG_TUD_AUDIO` 或原生 USB CDC**。
USB IN 使用库的包长控制，在 48 kHz 下每 1 ms 发送 192 字节；描述符仍保留
196 字节最大包容量，不能将最大容量误当作固定有效包长。
升级框架或音频库之前，需要重新检查这层适配，不能直接替换成任意版本。

ESP32 工具链可能报告 `std::atomic<uint32_t>::is_always_lock_free == false`。
固件允许 SDK 提供的原子操作后备实现，仍保留 acquire/release 内存序；
不以普通变量或 `volatile` 替代。电脑插件仍强制检查游标原子操作无锁。

串口日志走 USB-UART 下载口或 UART0，115200 波特率；原生 USB 已专用于音频。
启动后日志打印设备身份和 DHCP 地址；未配置 Wi-Fi 时 USB 仍可枚举，但网络不工作。
USB 音频停止期间每 5 秒输出流状态、收发数、欠载和队列丢帧统计；
音频活跃时只累计指标，不执行长串口输出，停止后立即打印汇总。

### 3. VST3 插件

依赖 CMake 3.22+、Ninja、C++17 编译器。默认固定 JUCE 8.0.6，并通过 CMake
获取官方 Xiph Opus 1.6.1；首次配置需要联网。
macOS 使用 Xcode Command Line Tools；Windows 使用 Visual Studio 2022 C++ 工具链。
Windows 上应从 VS 开发者环境启动 VS Code，或通过 CMake Tools 选择 MSVC 工具链。

VS Code 的 CMake Tools 源码目录选择 `vst3`，配置预设选 `release`；
macOS 同时支持 Intel/Apple Silicon 的产物选择 `mac-universal`。
也可以在 `vst3` 目录执行：

```sh
cmake --preset release
cmake --build --preset release
```

已有 JUCE 源码时，可在配置时加 `-DSOS_JUCE_PATH=/path/to/JUCE`，建议仍使用 8.0.6。
产物分别位于：

```text
vst3/build/lan_mic_send_artefacts/Release/VST3/LAN MIC Send.vst3
vst3/build/lan_mic_return_artefacts/Release/VST3/LAN MIC Return.vst3
```

构建成功后，两个 bundle 会自动复制到 DAW 的插件搜索目录：

- macOS：`~/Library/Audio/Plug-Ins/VST3`
- Windows：`C:\Program Files\Common Files\VST3`

这一步由 JUCE 的 `COPY_PLUGIN_AFTER_BUILD` 完成，目标目录不存在时会由 CMake 创建，
无需手动拷贝。随后在 Studio One 重新扫描插件即可看到 `LAN MIC Send` 和
`LAN MIC Return`。

注意事项：

- 自动安装默认开启。只想保留构建产物时用 `-DSOS_COPY_VST3_AFTER_BUILD=OFF` 关闭。
- Windows 公共插件目录通常需要管理员权限，复制失败时以管理员身份构建；
  或改用自定义目录：`cmake --preset release -DSOS_VST3_DIR="D:/VST3"`。
  自定义目录同样会替换 macOS 的默认位置。
- 覆盖已被 DAW 加载的 bundle 可能失败。构建前先在 Studio One 中关闭工程或卸载插件。
- 首次改用新名称时，手动删除插件目录中遗留的旧版 bundle，避免 Studio One
  同时显示新旧插件。Windows 还需要在 Studio One 中重新扫描插件；新插件使用
  独立的 VST3 Class ID，不会与历史缓存中的插件类冲突。
- 两个 bundle 的路径固定为
  `vst3/build/lan_mic_send_artefacts/Release/VST3/LAN MIC Send.vst3` 与
  `vst3/build/lan_mic_return_artefacts/Release/VST3/LAN MIC Return.vst3`，也可手动拷贝。
- Windows 自动发现会为每个有效 IPv4 地址绑定独立 UDP socket，避免广播被 VPN、
  虚拟网卡或错误的默认路由截走。搜索状态中的数字是成功绑定的接口数量。
- Windows 网络应设为“专用”，并允许 Studio One 在专用网络通过 Windows Defender
  防火墙收发 UDP。显示 `Searching on 0 IPv4 interface(s)` 时先检查本机 IPv4 网卡；
  数字大于零但仍无设备时检查防火墙、AP 客户端隔离以及电脑与 ESP32 是否同网段。

### 4. Studio One 路由

1. DAW 工程可使用 44.1、48、88.2、96 kHz 等采样率。`LAN MIC Send` 自动转换到
   网络 48 kHz，`LAN MIC Return` 自动从网络 48 kHz 转回工程采样率。
   DAW 向插件提供浮点音频，工程位深无需设为 16-bit；网络边界始终量化为 PCM16。
2. 在要发给手机的音轨或总线上插入 `LAN MIC Send`。插件始终保留本地原始声音，
   `Network gain` 只调整发送到手机的音量。
3. 在独立的、启用监听且持续处理的音轨或总线上插入 `LAN MIC Return`。它用手机回传声音替换输入，
   不与输入混音，也不需要先打开插件编辑器来启动网络。
4. 两个插件选择同一台设备。默认各自选择最先发现的设备；多设备环境应显式选择相同设备身份。
5. 手机上选择外置 USB 麦克风/耳麦，并在目标应用内授权录音或选择输入输出。
6. 不要将 `LAN MIC Return` 的返回总线重新送进 `LAN MIC Send`，除非明确需要环回，否则可能产生啸叫。

设备被占用时，停止另一实例的网络功能或关闭实例，等待约 3.5 秒租约释放。
DAW 离线导出时网络音频禁用；需要导出的手机声音应先实时录制为音频文件。
接收插件旁路输出静音，发送插件旁路保留原始输入；恢复需要重新续约和缓冲。

### 5. 可选发现工具

无需安装 Python 音频依赖，也不会占用收发租约：

```sh
python3 python/discover.py
python3 python/discover.py --seconds 5 --broadcast 192.168.1.255
```

第二条中的广播地址仅为示例，实际应按子网填写，**不是配置 ESP32 固定 IP**。
当前 Python 目录只提供发现诊断，音频收发由两个插件承担；
`sounddevice`、`numpy` 不是本实现运行的必需依赖。

## 兼容性与限制

- 面向 Android USB Host 和支持 USB Audio 的 iPhone。USB-C iPhone 使用合适的数据连接；Lightning iPhone 通常需要支持供电的相机转接器。
- 固件提供 **UAC2**，不是 UAC1。不同 Android 系统、iOS 版本、转接器和应用对双通道 USB 麦克风的支持不同，当前没有实测通过的机型清单。
- 系统枚举成功不代表所有通话、直播或录音应用都会采用该输入；应用可能强制使用内置麦克风或单声道。
- iOS 发出的 UAC2 播放音量/静音请求会在 USB OUT 进入网络编码前以平滑增益处理，
  并按 UAC 分贝刻度转换为听感均匀的振幅，因而控制手机返回 DAW 的声音。
  `Network gain` 只控制 DAW 发往手机麦克风的方向。
  部分 Android 设备不会发送 UAC Feature Unit 请求，其音量行为由系统或应用决定。
- 仅可信局域网使用：发现、控制和 Opus 音频均为未认证、未加密 UDP。不要映射端口到公网。
- 关闭 AP 客户端隔离；允许 DAW 的本地网络访问及防火墙 UDP 通信。VPN、多网卡或访客网络可能影响广播。
- USB VID/PID 沿用音频库的开发默认值，不代表拥有商业产品 USB 标识授权。
- 分发前确认 JUCE、音频库及相关依赖的许可证义务。

更多细节见 [网络协议](docs/PROTOCOL.md) 和 [人工验收清单](docs/VALIDATION.md)。
