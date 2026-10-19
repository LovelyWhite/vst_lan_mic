# Debug Session: audio-crackle-stability
- **Status**: [OPEN]
- **progress_status**: collecting post-fix evidence
- **Issue**: ESP32-S3 双向 USB/Wi-Fi 音频持续爆音；允许增加延迟，目标是稳定连续播放。
- **Evidence Source**: existing UART serial log
- **Evidence Directory**: `/tmp/trae-debug-vst-lan-mic/audio-crackle-stability`
- **Pre-fix Log**: `/Users/lw/Library/Application Support/Trae/User/workspaceStorage/2ce1acc2116fbc90cc3eda684aceb4ac/long-text/6ad36df8bb0d6a128bbe996b/mvci8u65-ghv0/ESP-ROM_es....txt`

## Reproduction Steps
1. ESP32-S3 启动并连接 Wi-Fi。
2. 手机打开双向 USB Audio 流，DAW 同时运行 `vst2esp` 与 `esp2vst`。
3. 连续传输约 2 分钟，监听爆音并保存 UART 输出。

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | 5 ms 双向 UDP 包率使 lwIP TX 缓冲耗尽 | High | Low | CONFIRMED：`udpFail=502`、最大连败 130，errno 12=ENOMEM，最低普通堆仍有 150964 字节 |
| B | 15 ms 起播缓冲过小，队列在欠载与裁剪间振荡 | High | Low | CONFIRMED：每个 5 秒窗口队列均跨越 0 到 1871/2111/2303，超过 720 目标与 1680 裁剪线 |
| C | ±0.2% 时钟补偿范围不足 | Medium | Low | REJECTED：提高到 ±0.5% 后，完整平均帧数下仍发生欠载 |
| D | networkTask 批处理造成下行突发而非均匀发送 | Medium | Medium | CONFIRMED：捕获队列和单轮发送峰值均为 3 |
| E | USB FIFO 本身持续欠载或溢出 | Medium | Medium | REJECTED：USB RX 丢弃始终为 0，TX/RX 端点传输计数稳定增长 |
| F | 第二轮丢包补偿的临时 PCM 帧使网络任务栈溢出 | High | Low | CONFIRMED：`Stack canary watchpoint triggered (sos-network)`，回溯在深层 lwIP 调用并落到 `main.cpp:225` |
| G | 32 个动态 Wi-Fi TX buffer 不足，扩为 48 可根治 ENOMEM | Medium | Medium | REJECTED AS FIX：最大连败下降，但 `udpFail/superseded` 仍持续增长 |
| H | 原始 PCM 下行平均负载超过当前 ESP32 Wi-Fi 发送路径的可持续能力 | High | High | CONFIRMED：信号、USB、栈和内部堆健康，扩容 TX buffer 后仍持续失败 |
| I | Opus 修复下行后，ESP 约 210 ms 的 PCM 播放储备仍不足以覆盖短时到达抖动 | High | Low | REJECTED AS ROOT：增至 308 ms 只延长耗空周期，未消除固定速率差 |
| J | PCM 平均速率正常，但周期性长发送或到达间隔使播放队列耗空 | High | Low | REJECTED：`upSourceMax=17`、`upArrivalMax<=45`，不足以耗空 308 ms 缓冲 |
| K | 关闭 USB IN 包长控制后固定发送 196 字节，使 48 kHz 实际按 49 kHz 消费 | High | Low | CONFIRMED：库描述符公式额外预留 1 样本，14.8 秒理论耗空周期与实测吻合 |
| L | DAW 返回声音的轻微杂音来自 96 kbps Opus 有损压缩伪影 | Medium | Low | ACTIVE：ESP 侧 UDP、USB OUT、编码和队列指标均稳定，提升码率验证 |

