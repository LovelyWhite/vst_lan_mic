# Debug Session: daw-phone-stutter
- **Status**: [OPEN]
- **progress_status**: analyzed
- **Issue**: DAW 到手机方向稳定运行时偶发短暂卡顿。
- **Evidence Source**: ESP32 现有 `[DEBUG]` 串口结构化日志；用户提供的 pre-fix 日志位于 `/Users/lw/Library/Application Support/Trae/User/workspaceStorage/2ce1acc2116fbc90cc3eda684aceb4ac/long-text/6ad36df8bb0d6a128bbe996b/mvekzhu1-qpw5/[DEBUG] se....txt`
- **Evidence Directory**: `/tmp/trae-debug-vst_lan_mic-daw-phone-stutter`（本轮使用现有本地日志路径，不启用 HTTP）

## Reproduction Steps
1. 同时保持 `vst2esp`、ESP32 USB Audio 和手机音频链路工作。
2. 连续播放 DAW 音频，等待偶发短暂卡顿。
3. 保存包含卡顿前后诊断快照的 ESP32 串口日志。

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| P | 网络包短时丢失或到达间隔尖峰 | Medium | Low | CONFIRMED：新日志中 `upLost` 由 19 增至 22，现有补偿会为每个孤立丢包制造约 28 ms 淡出/淡入凹陷 |
| Q | ESP 播放缓冲瞬时跌穿或校正裁剪 | Medium | Low | REJECTED：全程无欠载/裁剪/溢出，最低仍约有 276 ms 音频 |
| R | USB IN 1 ms 调度偶发停顿 | High | Medium | REJECTED：`usbTxGapMax=2`、`usbTxAdvanceMax=2`、`usbTxDone=192..192` |
| S | DAW 发送节拍偶发暂停但被 5 秒汇总掩盖 | Medium | Medium | INCONCLUSIVE：源间隔最大 27 ms，没有卡顿时间标记可做相关性判断 |

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

## Instrumentation Plan
- [DONE] 网络 PCM 包继续记录最大墙钟间隔、源时间间隔、丢包和序号缺口。
- [DONE] 新增 `audioLoopGapMax`，记录音频任务相邻循环的最大间隔。
- [DONE] 新增 `usbTxGapMax/usbTxAdvanceMax`，区分 USB IN 停顿与观察任务晚醒。
- [DONE] 新增 `usbTxDone=min..max`，确认主机实际完成的 USB IN 包长。
- [DONE] 播放队列继续以每个音频任务循环采样 `mic=min..max`。
- [DONE] 串口事件标识为 `session=daw-phone-stutter run=pre-fix hypotheses=P,Q,R,S`。

## Verification Conclusion
根因确认在短序号缺口的 PCM 补偿策略，而不是缓冲或 USB 调度。对 1～2 帧缺口应改用连续桥接，避免淡出到静音；长缺口继续使用静音保护。

## Minimal Fix
- `SequenceTracker` 对 `delta<=2` 的缺口在上一有效样本与当前帧首样本之间生成
  连续桥接，并按原序输出当前完整帧，不再经过淡出到静音和恢复淡入。
- `delta>2` 的异常长缺口保留原有最多 300 ms 的淡出、静音和恢复淡入保护。
- 新增累计指标 `upBridge`，并将串口标识切换为
  `session=daw-phone-stutter run=post-fix`。
- 保留 P/Q/R/S 插桩，等待实机比较：丢包发生时 `upLost` 与 `upBridge`
  应同步增长，同时 `under=0`、`usbTxDone=192..192` 且听感不再短暂卡顿。

## Final Evidence Gate
- **Evidence Sources**: 用户提供的 `post-fix-17` 串口日志
- **progress_status**: collecting
- **Hypothesis Conclusions**: P CONFIRMED；Q REJECTED；R REJECTED；S INCONCLUSIVE
