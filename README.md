# Studio One Stream

## 项目目标

使用 `ESP32-S3` 作为 Android 手机的外置 USB 耳麦，实现电脑与手机之间的双向音频传输。

音频源来自电脑端 DAW（通过 VST3 插件），手机播放的音频也可回传到电脑 DAW。

- ESP32-S3 枚举为 USB 立体声耳麦（USB IN 麦克风 + USB OUT 播放）
- 上行链路：电脑 → ESP32 → 手机麦克风
- 下行链路：手机播放 → ESP32 → 电脑 DAW
- 两个 VST3 插件：`vst2esp`（发送）和 `esp2vst`（接收）
- 无需固定 IP，自动发现 ESP32 设备
- 固定采样率 48kHz，单声道内部处理，立体声 USB 接口
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
DAW -> vst2esp -> Wi-Fi UDP -> ESP32-S3 -> USB IN -> 手机麦克风
DAW <- esp2vst <- Wi-Fi UDP <- ESP32-S3 <- USB OUT <- 手机播放
```

- 固定 48 kHz / PCM16，网络单声道，USB 双通道；立体声输入取左右平均，输出复制到左右。
- 5 ms 音频包，15 ms 起播缓冲；小幅速率校正补偿两台设备的时钟差。
- 独立网络线程、原子同步有界队列（电脑端要求无锁）、丢包补静音、欠载淡出、延迟积累裁剪。
- 广播发现，单播订阅与音频传输；每个插件使用系统分配的本地端口，不抢占固定电脑端口。
- 每台 ESP32 同时接受一个发送插件和一个接收插件，两个方向可以独立使用。
- 插件支持设备选择、增益、启停、状态统计和工程内配置保存。
- 固件通过 `arduino-audio-tools` 的 `USBAudioStream` 提供 USB Audio Class 2。

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

### 2. 固件

在 `esp32s3/include/WifiConfig.h` 填写 `SOS_WIFI_SSID` 和 `SOS_WIFI_PASSWORD`。
该本地配置已加入忽略规则，公共模板为 `WifiConfig.example.h`。密码为空可用于开放网络。

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
平台未锁定版本，其他机器的解析结果可能不同，应核对构建日志。
首次构建需要下载依赖。Arduino 3.3.12 使用原生 `AUDIO` 接口槽；
较早的 Arduino 2/3 版本保留 `CUSTOM` 接口槽适配。
音频库自带类驱动，因此**不要额外启用 `CFG_TUD_AUDIO` 或原生 USB CDC**。
升级框架或音频库之前，需要重新检查这层适配，不能直接替换成任意版本。

ESP32 工具链可能报告 `std::atomic<uint32_t>::is_always_lock_free == false`。
固件允许 SDK 提供的原子操作后备实现，仍保留 acquire/release 内存序；
不以普通变量或 `volatile` 替代。电脑插件仍强制检查游标原子操作无锁。

串口日志走 USB-UART 下载口或 UART0，115200 波特率；原生 USB 已专用于音频。
启动后日志打印设备身份和 DHCP 地址；未配置 Wi-Fi 时 USB 仍可枚举，但网络不工作。
每 5 秒输出 USB 流状态、收发数、欠载和队列丢帧统计。

### 3. VST3 插件

依赖 CMake 3.22+、Ninja、C++17 编译器。默认固定 JUCE 8.0.6，首次配置需要联网。
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
vst3/build/vst2esp_artefacts/Release/VST3/vst2esp.vst3
vst3/build/esp2vst_artefacts/Release/VST3/esp2vst.vst3
```

构建成功后，两个 bundle 会自动复制到 DAW 的插件搜索目录：

- macOS：`~/Library/Audio/Plug-Ins/VST3`
- Windows：`C:\Program Files\Common Files\VST3`

这一步由 JUCE 的 `COPY_PLUGIN_AFTER_BUILD` 完成，目标目录不存在时会由 CMake 创建，
无需手动拷贝。随后在 Studio One 重新扫描插件即可看到 `vst2esp` 和 `esp2vst`。

注意事项：

- 自动安装默认开启。只想保留构建产物时用 `-DSOS_COPY_VST3_AFTER_BUILD=OFF` 关闭。
- Windows 公共插件目录通常需要管理员权限，复制失败时以管理员身份构建；
  或改用自定义目录：`cmake --preset release -DSOS_VST3_DIR="D:/VST3"`。
  自定义目录同样会替换 macOS 的默认位置。
- 覆盖已被 DAW 加载的 bundle 可能失败。构建前先在 Studio One 中关闭工程或卸载插件。
- 两个 bundle 的路径固定为
  `vst3/build/vst2esp_artefacts/Release/VST3/vst2esp.vst3` 与
  `vst3/build/esp2vst_artefacts/Release/VST3/esp2vst.vst3`，也可手动拷贝。

### 4. Studio One 路由

1. 工程采样率设为 **48 kHz**。其他采样率不会进行网络音频传输，也不会隐式重采样。
2. 在要发给手机的音轨或总线上插入 `vst2esp`。默认保留本地原始声音；取消 `Local pass-through` 可静音本地输出，网络发送不受影响。
3. 在独立的、启用监听且持续处理的音轨或总线上插入 `esp2vst`。它用手机回传声音替换输入，不与输入混音。
4. 两个插件选择同一台设备。默认各自选择最先发现的设备；多设备环境应显式选择相同设备身份。
5. 手机上选择外置 USB 麦克风/耳麦，并在目标应用内授权录音或选择输入输出。
6. 不要将 `esp2vst` 的返回总线重新送进 `vst2esp`，除非明确需要环回，否则可能产生啸叫。

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
- USB 音量/静音控制未启用软件处理，音量请通过插件 `Network gain` 或手机应用调整。
- 仅可信局域网使用：发现、控制、PCM 均为未认证、未加密 UDP。不要映射端口到公网。
- 关闭 AP 客户端隔离；允许 DAW 的本地网络访问及防火墙 UDP 通信。VPN、多网卡或访客网络可能影响广播。
- USB VID/PID 沿用音频库的开发默认值，不代表拥有商业产品 USB 标识授权。
- 分发前确认 JUCE、音频库及相关依赖的许可证义务。

更多细节见 [网络协议](docs/PROTOCOL.md) 和 [人工验收清单](docs/VALIDATION.md)。