## Log Evidence
- 启用双向流后，约每 5 秒 `under` 增加 20–30，`trim` 增加 4–8。
- `NetworkUdp.cpp:255` 错误码 12 以数十次连续突发出现。
- 约 165 秒时：`up=32798 down=28769 under=696 trim=173 drop=0`。
- 已在 `esp32s3/src/main.cpp` 加入 UART 结构化诊断：
  - A：UDP 累计失败、最大连续失败、空闲/最低堆内存。
  - B/C：网络播放队列每 5 秒窗口内最小/最大样本数。
  - D：捕获队列峰值、单轮发送峰值、最大帧年龄。
  - E：USB TX/RX FIFO 范围、USB RX 丢弃字节及端点传输计数。
- 诊断只读取状态和增加计数，不改变音频包长、缓冲阈值、任务优先级或发送策略。
- 新 pre-fix 样本：
  - `up=15295 down=13459 under=462 trim=178 mic=0..1871 udpFail=502 udpFailBurst=130`
  - `up=16294 down=14329 under=490 trim=185 mic=0..2111`
  - `up=17289 down=15191 under=513 trim=192 mic=0..2303`
  - `usbRxDrop=0`，每 5 秒 USB TX/RX 传输计数均增加约 5023。
- `frameAgeMax=4294967295` 是诊断读取 `now` 后另一核心才发布新帧造成的无符号下溢，不是实际帧年龄。

## Verification Conclusion
根因是 5 ms 双向高包率和下行批量发送使 lwIP/Wi-Fi TX 资源耗尽，造成真实下行丢帧；
同时 15 ms 播放缓冲远小于已测网络抖动，队列持续在欠载和硬裁剪间振荡。实施稳定优先修复后，
需要相同场景采集 `run=post-fix` UART 证据。

## Fix Applied
- 协议升级到版本 4，音频帧由 240 样本/5 ms 增至 480 样本/10 ms，双向包率减半。
- 播放起播目标由 15 ms 增至 80 ms，硬裁剪余量由 20 ms 增至 80 ms。
- 播放环形队列增至 16384 样本，ESP32 捕获队列增至 32 帧。
- ESP32 下行每轮最多处理一个包；失败帧保留并以 10 ms 间隔重试，超过 200 ms 才丢弃。
- USB IN 软件 FIFO 目标由约 2 ms 增至约 8 ms，底层 FIFO 深度由 8 包增至 32 包。
- VST 发送队列增至 32 帧，陈旧帧阈值由 30 ms 增至 200 ms。
- 修正 `frameAgeMax` 诊断的跨任务时间快照下溢，并将诊断 runId 改为 `post-fix`。

## Post-fix Verification Gate
使用协议版本 4 的固件和两个 VST3 插件重复双向音频至少 60 秒。通过条件：
- `udpFail` 不增长，且不再出现 `NetworkUdp.cpp:255` errno 12。
- 稳态后 `under`、`trim`、`overflow`、`drop` 不增长。
- `mic` 不触及 0，也不超过 7680；`sendBatch` 最大值为 1。
- `usbRxDrop` 不增长，`frameAgeMax` 小于 200 ms。
- 实际监听没有爆音。

## Post-fix Run 1
- `udpFail=1217`、`udpFailBurst=74` 在连续三个 5 秒窗口不再增长，第一轮降包率和发送节流有效。
- `under` 仍从 34 增至 38，且每个窗口 `mic` 最小值均为 0，爆音仍可复现。
- `trim=1` 保持不变，说明剩余问题是欠载，不再是过满裁剪。
- 之前窗口 `capPeak=23`、`frameAgeMax=235`，实测网络停顿约 230 ms，明显超过第一轮 80 ms 起播缓冲。
- **结论**：第一轮改善但未通过验证，进入第二轮稳定优先修复。

## Fix Iteration 2
- 起播目标和裁剪余量分别提高到 300 ms，播放队列扩大到 32768 样本。
- 音频超时提高到 500 ms，ESP32/VST 陈旧帧阈值提高到 300 ms。
- 序号缺口最多保持 300 ms 时间轴；缺口开始淡出，真实音频恢复时淡入。
- 新增 `upLost`、`upGap` 和 `stale` 证据字段，诊断 runId 改为 `post-fix-2`。
- 第二轮通过条件：稳定起播后 `under/trim/overflow/drop/stale` 不增长；
  `mic` 不触及 0，`udpFail/upLost` 不持续增长，实际监听无爆音。

