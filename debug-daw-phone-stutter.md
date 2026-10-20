# Debug Session: daw-phone-stutter
- **Status**: [OPEN]
- **progress_status**: collecting
- **Issue**: DAW 到手机方向稳定运行时偶发短暂卡顿。
- **Evidence Source**: ESP32 现有 `[DEBUG]` 串口结构化日志；用户提供的 pre-fix 日志位于 `/Users/lw/Library/Application Support/Trae/User/workspaceStorage/2ce1acc2116fbc90cc3eda684aceb4ac/long-text/6ad36df8bb0d6a128bbe996b/mvekzhu1-qpw5/[DEBUG] se....txt`
- **Evidence Directory**: `/tmp/trae-debug-vst_lan_mic-daw-phone-stutter`（本轮使用现有本地日志路径，不启用 HTTP）

## Reproduction Steps
1. 同时保持 `LAN MIC Send`、ESP32 USB Audio 和手机音频链路工作。
2. 连续播放 DAW 音频，等待偶发短暂卡顿。
3. 保存包含卡顿前后诊断快照的 ESP32 串口日志。

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| P | 网络包短时丢失或到达间隔尖峰 | Medium | Low | CONFIRMED：新日志中 `upLost` 由 19 增至 22，现有补偿会为每个孤立丢包制造约 28 ms 淡出/淡入凹陷 |
| Q | ESP 播放缓冲瞬时跌穿或校正裁剪 | Medium | Low | REJECTED：全程无欠载/裁剪/溢出，最低仍约有 276 ms 音频 |
| R | USB IN 1 ms 调度偶发停顿 | High | Medium | REJECTED：`usbTxGapMax=2`、`usbTxAdvanceMax=2`、`usbTxDone=192..192` |
| S | DAW 发送节拍偶发暂停但被 5 秒汇总掩盖 | Medium | Medium | CONFIRMED：用户确认严格约 5 秒周期，VST 发送线程存在同周期同步网卡重建 |
| T | UDP 无交付保证，桥接只能掩盖而不能阻止连续缺口 | High | Medium | CONFIRMED：post-fix 累计 `upLost=1182`、`upBridge=1158`、`upGap=7`，仍有 24 帧进入长缺口保护 |
| U | VST 每 5 秒重建发现 socket 阻塞 PCM 发送线程 | High | Low | CONFIRMED：用户测得严格约 5 秒卡顿，代码在同一网络线程每 5000 ms 枚举网卡并创建多个 socket |
| V | ESP 每 5 秒超长串口快照干扰实时任务 | Medium | Low | INCONCLUSIVE：周期吻合；作为并行干扰源禁止在 USB 活跃时执行 |

## Log Evidence
- pre-fix-17 共 34 个约 5 秒快照：`under/trim/overflow/drop/udpFail/usbRxDrop` 始终为 0。
- 第 4 个快照首次出现 `upLost=1/upGap=1`，之后不再增长；该单包缺口不足以解释后续所有偶发卡顿。
- 稳态 `mic` 最低值约 13234 到 13913，接近 14784 目标且远离 0；现有 5 秒最小值没有显示欠载。
- `usbTxPacket=192` 始终正确，`usbTx` 稳态约 1152..1536；现有指标没有记录 USB SOF/发送间隔尖峰。
- `upArrivalMax` 偶发 34 到 41 ms，正常值约 22 到 25 ms；约 300 ms 缓冲吸收后未产生 `under`。
- 新增 pre-fix 日志中 `upLost` 在约 15 秒内由 19 增至 22，`upGap=2`；同期
  `audioLoopGapMax<=2`、`usbTxGapMax<=2`、`usbTxDone=192..192`，确认 USB 和音频任务稳定。
- `SequenceTracker` 对每个孤立缺口插入一帧淡出到静音，再将当前帧淡入；
  即使队列不欠载，也会产生约 28 ms 的可闻音量凹陷。
- post-fix 累计日志显示 `upLost=1182`、`upBridge=1158`、`upGap=7`；
  短缺口桥接覆盖约 98%，但仍有 24 个帧属于 3～7 包连续缺口。
- 该组快照的 `tx=0/rx=0` 表示 USB 流已经停止，只能使用累计缺口统计，
  不能把其中的 `mic=0` 或 `usbTx=0` 解释为播放欠载。

