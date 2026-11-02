# 网络协议与线程设计

## 协议版本

唯一的 C++ 协议定义位于 `common/Protocol.h`。固件和插件直接使用同一文件；
`esp32s3/include/Protocol.h` 仅为转发头。Python 发现工具按相同格式读取。

当前版本为 **8**，不兼容版本 7。使用时必须一起更新固件与两个插件。
版本 8 将电脑到 ESP32 的上行从 14 ms 单声道 PCM 改为 20 ms 单声道 Opus，
并删除 kind 5 Audio。两个方向都以 20 ms / 每声道 960 样本为 Opus 帧长：
下行（ESP32 到电脑）为 48 kHz、立体声（交错 L/R）、192 kbps CBR，
上行（电脑到 ESP32）为 48 kHz、单声道、192 kbps CBR，均使用音乐模式，
不使用 Ogg 容器。

## 数据报

所有多字节字段都显式按小端读写，不直接发送 C++ 结构体，不依赖结构体对齐或主机字节序。
固定包头 28 字节。Opus payload 为 1 至 512 字节：下行 192 kbps CBR 的 20 ms 音频
通常为 480 字节（整包 508 字节），上行 128 kbps CBR 通常为 320 字节（整包 348 字节），
加上 UDP/IPv4 头均远低于 1500 MTU，不需要 IP 分片。

| 偏移 | 字节 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 4 | magic | `0x31534f53`，线上字节为 `SOS1` |
| 4 | 1 | version | `8` |
| 5 | 1 | kind | 1 Discover、2 Offer、3 Subscribe、4 Ack、6 AudioOpus |
| 6 | 2 | flags | Sender=1、Receiver=2、Busy=`0x8000` |
| 8 | 4 | session | 非零会话 ID，Discover 可为零 |
| 12 | 4 | sequence | 每个音频方向独立的包序号，允许自然回绕 |
| 16 | 4 | timestampMs | 发送端单调毫秒计数，允许自然回绕 |
| 20 | 4 | sampleRate | 必须是 48000 |
| 24 | 2 | payloadBytes | 必须与实际 UDP 长度完全匹配 |
| 26 | 2 | reserved | 必须为零 |
| 28 | 可变 | payload | 控制信息或裸 Opus packet |

AudioOpus 用于两个音频方向，必须 flags=0，payloadBytes 为 1 至 512。
下行（ESP32 到电脑）每包解码为每声道 960 个样本，按 L,R 交错存放（共 1920 个样本）；
上行（电脑到 ESP32）每包解码为 960 个单声道样本。
Offer 必须 flags=0，负载为 1 至 64 字节 ASCII 设备身份，不带终止符。
Discover、Subscribe、Ack 无负载。接收方拒绝错误版本、采样率、长度、保留字段或类型。

## 发现与订阅

1. 固件监听 UDP 45678（控制）和 45679（上行音频）。
2. 每个插件绑定自己的主临时端口，并为每个有效 IPv4 接口绑定独立的临时发现
   socket，在尚未选中设备时每秒从各接口广播 Discover。设备选中后停止广播和
   网卡端点重建，仅保留单播订阅续约；租约失效后立即恢复发现。Windows 上
   JUCE 8.0.6 无法提供接口定向广播地址，因此各绑定 socket 发送 limited
   broadcast，避免 VPN、虚拟网卡或多网卡让发现包只从错误接口发出。
3. 固件单播 Offer 回请求来源端口，payload 为 `LAN MIC-XXXXXX`；`XXXXXX`
   是 eFuse MAC 后 6 位大写十六进制，与 USB 产品名和配网热点编号一致，
   session 为本次开机随机 ID。插件可从带 6 位或 12 位十六进制后缀的历史
   Offer 中提取末 6 位并规范化显示，允许固件与插件分步升级。
4. 插件自动选择最先发现的设备，或选择用户指定身份。DHCP 地址不保存在工程中。
5. 插件每秒向选中设备 45678 发送 Subscribe，flags 为 Sender 或 Receiver，session 为插件实例随机 ID。
6. 固件每方向独立维护 `源 IP + 源端口 + 插件 session` 租约。未占用或原持有者续约时返回 Ack；冲突时 Ack 追加 Busy。
7. 租约自最后一次成功 Subscribe 起 3500 ms 失效。关闭插件或禁用网络后自然超时，不发送广播注销。
8. 固件和插件都识别开机会话变化，重新发现和订阅。多实例不会共用一个电脑固定接收端口。

Sender 向固件 45679 发送音频，session 使用自己的实例 ID，且源端口必须与订阅相同。
Receiver 通过订阅登记接收端口。固件从 45679 向该端口单播音频，session 使用固件开机 ID。
旧的固定电脑下行端口 45680 不再使用。接收插件单独启动也可以获取手机音频，不依赖发送插件。

每台设备同时最多服务一个 Sender 和一个 Receiver。默认自动选择不会遇到 Busy 就抢占或切换其他设备，
避免发送、接收插件无意中连到不同手机；用户可在界面显式选择。

随机 session 用来隔离重启和实例，不是身份认证或安全令牌。