## Post-fix Run 2 Startup Failure
- 手机开启 USB 音频、Wi-Fi 就绪后，Core 0 立即触发
  `Stack canary watchpoint triggered (sos-network)`。
- 回溯经过 lwIP `api_msg.c`、`tcpip.c`、`sockets.c`，最终映射到控制包
  `control.endPacket()`；该调用只是进入深层栈时发现此前栈空间已耗尽。
- 第二轮 `SequenceTracker::accept()` 同时在栈上创建三个 480 样本 PCM 帧，
  约占 2880 字节；网络任务本身另有约 1 KB UDP 包、约 1 KB 待发帧及 lwIP 调用栈。

## Fix Iteration 3
- 丢包补偿改为复用单个 480 样本 scratch 帧，临时 PCM 栈占用约从 2880 降到 960 字节。
- `sos-network` 任务栈从 8192 增至 12288 字节。
- 新增 `netStackMin` 高水位指标，诊断 runId 改为 `post-fix-3`。
- 通过条件增加：运行期间无 Guru Meditation，`netStackMin` 保持充足且不持续下降。

## Post-fix Run 3
- `netStackMin=5820`、`heap=99492`、`minHeap=57276`，无 Guru Meditation，栈修复通过。
- 最后两个窗口 `udpFail=2949`、`stale=1219`、`upLost=14`、`drop=9` 均不再增长。
- 但完整运行中曾出现 `udpFailBurst=111`、`capPeak=32`、`frameAgeMax=324`，
  失败重试期间累计丢弃 1219 个过期帧，说明旧帧重试形成拥塞正反馈。
- `under` 从 10 增到 11 后暂时稳定；300 ms 缓冲有效，但仍可能被上述发送风暴耗空。

## Fix Iteration 4
- UDP 失败帧不再持续重试；后续发送按 10/20/40/80/100 ms 上限指数退避。
- 每次恢复发送前丢弃队列旧帧，只发送最新帧；接收端使用已有序号补偿保持时间轴。
- 新增 `superseded` 计数，区分主动跳过旧帧与超过 300 ms 的 `stale` 丢弃。
- 固件使用 `CORE_DEBUG_LEVEL=0` 禁止 `NetworkUdp.cpp` 为每次 ENOMEM 同步打印错误；
  保留项目自己的 5 秒结构化诊断，避免串口错误风暴反过来阻塞网络任务。
- 诊断 runId 改为 `post-fix-4`。通过条件：`stale/drop` 不增长，失败期
  `udpFail` 增速受退避限制，恢复后 `frameAgeMax < 300` 且实际音频不中断。

## Post-fix Run 4
- 栈、堆、USB 均稳定：`netStackMin=5852`、`minHeap=57840`、`usbRxDrop=0`。
- `stale=0`、`drop=0`，最新帧策略消除了过期帧回放。
- 但每 5 秒 `udpFail` 仍增加约 46–58，`superseded` 增加约 118–154；
  下行只成功约 293–337/500 帧，缺口仍接近 40%。
- 上行 `upLost=0/upGap=0`，但 `mic` 从约 14400 周期性降到 0，`under` 从 1 增至 2。
  平均包数与 USB 消费量匹配，说明电脑插件批量发送造成到达抖动，而不是实际丢包或时钟漂移。

## Fix Iteration 5
- 协议帧长从 480/10 ms 调整为 672/14 ms，双向包率由 100 降至约 71.4 包/秒；
  应用数据报 1372 字节，连同 IP/UDP 头仍低于 1500 MTU。
- 协议版本升级；固件、`vst2esp`、`esp2vst` 必须一起重建。
- VST 发送线程改为按 14 ms 节奏每轮最多发送一帧；积压时跳到最新帧，不再成批追赶。
- 播放缓冲继续按约 300 ms 计算，不随帧长改变而意外增加。
- 新增 Wi-Fi RSSI 字段，用于区分发送资源压力与弱信号重传。