## Instrumentation Plan
- [DONE] 网络 PCM 包继续记录最大墙钟间隔、源时间间隔、丢包和序号缺口。
- [DONE] 新增 `audioLoopGapMax`，记录音频任务相邻循环的最大间隔。
- [DONE] 新增 `usbTxGapMax/usbTxAdvanceMax`，区分 USB IN 停顿与观察任务晚醒。
- [DONE] 新增 `usbTxDone=min..max`，确认主机实际完成的 USB IN 包长。
- [DONE] 播放队列继续以每个音频任务循环采样 `mic=min..max`。
- [DONE] 串口事件标识为 `session=daw-phone-stutter run=pre-fix hypotheses=P,Q,R,S`。

## Verification Conclusion
USB 调度和播放缓冲已排除。根因是 DAW→ESP32 的 UDP 序号缺口持续存在，
而第一轮直线桥接会把部分缺口转换成低频斜坡。第二轮同时从发送端增加延迟冗余，
并将残余短缺口改为无端点斜率的平滑淡出/淡入。

## Minimal Fix
- `SequenceTracker` 最初对 `delta<=2` 的缺口在上一有效样本与当前帧首样本之间
  生成直线桥接；post-fix 仍有偶发爆音，说明该桥接会在端点极性差异较大时产生
  14～28 ms 的低频斜坡。
- `delta>2` 的异常长缺口保留原有最多 300 ms 的淡出、静音和恢复淡入保护。
- 新增累计指标 `upBridge`，并将串口标识切换为
  `session=daw-phone-stutter run=post-fix`。
- 保留 P/Q/R/S 插桩，等待实机比较：丢包发生时 `upLost` 与 `upBridge`
  应同步增长，同时 `under=0`、`usbTxDone=192..192` 且听感不再短暂卡顿。

## Superseded Fix Iteration 2
- DAW→ESP32 每个 PCM 主包发送后，保存独立副本并在约 3 ms 后发送同序号冗余包。
- ESP32 现有 `SequenceTracker` 自动拒绝已收到主包对应的重复副本；主包丢失时，
  冗余副本在下一帧到达前补位，不增加播放延迟。
- VST 计数器新增 `+R` 冗余成功数，`IO` 显示主发送错误与冗余错误。
- 保留短缺口桥接和长缺口淡出作为双重兜底。等待用户重建 VST 后提供活跃播放
  (`tx=1`) 的 post-fix 日志和听感；预期 `TX:+R≈1:1`，`upLost` 增长率显著下降。
- 短缺口兜底同步改为在缺口自身时长内使用 smoothstep 淡出到零再淡入；
  该曲线在端点处斜率为零，不再合成跨越两个任意样本端点的直流斜坡，且不会
  额外淡入或改写后续完整帧。
- 延迟冗余随后被撤销：主包与副本位于同一被周期阻塞的发送线程，无法解决固定
  5 秒停顿，且会无必要地把网络包量翻倍。smoothstep 丢包兜底继续保留。

## Root Fix Iteration 3
- 用户确认卡顿具有约 5 秒固定周期。VST 的发送线程恰好每 5000 ms 调用
  `rebuildDiscoveryEndpoints()`，其中包含网卡枚举和每接口 socket 创建；
  该同步操作与 PCM 主包、冗余包发送共用线程，能同时延迟两者。
- 网卡端点刷新改为仅在 `peerIp` 为空、即设备尚未选中时执行。连接期间停止刷新；
  若租约失效或设备重启，`clearPeer()` 清空 `peerIp`，下一轮会立即恢复刷新。
- ESP 的 5 秒完整串口快照改为仅在 USB TX/RX 均不活跃时输出，防止诊断本身成为
  另一个固定周期实时干扰源；音频活跃期间指标继续累计，停止后立即输出。
- 移除同线程冗余副本方案，最终修复只隔离周期维护工作，不增加音频网络负载。

## Final Evidence Gate
- **Evidence Sources**: 用户提供的 pre-fix 活跃播放日志；post-fix 累计日志
- **progress_status**: collecting
- **Hypothesis Conclusions**: P CONFIRMED；Q REJECTED；R REJECTED；S CONFIRMED；T CONFIRMED；U CONFIRMED；V INCONCLUSIVE