## Post-fix Run 5
- `rssi=-37 dBm`，信号质量优秀，REJECT 弱信号导致 Wi-Fi 重传的解释。
- 5 秒窗口内上行收到 332 帧、`upLost` 增加 27，合计约 359 帧，正好匹配
  14 ms 帧的理论数量；`upGap=1` 表明 VST 相对定时持续积累 1 帧缺口。
- 下行约成功 215/359 帧，`udpFail` 增加 68、`superseded` 增加 77，缺口比例仍约 40%。
- `minHeap=30504`，较第四轮约 57 KB 明显下降。14 ms 帧使 32 槽捕获队列增大，
  而最新帧策略只需要覆盖最大 100 ms 退避；多余队列直接挤压 lwIP 可用内存。
- `netStackMin=4324` 仍安全，USB 无丢弃。

## Fix Iteration 6
- VST 发送截止时间改为绝对时间累计，消除每轮 1–2 ms 调度误差的长期漂移。
- ESP 捕获队列从 32 缩至 16 个 14 ms 帧，仍覆盖 224 ms，超过 100 ms 最大退避。
- USB FIFO 从 32 缩至 16 个 1 ms 包，仍高于 8 ms 软件填充目标。
- 预计返还约 27 KB 内存给 lwIP；诊断 runId 改为 `post-fix-6`。

## Post-fix Run 6
- 正常窗口达到完整吞吐：5 秒内 `up +359/down +359`，所有错误计数保持不变。
- VST 绝对定时生效：`upLost` 仅增加 1 后停止。
- 前一窗口仍有一次资源拥塞：`udpFail +61`、`superseded +78`、`under +1`；
  恢复后空闲堆从约 69 KB 回升到约 109 KB。
- `rssi=-30..-31 dBm`、`usbRxDrop=0`、`netStackMin=4324`，继续排除信号、USB 和栈。
- 当前 `qio_opi` Arduino 库配置使用 32 个动态 Wi-Fi TX 缓冲；
  `WiFi.useStaticBuffers(true)` 在此库变体中仍保持动态 TX，不能解决该问题。

## Fix Iteration 7
- ESP32 播放环形队列从 32768 缩至 16384 样本，返还 32 KB 内部 RAM 给
  Wi-Fi DMA/lwIP；电脑插件仍保留 32768 样本队列。
- ESP32 起播目标改为 15 帧、约 210 ms；电脑端保持 22 帧、约 308 ms。
- 该取舍减少约 98 ms 缓冲，但消除大幅多余容量；协议版本不变。
- 诊断 runId 改为 `post-fix-7`。通过条件：连续运行期间 `udpFail/superseded/under`
  不增长，空闲堆不再伴随发送失败周期性大幅下降。

## Post-fix Run 7
- `heap=108988/minHeap=54504` 与第六轮几乎相同，预期的 32 KB 内存没有释放。
- 静态检查确认条件编译误用了未定义的 `SOS_ALLOW_NONLOCKFREE_ATOMICS`；
  固件实际仍选择 32768 样本队列，因此本轮不能用于评价缩容效果。
- 累计计数仍显示此前发生过拥塞，但只有一个快照，无法计算当前窗口增量。

## Fix Iteration 8
- ESP 队列条件改用实际存在的 `ARDUINO_ARCH_ESP32`，确保选择 16384 样本容量。
- 新增 `queueCap/target/maxBlock/psramFree/psramTotal`，同时验证实际编译容量、
  内部堆连续空间和物理 PSRAM。
- 诊断 runId 改为 `post-fix-8`；其余音频与协议行为不变。

## Post-fix Run 8
- 队列缩容已生效：`queueCap=16384/target=10080`。
- 板载 PSRAM 可用：`psramTotal=8388608/psramFree=8366820`。
- 内部最低堆提升到 `86768`，最大连续块 `55284`，但累计仍有
  `udpFail=850/superseded=1271`；内部内存充足时仍发生 ENOMEM。
- 乐鑫 FAQ 对该组合的解释是 Wi-Fi TX buffer 已满，建议减速或增加 TX buffer；
  当前协议已经从 100 降至约 71.4 包/秒，因此选择增加缓冲。

## Fix Iteration 9
- 使用链接器 `--wrap=esp_wifi_init`，在不修改 PlatformIO 全局框架的前提下，
  将 qio_opi 变体的动态 Wi-Fi TX buffer 从 32 提高至 48。
- 新增 `wifiTxBuf` 验证实际传给驱动的数量，诊断 runId 改为 `post-fix-9`。
- 预计额外最坏占用约 24–32 KB 内部 RAM；第八轮最低内部堆 86 KB，余量可接受。
- PSRAM 已确认可用，但本轮不迁移 USB DMA/FIFO；先验证官方建议对应的直接修复。

## Post-fix Run 9
- `wifiTxBuf=48` 已确认扩容生效，但累计仍达到 `udpFail=1039`、
  `superseded=1380`，最大连续失败为 7。
- `rssi=-33 dBm`、`usbRxDrop=0`、`netStackMin=4324`，继续排除弱信号、USB 和栈。
- `heap=78460/minHeap=62944/maxBlock=31732`，内部内存仍有余量，但扩容 TX buffer
  反而消耗约 24–32 KB 内部 RAM，不能解决持续吞吐问题。
- **结论**：G 作为根治方案被拒绝；H 被确认。继续增加 Wi-Fi buffer 不合理，
  应降低 ESP32 到电脑方向的平均码率。

## Fix Iteration 10
- 协议升级到版本 6；电脑到 ESP32 继续使用 672 样本/14 ms PCM。
- ESP32 到电脑改为单声道 48 kHz、960 样本/20 ms、96 kbps CBR Opus，
  使用裸 Opus packet，不增加 Ogg 容器。
- Opus 编码器状态显式分配到 8 MB PSRAM，释放内部 RAM 给 Wi-Fi/lwIP；
  恢复默认动态 Wi-Fi TX buffer，不再使用 48 buffer 链接器包装。
- `esp2vst` 使用独立 Opus 解码器；短序号缺口调用 PLC，最多补偿 15 帧/300 ms，
  大缺口和重连、超时、启停、设备切换都会重置解码器状态。
- 新增 `opusBytes` 和 `opusErr`，诊断 runId 改为 `post-fix-10`。
- 通过条件：`opusErr=0`，CBR 包通常约 240 字节；稳态
  `udpFail/superseded/under` 不增长，实际监听无卡顿和爆音。

## Post-fix Run 10
- 约 171.2 秒时 `up=12225/down=8560`，分别与 14 ms PCM 的约 71.4 包/秒和
  20 ms Opus 的 50 包/秒吻合，双向平均吞吐保持实时。
- `udpFail=0/udpFailBurst=0/superseded=0`，原始 PCM 下行造成的持续发送资源
  耗尽已消失；`capPeak=1/sendBatch=1/frameAgeMax=6`，编码和发送没有形成积压。
- `opusBytes=240..240/opusErr=0`，96 kbps CBR 输出符合预期。
- Opus 状态已进入 PSRAM：`psramFree=8291068`；内部
  `heap=147488/minHeap=143576/maxBlock=106484`，Wi-Fi/lwIP 内存余量显著恢复。
- USB 保持稳定：`usbRxDrop=0`。上行累计仅 `upLost=3/upGap=1`，丢包率很低。
- `under=17/stale=1` 为累计值，单个快照不能判断是否仍在增长；本窗口
  `mic=909..7441` 未触及零，但最低余量偏低，需要后续连续快照确认稳态。
- `netStackMin=2540` 比 PCM 方案下降，说明 Opus 编码增加了栈使用；尚有余量，
  后续长时间运行需确认该值不继续下降。
- 第二个快照相隔约 75.5 秒：`up +5393/down +3776` 仍匹配两个方向的理论实时包率，
  `udpFail/superseded/opusErr` 保持为零，Opus 下行修复继续成立。
- 同期 `under 17 -> 25`，但 `upLost 3 -> 5` 且最大缺口仍为 1；平均吞吐和实际丢包
  无法解释周期欠载，确认剩余问题是 PCM 到达/消费短时抖动超过 210 ms 播放储备。

## Fix Iteration 11
- ESP 播放队列从 16384 恢复为 32768 样本，起播目标从 10080 提高到
  14784 样本/约 308 ms，增加约 98 ms 抖动余量。
- Post-fix 10 最低内部堆为约 143 KB；队列增加约 32 KB 后预计仍超过 100 KB，
  高于为 Wi-Fi/lwIP 保留 50 KB 的安全线。
- Opus 编码使 `netStackMin` 降至 2540，因此网络任务栈从 12288 提高到
  16384 字节，降低深层 Opus/lwIP 调用叠加时的栈溢出风险。
- 诊断 runId 改为 `post-fix-11`。通过条件：确认
  `queueCap=32768/target=14784`；连续至少 5 分钟
  `under/trim/overflow/drop/udpFail/superseded/opusErr` 均不增长，
  `minHeap > 100 KB`，实际监听无爆音。

## Post-fix Run 11
- 参数和资源余量符合预期：`queueCap=32768/target=14784`、
  `netStackMin=6676`、`minHeap=106192`。
- Opus、UDP 和 USB 继续稳定：`opusBytes=240..240/opusErr=0`、
  `udpFail=0/superseded=0/usbRxDrop=0`。
- 两个快照相隔约 35.2 秒，`under 8 -> 10`、`upLost 2 -> 3`；
  单个丢失帧已有序号补偿，不足以解释两次完整欠载。
- 队列窗口从 `7945..14153` 降至 `4803..10567`，持续位于 14784 目标以下。
  增大固定缓冲延长了欠载周期，但 ±0.2% 校正不能及时恢复低水位。

## Fix Iteration 12
- ESP32 播放方向的连续速率校正上限从 ±0.2% 提高到 ±0.5%，电脑端仍保持 ±0.2%。
- 校正仍与队列偏差成比例，接近 14784 样本目标时自动回到零，不施加固定变速。
- 固定起播延迟、队列容量、Opus 和网络发送逻辑均不变。
- 诊断 runId 改为 `post-fix-12`。通过条件：连续至少 5 分钟 `under` 不增长，
  `mic` 不再长期向零下降；同时 `trim` 不增长且实际听感无可察觉变调。

## Post-fix Run 12
- 连续两个 5 秒窗口中 `under 2 -> 3`，第二个窗口
  `mic=60..14788`，明确记录到队列耗空后重新积累至起播目标。
- 同期 `up +360`，与约 5.0 秒的理论 PCM 帧数一致；`upLost=2/upGap=1`
  完全不变，排除平均输入速率不足和新增 UDP 丢包。
- `udpFail/superseded/opusErr/usbRxDrop` 均为零，下行 Opus、Wi-Fi 发送和 USB
  仍然稳定。
- **结论**：±0.5% 校正没有消除欠载，C 不是当前根因。现象更符合 PCM 包出现
  长间隔后恢复，累计帧数正常但瞬时队列已耗空。

## Fix Iteration 13
- 撤回 ESP32 ±0.5% 校正，恢复两端 ±0.2%，避免无证据的额外变速。
- 新增 `upArrivalMax`：ESP32 连续有效 PCM 包的最大实际到达间隔。
- 新增 `upSourceMax`：相邻 PCM 包中 VST 单调时间戳的最大间隔。
- 若两者同时很大，根因在 DAW 回调/电脑发送源；若仅 `upArrivalMax` 很大，
  根因在发送线程、操作系统或网络到达调度。
- 诊断 runId 改为 `post-fix-13`。正常 14 ms 节拍应接近 14 ms；
  重点捕获与 `under` 增量同窗口出现的最大间隔。

## Post-fix Run 13
- 三个窗口中 `upSourceMax=17 ms`，VST/DAW 帧生成节拍正常；
  `upArrivalMax=42..45 ms` 仅为轻微到达抖动，远小于 308 ms 起播缓冲。
- `upLost=0/upGap=0/udpFail=0`，网络没有丢包；`under 1 -> 2` 且
  `mic=0..14927` 再次记录到完整的耗空和重新起播周期。
- 检查固定版本 `arduino-audio-tools` 源码确认：UAC2 描述符最大包长为
  `((48000 + 999) / 1000 + 1) * 2 * 2 = 196` 字节。
- `enable_ep_in_flow_control=false` 时回调每 1 ms 固定从 FIFO 读取该最大包长，
  即 49 个立体声样本，实际消费 49 kHz。网络生产保持 48 kHz，因此每秒净减少
  1000 样本；14784 样本目标约 14.8 秒耗空，与实测周期吻合。

## Fix Iteration 14
- 启用 USB IN 包长控制。48 kHz 是整数采样率，运行时每 1 ms 固定发送
  48 个立体声样本/192 字节；196 字节仍只是端点最大容量。
- 反馈端点继续关闭；本修复不改变 USB 描述符容量、网络包长、缓冲或 Opus。
- 新增 `usbTxPacket` 验证当前 USB IN 有效包长，诊断 runId 改为 `post-fix-14`。
- 通过条件：`usbTxPacket=192`，连续至少 5 分钟
  `under/trim/overflow/drop` 不增长，`mic` 不再形成周期性归零锯齿，
  且 `udpFail/superseded/opusErr/usbRxDrop` 继续为零。

## Post-fix Run 14
- 约 266 秒时 `usbTxPacket=192`，确认 USB IN 已从错误的固定 196 字节恢复为
  48 kHz 所需的 192 字节。
- `under=0/trim=0/overflow=0/drop=0`，且 `mic=12711..15498` 始终围绕
  14784 目标水位；此前约 15 秒一次的周期耗空已消失，K 得到实机确认。
- `usbRxDrop=0/opusErr=0`，USB OUT 与 Opus 编码保持稳定；
  `minHeap=93520/netStackMin=6620` 仍有健康余量。
- 上行累计 `upLost=26/upGap=1`，均为可补偿的单帧缺口；约占 18882 包的
  0.14%，没有造成播放欠载。
- 下行累计出现 `udpFail=17/udpFailBurst=15/superseded=48`。单个快照无法判断
  是启动瞬态还是持续拥塞，需用后续连续快照确认这些累计值是否继续增长。
- 另一轮独立运行约 292 秒时，连续两个窗口均为
  `udpFail=0/superseded=0/opusErr=0/usbRxDrop=0`，下行拥塞未复现。
- 最后约 5.05 秒内 `under=2`、`upLost=172`、`upGap=11` 均保持不变，
  `mic=13949..15708` 稳定围绕目标，确认稳态没有继续欠载。
- 本轮较早发生过最大 11 帧/约 154 ms 的上行缺口，但 308 ms 缓冲成功吸收；
  最后窗口 `upArrivalMax=24/upSourceMax=17` 已恢复正常。
- **代码侧结论**：固定 196 字节 USB 包导致的周期欠载已消除；Opus 下行和双向
  缓冲在当前运行证据中稳定。最终通过仍需结合实际监听无爆音的主观验收。
- 后续约 386 秒快照仍为 `usbTxPacket=192`，`mic=14280..16166`，
  `udpFail=0/usbRxDrop=0/stale=0/superseded=0/opusErr=0`；用户反馈 DAW 内
  手机返回声音仍有轻微杂音，现有 ESP 指标不支持传输丢包或 USB 丢弃解释。

## Fix Iteration 15
- ESP32 到电脑的 Opus 从 96 kbps CBR 提高到 192 kbps CBR，保持单声道、
  48 kHz、20 ms、音乐模式和裸 Opus packet。
- 每包 payload 从 240 增至 480 字节，仍低于协议 512 字节上限和网络 MTU；
  包率仍为 50 包/秒，远低于原始 PCM 带宽。
- 协议版本保持 6；Opus 解码器从包内读取码率，不需要改变解码路径。
- 诊断 runId 改为 `post-fix-15`。通过条件：`opusBytes=480..480/opusErr=0`，
  `udpFail/superseded` 不持续增长，并由实际监听确认杂音消失或明显降低。

## Post-fix Run 15
- 连续约 5.5 分钟日志保持 `opusBytes=480..480/opusErr=0`，
  `under/trim/overflow/drop/udpFail/superseded/usbRxDrop` 均为零。
- Opus 状态放入内部 RAM 后仍有 `minHeap=87180/maxBlock=51188`，
  `netStackMin=6624`；用户确认此前返回音频杂音已经消失，L 得到实机确认。

## Hypothesis M: iOS UAC Volume Control Is Advertised but Applied to No Audio
- iOS 会对 Feature Unit 的 master、left、right 发送 `SET_CUR`，日志显示
  `setVolume ... channel: 0/1/2`；Android 当前未发送这类请求。
- 描述符始终把音量/静音声明为 host-programmable。`volume_active=false`
  只禁止库内样本缩放，不会移除控制；因此 iOS 音量状态更新但听感不变。
- 库内 `setVolume` 还以 Warning 在 USB 控制回调打印，连续调节时串口
  flush 会阻塞实时 USB 处理，符合“调节时卡顿”现象。

## Fix Iteration 16
- Audio Tools 日志级别降至 Error，禁止音量/静音控制请求在实时回调逐条输出。
- 保持库的 `volume_active=false`，避免错误缩放 DAW 到手机的 USB IN 麦克风；
  实体请求回调只接受 Feature Unit 1（手机播放的 USB OUT 链路），忽略
  Feature Unit 2（DAW 到手机的 USB IN 麦克风链路）。回调仅更新原子状态，
  不做串口输出、动态分配或显式加锁；音量在手机播放 USB OUT 下混前处理。
- master 控制同时更新左右，channel 1/2 分别更新左右，不重复相乘；音频任务
  使用 Q16 定点增益和约 5 ms 平滑，避免阶跃爆音。
- 诊断 runId 改为 `post-fix-16`，新增 `usbVol=L/R` 和 `usbMute=L/R`。
  通过条件：iOS 调节时字段同步变化、DAW 音量生效、无 Warning，且
  `usbRxDrop/under/udpFail` 不增长。

## Post-fix Run 16
- 用户确认连续调节 iOS 音量已不再卡顿，说明控制回调日志阻塞问题已解决。
- 音量变化仍不符合听感刻度，最低档之前音量过大，M 得到部分确认。

## Hypothesis N: Normalized UAC Control Position Is Used as Amplitude
- Audio Tools 将描述符声明的 `-100..0 dB` 线性映射为 `0..1` 控制位置。
  例如 `0.72` 表示约 `-28 dB`，对应振幅约 `0.04`，并非 `0.72`。
- 直接将控制位置作为 PCM 增益会使中低档严重偏响，也会造成调节听感不均匀。

## Fix Iteration 17
- 将归一化控制位置还原为分贝后，按 `gain = 10^(dB/20)` 转为振幅 Q16；
  `0.0` 明确映射为零，原有约 5 ms 平滑保持不变。
- 诊断 runId 改为 `post-fix-17`，`usbVol=L/R` 现在显示实际振幅百分比。
  通过条件：iOS 音量变化听感均匀，最低档接近静音，静音档完全无声。

## Opus Memory Placement Override
- 按当前要求，Opus 编码器状态从 PSRAM 改为显式内部 RAM：
  `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`。此前 Post-fix 10 的 PSRAM 分配记录
  仍是当时的历史证据，不代表当前配置。
- 重新验收时必须确认编码器初始化成功，并观察 `minHeap/maxBlock/udpFail`；
  若最大连续块不足，初始化会直接失败，不允许静默回退到 PSRAM。

## Final Evidence Gate
- **Evidence Sources**: 上述 UART 日志；待采集含诊断字段的 UART 日志
- **progress_status**: collecting post-fix evidence
- **Hypothesis Conclusions**:
  - A: CONFIRMED
  - B: CONFIRMED
  - C: REJECTED AS PRIMARY
  - D: CONFIRMED
  - E: REJECTED
  - F: CONFIRMED
  - G: REJECTED AS FIX
  - H: CONFIRMED
  - I: REJECTED AS ROOT
  - J: REJECTED
  - K: CONFIRMED
  - L: CONFIRMED
  - M: PARTIALLY CONFIRMED
  - N: ACTIVE
