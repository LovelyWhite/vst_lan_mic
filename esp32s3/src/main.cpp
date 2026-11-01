#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_arduino_version.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp32-hal-tinyusb.h>
#include <cmath>
#include <opus.h>

// Audio Tools supplies its own class driver. Older cores use the CUSTOM slot;
// Arduino 3.3.12 and newer expose the AUDIO slot directly.
#if ESP_ARDUINO_VERSION_MAJOR != 2 && ESP_ARDUINO_VERSION_MAJOR != 3
#error Review the USB audio adapter for this Arduino major version.
#endif
#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#define USB_INTERFACE_AUDIO USB_INTERFACE_CUSTOM
#endif
#include <AudioTools/Communication/USB/USBAudioStream.h>
#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#undef USB_INTERFACE_AUDIO
#endif
#include "AudioBuffer.h"
#include "NonBlockingUdp.h"

namespace {
constexpr uint8_t kWifiResetPin = 0;
constexpr uint32_t kWifiResetHoldMs = 3000;
// How long the USB bridge must stay quiet before Wi-Fi modem sleep is
// re-enabled; holding the radio awake costs real current while idle.
constexpr uint32_t kWifiIdleSleepMs = 5000;
constexpr uint32_t kWifiSnapshotIntervalMs = 1000;
// ISO-IN transfers complete once per millisecond while the host keeps the
// streaming alt setting open, so a 100 ms silence in the ISR-side completion
// counter means the endpoint is closed (or the cable pulled; mounted() covers
// that instantly).
constexpr uint32_t kUsbTxActiveTimeoutMs = 100;
constexpr uint8_t kStatusLedPin = 18;
constexpr bool kStatusLedActiveHigh = true;
// Opus PLC synthesizes at most 300 ms of concealment; longer gaps reset the
// decoder instead of padding with noise.
constexpr uint32_t kMaxPlcFrames = 300 / sos::kOpusFrameDurationMs;

audio_tools::USBAudioStream usb;
constexpr uint32_t kUsbUnityGainQ16 = 65536;
std::atomic<uint32_t> usbSpeakerLeftGainQ16{kUsbUnityGainQ16};
std::atomic<uint32_t> usbSpeakerRightGainQ16{kUsbUnityGainQ16};
std::atomic<uint32_t> usbSpeakerLeftMuted{0};
std::atomic<uint32_t> usbSpeakerRightMuted{0};
sos::MonoPlayout microphone; // Uplink (DAW -> phone) stays mono.
struct CapturedFrame {
    sos::OpusFrame pcm{};
    uint32_t capturedMs = 0, sequence = 0;
};
sos::SpscRing<CapturedFrame, 8> captured;
std::atomic<uint32_t> uplinkEpoch{0}, lastUplinkMs{0};
std::atomic<bool> wifiResetInProgress{false};
std::atomic<bool> statusConfigPortalActive{false};
std::atomic<bool> statusAudioStreaming{false};
std::atomic<bool> statusResetArmed{false};
std::atomic<bool> statusError{false};
std::atomic<uint32_t> wifiSnapshotConnected{0};
std::atomic<uint32_t> wifiSnapshotIpRaw{0};
std::atomic<int32_t> wifiSnapshotRssi{-128};
char identity[64]{}, usbSerial[24]{};
uint32_t bootSession = 0;

// ---- Aggregated debug statistics: one CSV line per second, printed by ----
// loop() on the Arduino task. Hot tasks only touch lock-free relaxed
// atomics here -- no locks, no delays, no per-frame prints, so scheduling
// and the ISR paths stay untouched. Event counters accumulate since boot;
// window metrics (loop-time/FIFO max/min) are swapped out by the 1 Hz
// reporter which also resets the cell for the next window.
struct DebugStats {
    // audioTask Pacer 的超期/重锚次数（wait() 语义，超时即重锚、不追赶，
    // 两者恒相等）。
    std::atomic<uint32_t> audioPacerOverrun{0}, audioPacerAnchor{0};
    // networkTask 轮询 Pacer 的超期/重锚次数。该 Pacer 只管 socket 轮询
    // 节拍（5 ms/50 ms），下行发送由入队通知驱动，不占它。口径注意
    // （见 Pacer::waitNotify）：到点自然醒来不计 overrun，n_ovr 只统计
    // 错过一个完整轮询周期（退避后返回/单轮超时），与 a_ovr 不对齐；
    // n_anch 另含通知提前唤醒的事件重锚，故 n_anch >= n_ovr。
    std::atomic<uint32_t> netPacerOverrun{0}, netPacerAnchor{0};
    // 单轮循环工作耗时最大值（微秒，1 秒窗口；不含 pacer/backoff 睡眠）。
    std::atomic<uint32_t> audioLoopMaxUs{0}, netLoopMaxUs{0};
    // captured 队列：push 失败=音频任务队满丢下行帧；pop 失败=被入队通知
    // 唤醒时队列已空（帧已被上一轮消费的通知竞态，应恒接近 0）。
    std::atomic<uint32_t> queuePushFail{0}, queuePopFail{0};
    // PLC 补偿合成的帧数 / 上行 Opus 解码器复位次数（均自启动累计）。
    std::atomic<uint32_t> plcFrames{0}, uplinkResets{0};
    // UDP 发送失败次数（control 应答 + audio 下行两路，累计）。
    std::atomic<uint32_t> udpEndPacketFail{0};
    // WiFi modem sleep 状态切换次数（进/出省电各计一次，累计）。
    std::atomic<uint32_t> wifiSleepToggles{0};
    // USB TX FIFO 可写空间窗口值（字节；min=最满时，max=最空时）。
    std::atomic<int32_t> usbTxFifoMinFree{INT32_MAX};
    std::atomic<int32_t> usbTxFifoMaxFree{-1};
};
DebugStats stats; // 唯一统计块：热任务只写 relaxed 原子，loop 任务 1 Hz 读取。
TaskHandle_t audioTaskHandle = nullptr, networkTaskHandle = nullptr; // 供 1 Hz 栈高水位查询

// Publish a window max/min via CAS so a better sample landing between the
// reporter's read and reset can never be clobbered by a stale value.
void statsMax(std::atomic<uint32_t>& cell, uint32_t value) {
    uint32_t current = cell.load(std::memory_order_relaxed);
    while (value > current &&
           !cell.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {}
}

void statsMax(std::atomic<int32_t>& cell, int32_t value) {
    int32_t current = cell.load(std::memory_order_relaxed);
    while (value > current &&
           !cell.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {}
}

void statsMin(std::atomic<int32_t>& cell, int32_t value) {
    int32_t current = cell.load(std::memory_order_relaxed);
    while (value < current &&
           !cell.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {}
}

void setStatusLed(bool on) {
    digitalWrite(kStatusLedPin, on == kStatusLedActiveHigh ? HIGH : LOW);
}

void refreshWifiSnapshot() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    wifiSnapshotConnected.store(connected ? 1u : 0u, std::memory_order_release);
    wifiSnapshotIpRaw.store(connected ? uint32_t(WiFi.localIP()) : 0u,
                            std::memory_order_release);
    wifiSnapshotRssi.store(connected ? int32_t(WiFi.RSSI()) : -128,
                           std::memory_order_release);
}

void statusLedTask(void*) {
    for (;;) {
        const uint32_t now = millis();
        bool on = false;
        // Poll just fast enough to catch the next edge of the active pattern;
        // steady states can idle far longer than a fixed 25 ms tick.
        uint32_t pollMs = 25;
        if (statusError.load(std::memory_order_relaxed)) {
            const uint32_t phase = now % 1000;
            on = phase < 100 || (phase >= 200 && phase < 300);
        } else if (statusResetArmed.load(std::memory_order_relaxed)) {
            on = ((now / 75) & 1u) == 0;
        } else if (statusConfigPortalActive.load(std::memory_order_relaxed)) {
            on = ((now / 500) & 1u) == 0;
            pollMs = 125;
        } else if (!wifiSnapshotConnected.load(std::memory_order_acquire)) {
            on = ((now / 125) & 1u) == 0;
            pollMs = 31;
        } else if (statusAudioStreaming.load(std::memory_order_relaxed)) {
            // Brief flash per second instead of a 90% duty glow: streaming
            // stays visible while the LED current budget drops ~9x.
            on = now % 1000 < 100;
            pollMs = 50;
        } else {
            on = true;
            pollMs = 250;
        }
        setStatusLed(on);
        vTaskDelay(pdMS_TO_TICKS(pollMs));
    }
}

uint32_t usbGainQ16(float volume) {
    const float bounded = std::max(0.0f, std::min(volume, 1.0f));
    if (bounded <= 0.0f) return 0;
    // Audio Tools maps the advertised -100..0 dB UAC range to 0..1.
    // Convert that control position back to an amplitude gain.
    const float gain = std::pow(10.0f, (bounded - 1.0f) * 5.0f);
    return uint32_t(gain * float(kUsbUnityGainQ16) + 0.5f);
}

void onUsbVolume(float volume, uint8_t channel) {
    const uint32_t gain = usbGainQ16(volume);
    if (channel == 0 || channel == 1)
        usbSpeakerLeftGainQ16.store(gain, std::memory_order_release);
    if (channel == 0 || channel == 2)
        usbSpeakerRightGainQ16.store(gain, std::memory_order_release);
}

void onUsbMute(bool muted, uint8_t channel) {
    if (channel == 0 || channel == 1)
        usbSpeakerLeftMuted.store(muted ? 1u : 0u, std::memory_order_release);
    if (channel == 0 || channel == 2)
        usbSpeakerRightMuted.store(muted ? 1u : 0u, std::memory_order_release);
}

struct Peer {
    IPAddress ip;
    uint16_t port = 0;
    uint32_t session = 0, seenMs = 0;
    bool active(uint32_t now) const {
        return port != 0 && uint32_t(now - seenMs) < sos::kLeaseMs;
    }
    bool matches(IPAddress address, uint16_t sourcePort, uint32_t id) const {
        return port != 0 && ip == address && port == sourcePort && session == id;
    }
};

// One FreeRTOS tick in microseconds, derived from the config instead of
// assuming a 1 kHz tick so a tick-rate change cannot silently stretch the
// hot loops.
constexpr int64_t kTickUs = 1000000 / int64_t(configTICK_RATE_HZ);

// Fixed-period pacing for the hot loops. Deadlines are tracked in the
// esp_timer refclock (monotonic microseconds) rather than with relative
// vTaskDelay calls, whose period degrades to "one tick plus loop work" and
// accumulates drift. Each wait converts the refclock deadline to an absolute
// tick index and sleeps via xTaskDelayUntil, so loop work can neither stretch
// the period nor phase-shift it; being tick-anchored it also cannot undersleep
// and start spinning. An overrun (or one of the mid-loop backoff sleeps)
// re-anchors the schedule to now instead of bursting one catch-up pass per
// missed deadline.
struct Pacer {
  TickType_t periodTicks;
  TickType_t nextTick;
  // Debug counters: a missed deadline is always re-anchored (never caught up
  // in a burst). wait() 的两个计数恒相等；waitNotify() 的 anchor 另计
  // 通知提前唤醒的事件重锚，故 anchor >= overrun。
  // overrun=错过截止期次数；anchor=全部重锚次数（超期 + waitNotify 事件）。
  uint32_t overrun = 0, anchor = 0;

  explicit Pacer(int64_t periodUs) {
    periodTicks = usToTicks(periodUs);
    nextTick = xTaskGetTickCount();
  }

  static TickType_t usToTicks(int64_t us) {
    return TickType_t((us + kTickUs - 1) / kTickUs); // 向上取整
  }

  void setPeriodUs(int64_t us) {
    periodTicks = usToTicks(us);
  }

  void wait() {
    TickType_t now = xTaskGetTickCount();
    if ((now - nextTick) < periodTicks) {
      // 还没到点，阻塞到 nextTick
      xTaskDelayUntil(&nextTick, periodTicks);
    } else {
      // 已经超时，直接重锚，不追赶
      ++overrun;
      ++anchor;
      nextTick = now;
    }
  }

  // 睡到下一个轮询截止期，但可被任务通知提前唤醒（用于 networkTask：
  // audioTask 每入队一帧下行数据就通知一次）。返回 true=被通知提前唤醒
  // （消费侧据此区分「竞态空队列」与正常空探测），false=睡到截止期。
  // 注意：任务通知是唤醒信号而非信号量——清零式取走不保证逐通知消费，
  // 消费侧必须把队列排空（见 networkTask 的下行发送循环）。
  // 网格语义：进入时 now ∈ [nextTick, nextTick+period) 本轮有效，睡
  // period-diff 到下一网格点；通知提前唤醒则以事件为准重锚网格（事件
  // 驱动优先，轮询网格只是兜底）。overrun 只统计错过一个完整轮询周期
  // （退避睡眠后返回/单轮工作超时）；anchor 另含通知重锚，故 n_anch
  // >= n_ovr，与 audioTask 的 a_ovr 口径不同。
  bool waitNotify() {
    TickType_t now = xTaskGetTickCount();
    const TickType_t diff = TickType_t(now - nextTick);
    if (diff < periodTicks) {
      // 等待时长必须用 period-diff 而不是 nextTick-now：本分支里
      // now >= nextTick 恒成立，后者在 now==nextTick 时为 0——
      // ulTaskNotifyTake(0) 立即返回，与下一轮的重锚分支形成乒乓自旋
      // （曾以 n_ovr=21 万、core 1 烧满的形式复现）；在 now 领先 1~4
      // tick 时则无符号回绕成 ~49 天超时，任务睡死——且队列随后压满、
      // push 失败不再通知，再也无法唤醒。period-diff 恒在 (0, period]。
      const TickType_t remaining = TickType_t(periodTicks - diff);
      if (ulTaskNotifyTake(pdTRUE, remaining) != 0) {
        // 新帧提前唤醒：事件时刻即新网格锚点（不 ++overrun——这是提前
        // 而非超期；重锚后下一轮 diff≈0，睡满一个兜底周期）。
        ++anchor;
        nextTick = now;
        return true;
      }
      // 睡到网格点：本轮消耗掉该时段，网格前移一个周期
      nextTick = TickType_t(nextTick + periodTicks);
      return false;
    }
    // 已超过一个完整轮询周期（中途退避睡眠或单轮工作超时）：重锚，不追赶
    ++overrun;
    ++anchor;
    nextTick = now;
    return false;
  }
};

void audioTask(void*) {
    Pacer pacer{1000};
    uint32_t epoch = uplinkEpoch.load();
    bool txWasActive = false, rxWasActive = false, uplinkWasFresh = false;
    // Stereo frames doubled the buffer; keeping it off the 8 KB task stack.
    static CapturedFrame frame;
    size_t frameUsed = 0;
    uint32_t captureSequence = 0;
    uint32_t lastStreamActiveMs = 0;
    bool wifiModemSleeping = true; // IDF default while associated.
    std::array<uint8_t, 192> speakerBytes{};
    std::array<int16_t, 96> micSamples{};
    int32_t speakerLeftGainQ16 = int32_t(kUsbUnityGainQ16);
    int32_t speakerRightGainQ16 = int32_t(kUsbUnityGainQ16);
    // See the audio loop below for why the completion counter is the
    // authority for TX liveness. lastUsbTxProgressMs starts at 0: no progress
    // observed, so TX stays inactive until the first ISR-side transfer
    // completes.
    uint32_t lastUsbTxCount = usb.getTxXferCount();
    uint32_t lastUsbTxProgressMs = 0;
    auto smoothGain = [](int32_t current, int32_t target) {
        const int32_t difference = target - current;
        if (difference == 0) return current;
        int32_t step = difference / 240; // About 5 ms at 48 kHz.
        if (step == 0) step = difference > 0 ? 1 : -1;
        return current + step;
    };
    for (;;) {
        const int64_t audioLoopStartUs = esp_timer_get_time(); // 本轮起点，仅用于统计工作耗时
        const uint32_t audioLoopNow = millis();
        // isStreamingActiveTx() reads ep_in, a field written by the USB
        // device task/ISR and read here without cross-context ordering; the
        // library documents platforms where it keeps returning false while
        // ISO-IN transfers are actually running (USBAudioDeviceBase.h "known
        // issues"), and the ESP32-S3 port reproduces it: snapshots showed
        // txXfer climbing ~1k/s while tx=0 and the supply loop never ran,
        // leaving the phone fed with zero-padding -- the DAW->phone stutter.
        // Trust the ISR-side completion counter instead: audiod_xfer_cb()
        // advances it once per millisecond only while the IN pipe is live,
        // so recent progress is proof of a streaming alt setting.
        const uint32_t usbTxCount = usb.getTxXferCount();
        const bool usbTxProgress = usbTxCount != lastUsbTxCount;
        const uint32_t sinceProgressMs = audioLoopNow - lastUsbTxProgressMs;
        if (usbTxProgress) {
            lastUsbTxCount = usbTxCount;
            lastUsbTxProgressMs = audioLoopNow;
        }
        const bool txActive = usb.mounted() &&
            (usbTxProgress || sinceProgressMs < kUsbTxActiveTimeoutMs);
        const bool rxActive = usb.mounted() && usb.isStreamingActiveRx();
        statusAudioStreaming.store(txActive || rxActive, std::memory_order_relaxed);
        // Modem sleep trades a little Wi-Fi latency for a large idle power
        // saving, so it only returns after the USB bridge has been quiet.
        if (txActive || rxActive) {
            lastStreamActiveMs = millis();
            if (wifiModemSleeping) {
                wifiModemSleeping = false;
                WiFi.setSleep(false);
                ++stats.wifiSleepToggles;
            }
        } else if (!wifiModemSleeping &&
                   uint32_t(millis() - lastStreamActiveMs) >= kWifiIdleSleepMs) {
            WiFi.setSleep(true);
            wifiModemSleeping = true;
            ++stats.wifiSleepToggles;
        }
        const auto newEpoch = uplinkEpoch.load(std::memory_order_acquire);
        const bool fresh = uint32_t(millis() - lastUplinkMs.load()) < sos::kAudioTimeoutMs;
        if (newEpoch != epoch || txActive != txWasActive || (uplinkWasFresh && !fresh)) {
            microphone.reset();
            epoch = newEpoch;
        }
        txWasActive = txActive;
        uplinkWasFresh = fresh;

        // Top the USB IN FIFO up to (almost) full on every pass. Flow control
        // drains one 192-byte stereo packet per millisecond and zero-pads any
        // shortfall -- audible as a click. The steady FIFO level IS the stall
        // budget: the old 1536-byte floor was 8 ms in stereo and the observed
        // 5-6 ms scheduling jitter emptied it (debug usbTx=0..1536 with
        // usbTxGapMax=5), and even the 15 ms of a 16-packet FIFO still let the
        // rarer longer stalls -- flash cache misses, WiFi internals -- surface
        // as isolated clicks. The config now sizes the FIFO at 48 packets
        // (~47 ms); this full top-up is what holds that level steady.
        // Checking free space avoids Audio Tools' blocking write retry path,
        // and the free-space gate doubles as the poison-pill filter here:
        // write() re-checks the same unreliable isStreamingActiveTx() flag
        // and silently discards the packet while reporting success, so feed
        // bufferTx() directly (the only other writer is this task, and the
        // ISR-side xfer callback only drains, so the free-space observation
        // cannot go stale towards "less room").
        if (txActive) {
            // Track writable space: min shows how full the FIFO stayed, max
            // shows whether it ever approached empty during this window.
            const int32_t txFree = int32_t(usb.availableForWrite());
            statsMin(stats.usbTxFifoMinFree, txFree);
            statsMax(stats.usbTxFifoMaxFree, txFree);
            microphone.prepare();
            for (int batch = 0; batch < 8 &&
                 usb.availableForWrite() >= int(sizeof(micSamples)); ++batch) {
                for (size_t i = 0; i < 48; ++i) {
                    const int16_t mono = fresh ? sos::pcm16(microphone.next()) : 0;
                    micSamples[2 * i] = mono;
                    micSamples[2 * i + 1] = mono;
                }
                usb.bufferTx().writeArray(
                    reinterpret_cast<uint8_t*>(micSamples.data()),
                    int(sizeof(micSamples)));
            }
        } else {
            microphone.reset();
        }

        if (rxActive != rxWasActive) {
            frameUsed = 0;
            // Only this task consumes bufferRx().
            while (usb.available() > 0)
                usb.readBytes(speakerBytes.data(), speakerBytes.size());
        }
        rxWasActive = rxActive;
        for (int batch = 0; rxActive && batch < 8 && usb.available() >= 4; ++batch) {
            const size_t bytes = std::min(size_t(usb.available()), speakerBytes.size()) & ~size_t(3);
            const size_t got = usb.readBytes(speakerBytes.data(), bytes);
            const int32_t targetLeftGainQ16 = usbSpeakerLeftMuted.load(
                std::memory_order_acquire)
                ? 0 : int32_t(usbSpeakerLeftGainQ16.load(std::memory_order_acquire));
            const int32_t targetRightGainQ16 = usbSpeakerRightMuted.load(
                std::memory_order_acquire)
                ? 0 : int32_t(usbSpeakerRightGainQ16.load(std::memory_order_acquire));
            for (size_t i = 0; i + 3 < got; i += 4) {
                const uint16_t l = sos::get16(speakerBytes.data() + i);
                const uint16_t r = sos::get16(speakerBytes.data() + i + 2);
                const int32_t left = l < 32768 ? int32_t(l) : int32_t(l) - 65536;
                const int32_t right = r < 32768 ? int32_t(r) : int32_t(r) - 65536;
                speakerLeftGainQ16 = smoothGain(
                    speakerLeftGainQ16, targetLeftGainQ16);
                speakerRightGainQ16 = smoothGain(
                    speakerRightGainQ16, targetRightGainQ16);
                const int32_t scaledLeft = int32_t(
                    (int64_t(left) * speakerLeftGainQ16) >> 16);
                const int32_t scaledRight = int32_t(
                    (int64_t(right) * speakerRightGainQ16) >> 16);
                // True stereo downlink: keep the L/R separation the phone sent
                // instead of folding both channels into one averaged sample.
                frame.pcm[frameUsed++] = int16_t(scaledLeft);
                frame.pcm[frameUsed++] = int16_t(scaledRight);
                if (frameUsed == frame.pcm.size()) {
                    frame.capturedMs = millis();
                    frame.sequence = captureSequence++;
                    if (!captured.push(&frame, 1)) {
                        ++stats.queuePushFail;
                    } else if (networkTaskHandle) {
                        // 事件驱动下行：入队即以直接任务通知唤醒
                        // networkTask，取代旧的 1 ms 轮询探测。
                        // （句柄可能尚未创建，audioTask 先于 networkTask 启动。）
                        xTaskNotifyGive(networkTaskHandle);
                    }
                    frameUsed = 0;
                }
            }
        }
        // Serve USB at 1 kHz only while a direction is live; an idle bridge
        // slows to 200 Hz, adding at most 5 ms to start-of-stream detection,
        // which the 300 ms network playout buffer hides.
        pacer.setPeriodUs((txActive || rxActive) ? 1000 : 5000);
        // Loop work time only (pacer sleep excluded): the max shows how close
        // a pass comes to the 1 ms period budget.
        statsMax(stats.audioLoopMaxUs,
                 uint32_t(esp_timer_get_time() - audioLoopStartUs));
        pacer.wait();
        stats.audioPacerOverrun.store(pacer.overrun, std::memory_order_relaxed);
        stats.audioPacerAnchor.store(pacer.anchor, std::memory_order_relaxed);
    }
}

void networkTask(void*) {
    // Socket 轮询节拍（发现保活 + 上行接收 + 租约检查）。下行发送不依赖
    // 该节拍：由 audioTask 的入队通知驱动（见 waitNotify）。无 socket 时
    // 降为 50 ms（重连本身是定时器驱动）。5 ms 轮询对 20 ms 的上行帧周期
    // 引入的抖动被上行 300 ms 目标缓冲完全吸收。
    Pacer pacer{5000};
    NonBlockingUdp control, audio;
    NonBlockingUdp::Packet received;
    Peer sender, receiver;
    bool uplinkSequenceValid = false;
    uint32_t nextUplinkSequence = 0;
    uint32_t lastRetry = 0, lastValidAudio = 0;
    uint32_t nextDownlinkAttemptMs = 0;
    uint32_t downlinkBackoffMs = 10;
    bool socketsReady = false;
    bool notified = false; // 上一轮 waitNotify 是被入队通知唤醒的（供消费块区分竞态空队列）
    CapturedFrame pendingDownlink;
    IPAddress boundIp, currentIp;
    std::array<uint8_t, sos::kMaxPacketSize + 1> packet{};
    NonBlockingUdp::Ready controlReady, audioReady;
    const int opusEncoderBytes = opus_encoder_get_size(sos::kOpusChannels);
    // Keep the complete encoder state in internal RAM; do not fall back to PSRAM.
    void* opusMemory = opusEncoderBytes > 0
        ? heap_caps_malloc(size_t(opusEncoderBytes), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : nullptr;
    auto* opusEncoder = static_cast<OpusEncoder*>(opusMemory);
    int opusStatus = opusEncoder
        ? opus_encoder_init(opusEncoder, sos::kSampleRate, sos::kOpusChannels,
                            OPUS_APPLICATION_AUDIO)
        : OPUS_ALLOC_FAIL;
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(sos::kOpusBitrate));
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_VBR(0));
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(4));
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    if (opusStatus != OPUS_OK) {
        Serial.printf("Opus encoder initialization failed: %d\n", opusStatus);
        if (opusMemory) heap_caps_free(opusMemory);
        vTaskDelete(nullptr);
        return;
    }

    // Uplink decoder mirrors the plugin's mono 20 ms encoder profile; like the
    // encoder it stays in internal RAM.
    sos::Frame decoded{};
    const int opusDecoderBytes = opus_decoder_get_size(sos::kUplinkChannels);
    void* opusDecoderMemory = opusDecoderBytes > 0
        ? heap_caps_malloc(size_t(opusDecoderBytes), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : nullptr;
    auto* opusDecoder = static_cast<OpusDecoder*>(opusDecoderMemory);
    int opusDecoderStatus = opusDecoder
        ? opus_decoder_init(opusDecoder, sos::kSampleRate, sos::kUplinkChannels)
        : OPUS_ALLOC_FAIL;
    if (opusDecoderStatus != OPUS_OK) {
        Serial.printf("Opus decoder initialization failed: %d\n", opusDecoderStatus);
        if (opusMemory) heap_caps_free(opusMemory);
        if (opusDecoderMemory) heap_caps_free(opusDecoderMemory);
        vTaskDelete(nullptr);
        return;
    }
    auto resetUplink = [&] {
        uplinkSequenceValid = false;
        ++stats.uplinkResets;
        opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
    };

    auto invalidateSender = [&] {
        sender = Peer{};
        resetUplink();
        ++uplinkEpoch;
    };
    for (;;) {
        const int64_t netLoopStartUs = esp_timer_get_time(); // 本轮起点，仅用于统计工作耗时
        if (wifiResetInProgress.load(std::memory_order_acquire)) {
            if (socketsReady) {
                control.stop();
                audio.stop();
                socketsReady = false;
            }
            statsMax(stats.netLoopMaxUs,
                     uint32_t(esp_timer_get_time() - netLoopStartUs));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const uint32_t now = millis();
        const bool wifiConnected =
            wifiSnapshotConnected.load(std::memory_order_acquire) != 0;
        currentIp = IPAddress(wifiSnapshotIpRaw.load(std::memory_order_acquire));
        if (!wifiConnected || (socketsReady && boundIp != currentIp)) {
            if (socketsReady) {
                control.stop(); audio.stop();
                socketsReady = false;
                invalidateSender();
                receiver = Peer{};
            }
            if (uint32_t(now - lastRetry) >= 5000) {
                lastRetry = now;
                WiFi.reconnect();
            }
        } else if (!socketsReady) {
            control.stop(); audio.stop();
            const bool controlBound = control.begin(sos::kDiscoveryPort);
            const bool audioBound = audio.begin(sos::kUplinkPort);
            socketsReady = controlBound && audioBound;
            if (!socketsReady) {
                control.stop(); audio.stop();
                statsMax(stats.netLoopMaxUs,
                         uint32_t(esp_timer_get_time() - netLoopStartUs));
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            boundIp = currentIp;
            Serial.printf("Wi-Fi ready: %s, device %s\n", boundIp.toString().c_str(), identity);
        }

        if (sender.port && !sender.active(now)) invalidateSender();
        if (receiver.port && !receiver.active(now)) receiver = Peer{};

        if (socketsReady) {
            if (!NonBlockingUdp::poll(control, controlReady, audio, audioReady)) {
                control.stop(); audio.stop();
                socketsReady = false;
                invalidateSender();
                receiver = Peer{};
            }
        }

        if (socketsReady) {
            for (int n = 0; n < 8; ++n) {
                const int got = control.read(controlReady, packet.data(), packet.size(), received);
                if (got < 0) {
                    control.stop(); audio.stop();
                    socketsReady = false;
                    invalidateSender();
                    receiver = Peer{};
                    break;
                }
                if (!got) break;
                const int length = received.length;
                const IPAddress ip = received.ip;
                const uint16_t port = received.port;
                sos::Header h;
                if (got != length || !sos::decode(packet.data(), size_t(got), h)) continue;
                sos::Header reply;
                reply.timestampMs = now;
                if (h.kind == sos::Kind::Discover) {
                    reply.kind = sos::Kind::Offer;
                    reply.session = bootSession;
                    reply.payloadBytes = uint16_t(strlen(identity));
                    memcpy(packet.data() + sos::kHeaderSize, identity, reply.payloadBytes);
                } else if (h.kind == sos::Kind::Subscribe) {
                    Peer& peer = h.flags == sos::Sender ? sender : receiver;
                    const bool accepted = !peer.active(now) || peer.matches(ip, port, h.session);
                    if (accepted) {
                        const bool changed = !peer.matches(ip, port, h.session);
                        peer = {ip, port, h.session, now};
                        if (changed && h.flags == sos::Sender) {
                            resetUplink();
                            ++uplinkEpoch;
                        } else if (changed && h.flags == sos::Receiver) {
                            captured.clear();
                            ulTaskNotifyTake(pdTRUE, 0); // 换端清队列，顺带清残留通知
                            opus_encoder_ctl(opusEncoder, OPUS_RESET_STATE);
                        }
                    }
                    reply.kind = sos::Kind::Ack;
                    reply.session = h.session;
                    reply.flags = h.flags | (accepted ? 0 : sos::Busy);
                } else {
                    continue;
                }
                const auto bytes = sos::encode(packet.data(), reply);
                const auto sent = control.sendTo(controlReady, ip, port, packet.data(), bytes);
                if (sent == NonBlockingUdp::SendResult::Error) {
                    ++stats.udpEndPacketFail;
                    control.stop(); audio.stop();
                    socketsReady = false;
                    invalidateSender();
                    receiver = Peer{};
                    break;
                }
            }
        }

        if (socketsReady) {
            for (int n = 0; n < 16; ++n) {
                const int got = audio.read(audioReady, packet.data(), packet.size(), received);
                if (got < 0) {
                    control.stop(); audio.stop();
                    socketsReady = false;
                    invalidateSender();
                    receiver = Peer{};
                    break;
                }
                if (!got) break;
                const int length = received.length;
                const IPAddress ip = received.ip;
                const uint16_t port = received.port;
                sos::Header h;
                if (got != length || !sos::decode(packet.data(), size_t(got), h) ||
                    h.kind != sos::Kind::AudioOpus || !sender.active(now) ||
                    !sender.matches(ip, port, h.session)) continue;
                if (uint32_t(now - lastValidAudio) >= sos::kAudioTimeoutMs)
                    uplinkSequenceValid = false;

                uint32_t gap = 0;
                if (uplinkSequenceValid) {
                    gap = h.sequence - nextUplinkSequence;
                    if (gap >= 0x80000000u) continue; // Duplicate or late packet.
                }
                if (gap > kMaxPlcFrames) {
                    ++stats.uplinkResets;
                    opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
                }
                bool decodeOk = true;
                const uint32_t plcFrames = std::min(gap, kMaxPlcFrames);
                stats.plcFrames.fetch_add(plcFrames, std::memory_order_relaxed);
                for (uint32_t i = 0; i < plcFrames; ++i) {
                    const int samples = opus_decode(opusDecoder, nullptr, 0,
                                                    decoded.data(),
                                                    int(sos::kOpusFrameSamples), 0);
                    if (samples != int(sos::kOpusFrameSamples)) {
                        decodeOk = false;
                        break;
                    }
                    microphone.push(decoded);
                }
                const int samples = decodeOk
                    ? opus_decode(opusDecoder, packet.data() + sos::kHeaderSize,
                                  h.payloadBytes, decoded.data(),
                                  int(sos::kOpusFrameSamples), 0)
                    : OPUS_INTERNAL_ERROR;
                if (samples != int(sos::kOpusFrameSamples)) {
                    resetUplink();
                    continue;
                }
                microphone.push(decoded);
                nextUplinkSequence = h.sequence + 1;
                uplinkSequenceValid = true;
                lastValidAudio = now;
                lastUplinkMs.store(now, std::memory_order_release);
            }
        }

        // Keep every produced frame and catch up at 4 ms per packet against a
        // 20 ms production period: the phone playout buffer absorbs the burst
        // with no audible hole, while dropping to the newest frame punched a
        // 20 ms gap into the stream for every scheduler hiccup. On real
        // congestion (sendto fails) drop the frame -- retransmitting into
        // a congested queue amplifies the loss.
        // 下行为事件驱动：audioTask 每入队一帧即以任务通知唤醒本任务（另有
        // 5 ms 轮询兜底）。通知只是唤醒信号而非信号量——清零式取走不保证
        // 逐通知消费，唤醒一次就要把队列消费到排空（否则 burst 入队只发
        // 一帧，余帧要等下次唤醒），因此发送放在 for(;;) 循环里；4 ms
        // 追赶限速的检查在循环内且每帧刷新时间戳，只隔帧、不锁死整轮，
        // 未到点的剩余帧由下一次唤醒（新通知或轮询到点，最迟 5 ms）继续
        // 发送，积压按 ~5 ms/帧排空，300 ms 手机端缓冲完全吸收。
        // queuePopFail 只在「被通知唤醒却已空」时计数（帧已被上一轮消费
        // 的通知竞态）；循环内排空后的 pop 失败是正常终止，不计。
        const uint32_t attemptNow = millis();
        if (!socketsReady || !receiver.active(attemptNow)) {
            captured.clear();
            ulTaskNotifyTake(pdTRUE, 0); // 丢帧后清掉残留通知，避免一次空唤醒
            nextDownlinkAttemptMs = attemptNow;
            downlinkBackoffMs = 10;
        } else if (notified && captured.size() == 0) {
            ++stats.queuePopFail;
        } else {
            for (;;) {
                const uint32_t sendNow = millis();
                if (int32_t(sendNow - nextDownlinkAttemptMs) < 0) break;
                if (!audioReady.write) {
                    nextDownlinkAttemptMs = sendNow + downlinkBackoffMs;
                    downlinkBackoffMs =
                        std::min(downlinkBackoffMs * 2, uint32_t(100));
                    break;
                }
                if (!captured.pop(pendingDownlink)) break; // 队列已排空
                const int encoded = opus_encode(
                    opusEncoder, pendingDownlink.pcm.data(), int(sos::kOpusFrameSamples),
                    packet.data() + sos::kHeaderSize, int(sos::kOpusMaxBytes));
                if (encoded <= 0) {
                    // 坏帧丢弃后停一个帧周期再继续
                    nextDownlinkAttemptMs = sendNow + sos::kOpusFrameDurationMs;
                    continue;
                }
                sos::Header h{sos::Kind::AudioOpus, 0, bootSession,
                              pendingDownlink.sequence, pendingDownlink.capturedMs,
                              uint16_t(encoded)};
                const auto bytes = sos::encode(packet.data(), h);
                const auto sent = audio.sendTo(audioReady, receiver.ip, receiver.port,
                                               packet.data(), bytes);
                if (sent == NonBlockingUdp::SendResult::Sent) {
                    downlinkBackoffMs = 10;
                    nextDownlinkAttemptMs = sendNow + 4;
                } else {
                    if (sent == NonBlockingUdp::SendResult::Error) {
                        ++stats.udpEndPacketFail;
                        control.stop(); audio.stop();
                        socketsReady = false;
                        invalidateSender();
                        receiver = Peer{};
                    }
                    nextDownlinkAttemptMs = sendNow + downlinkBackoffMs;
                    downlinkBackoffMs =
                        std::min(downlinkBackoffMs * 2, uint32_t(100));
                    if (!socketsReady) break;
                }
            }
        }
        // Socket 轮询档位：绑定 socket 后 5 ms（发现保活 + 上行接收 +
        // 租约检查；发现/订阅时延从 1 ms 放宽到 5 ms，对交互无感），
        // 无 socket 时 50 ms（重连是定时器驱动，轮询无事可做）。
        // 下行发送不在此节拍上：入队通知会提前唤醒（waitNotify）。
        pacer.setPeriodUs(socketsReady ? 5000 : 50000);
        // Loop work time only (pacer/backoff sleeps excluded).
        statsMax(stats.netLoopMaxUs,
                 uint32_t(esp_timer_get_time() - netLoopStartUs));
        notified = pacer.waitNotify();
        stats.netPacerOverrun.store(pacer.overrun, std::memory_order_relaxed);
        stats.netPacerAnchor.store(pacer.anchor, std::memory_order_relaxed);
    }
}

// 1 Hz 统计行。运行在 Arduino loop 任务（优先级 1，core 1），低于两个热任务，
// 因此一行的 UART 发送时间（~15 ms）和 Wi-Fi/RSSI 查询永远不会抢占音频或网络
// 调度。这是唯一的统计输出；没有任何逐帧打印。
// 字段名直接嵌在每行里（name=value），无需单独表头。各字段含义/单位：
// t_ms=毫秒；*_ovr/*_anch/*_fail/*_frames/*_rst/*_tog=自启动累计次数
// （n_ovr 仅统计错过一个完整轮询周期、n_anch 另含通知事件重锚，与 a_* 口径不同，见 waitNotify）；
// *_loop_max_us=微秒；usb_tx_xfer=每秒 ISO-IN 完成次数增量；usb_fifo_*=可写字节（-1=未采样）；
// rssi_dbm=dBm（未连接 -128）；*_stack_min_b=任务栈最小剩余字节（0=任务未创建）。
void printStatsLine() {
    static uint32_t lastUsbTxCount = 0; // 上一秒的 TX 完成计数快照
    const uint32_t now = millis();
    const uint32_t usbTxCount = usb.getTxXferCount(); // ISR 侧累计完成数（volatile 字读取，跨任务安全）
    const uint32_t usbTxXferDelta = uint32_t(usbTxCount - lastUsbTxCount); // 每秒增量（无符号差，回绕安全）
    lastUsbTxCount = usbTxCount;
    // Swap-out resets the window cells; a better sample landing between the
    // swap and a concurrent writer is lost for at most one window.
    const uint32_t audioLoopUs =
        stats.audioLoopMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netLoopUs =
        stats.netLoopMaxUs.exchange(0, std::memory_order_relaxed);
    const int32_t fifoMin =
        stats.usbTxFifoMinFree.exchange(INT32_MAX, std::memory_order_relaxed);
    const int32_t fifoMax =
        stats.usbTxFifoMaxFree.exchange(-1, std::memory_order_relaxed);
    const long rssi = wifiSnapshotRssi.load(std::memory_order_acquire); // dBm，未连接打印 -128
    // High-water mark in bytes on this Arduino-ESP32 core; 0 while the task never started.
    const uint32_t audioStackMin = audioTaskHandle // audioTask 栈最小剩余字节；0=任务未创建
        ? uint32_t(uxTaskGetStackHighWaterMark(audioTaskHandle)) : 0u;
    const uint32_t netStackMin = networkTaskHandle // networkTask 栈最小剩余字节；0=任务未创建
        ? uint32_t(uxTaskGetStackHighWaterMark(networkTaskHandle)) : 0u;
    Serial.printf(
        "t_ms=%lu,a_ovr=%u,a_anch=%u,n_ovr=%u,n_anch=%u,"
        "a_loop_max_us=%u,n_loop_max_us=%u,usb_tx_xfer=%u,"
        "usb_fifo_min=%d,usb_fifo_max=%d,q_push_fail=%u,q_pop_fail=%u,"
        "plc_frames=%u,ul_rst=%u,udp_send_fail=%u,"
        "wifi_sleep_tog=%u,rssi_dbm=%ld,a_stack_min_b=%u,n_stack_min_b=%u\n",
        (unsigned long)now,
        stats.audioPacerOverrun.load(std::memory_order_relaxed),
        stats.audioPacerAnchor.load(std::memory_order_relaxed),
        stats.netPacerOverrun.load(std::memory_order_relaxed),
        stats.netPacerAnchor.load(std::memory_order_relaxed),
        audioLoopUs, netLoopUs, usbTxXferDelta,
        fifoMin == INT32_MAX ? -1 : int(fifoMin),
        int(fifoMax),
        stats.queuePushFail.load(std::memory_order_relaxed),
        stats.queuePopFail.load(std::memory_order_relaxed),
        stats.plcFrames.load(std::memory_order_relaxed),
        stats.uplinkResets.load(std::memory_order_relaxed),
        stats.udpEndPacketFail.load(std::memory_order_relaxed),
        stats.wifiSleepToggles.load(std::memory_order_relaxed),
        rssi, audioStackMin, netStackMin);
}
} // namespace

void setup() {
    Serial.begin(115200); // UART bridge, not native USB CDC.
    pinMode(kStatusLedPin, OUTPUT);
    setStatusLed(false);
    const auto ledResult = xTaskCreatePinnedToCore(
        statusLedTask, "status-led", 2048, nullptr, 1, nullptr, 0);
    if (ledResult != pdPASS) {
        setStatusLed(true);
        Serial.println("Status LED task creation failed");
    }
    audio_tools::AudioToolsLogger.setLogLevel(audio_tools::AudioLogger::Error);
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(usbSerial, sizeof(usbSerial), "%012llX", (unsigned long long)mac);
    char deviceNumber[7]{};
    snprintf(deviceNumber, sizeof(deviceNumber), "%06llX",
             (unsigned long long)(mac & 0xFFFFFFULL));
    snprintf(identity, sizeof(identity), "%s-%s", sos::kDeviceName, deviceNumber);
    bootSession = esp_random();
    if (!bootSession) bootSession = 1;
    char provisioningSsid[33]{};
    snprintf(provisioningSsid, sizeof(provisioningSsid), "%s", identity);

    audio_tools::USBAudioConfig config;
    config.sample_rate = sos::kSampleRate;
    config.channels = 2;
    config.bits_per_sample = 16;
    config.product = identity;
    config.manufacturer = "suxitech";
    config.serial = usbSerial;
    config.enable_ep_in = config.enable_ep_out = true;
    config.enable_feedback_ep = false;
    config.enable_multi_sample_rate = false;
    // The descriptor reserves 49 samples, but flow control sends exactly
    // 48 stereo samples (192 bytes) per 1 ms frame at 48 kHz.
    config.enable_ep_in_flow_control = true;
    // The IN flow control zero-pads -- an audible click -- as soon as the ISR
    // outruns the supply task, so the steady FIFO level IS the stall budget:
    // 16 packets covered the observed 5-6 ms scheduling jitter but not the
    // rarer longer stalls (flash cache misses, WiFi internals) that surfaced
    // as occasional DAW->phone clicks. 48 packets lift the budget to ~47 ms
    // for ~32 ms more uplink latency, hidden by the 300 ms playout buffer;
    // bufferRx() grows the same way and only ever dropped while full.
    config.fifo_packets = 48;
    config.rx_start_fill_percent = 0;
    // Host volume controls the phone-playback (USB OUT) path in audioTask.
    // Enabling this library option would instead scale USB IN microphone data.
    config.volume_active = false;
    config.self_powered = false;
    config.max_power_ma = 250;
    config.begin_usb = true;
    usb.setReqEntityCallback([](audio_tools::USBAudioDeviceBase* device,
                                uint8_t,
                                const audio_tools::UsbSetupPacket& request,
                                uint8_t*) {
        const uint8_t entity = audio_tools::u16High(request.wIndex);
        if (entity != audio_tools::USBAudio2DescriptorBuilder::ENTITY_FU1 ||
            request.bRequest != audio_tools::kAudioCsReqCur)
            return true;
        const uint8_t control = audio_tools::u16High(request.wValue);
        const uint8_t channel = audio_tools::u16Low(request.wValue);
        if (control == audio_tools::AUDIO_FU_CTRL_VOLUME)
            onUsbVolume(device->volume(channel), channel);
        else if (control == audio_tools::AUDIO_FU_CTRL_MUTE)
            onUsbMute(device->isMute(channel), channel);
        return true;
    });
    if (!usb.begin(config)) {
        statusError.store(true, std::memory_order_relaxed);
        Serial.println("USB audio initialization failed");
        return;
    }
    pinMode(kWifiResetPin, INPUT_PULLUP);
    WiFiManager wifiManager;
    wifiManager.setDebugOutput(false);
    wifiManager.setConnectTimeout(6);
    wifiManager.setConfigPortalTimeout(0);
    wifiManager.setShowInfoUpdate(false);
    wifiManager.setTitle("LAN MIC");
    wifiManager.setAPCallback([](WiFiManager*) {
        statusConfigPortalActive.store(true, std::memory_order_relaxed);
    });
    wifiManager.setWebServerCallback([&]() {
    wifiManager.server->on("/", [&]() {
        wifiManager.server->sendHeader("Location", "/wifi", true);
        wifiManager.server->send(302, "text/plain", "");
      });
    });
    Serial.printf("Open Wi-Fi provisioning AP: %s\n", provisioningSsid);
    if (!wifiManager.autoConnect(provisioningSsid)) {
        statusError.store(true, std::memory_order_relaxed);
        Serial.println("Wi-Fi provisioning failed to start; rebooting");
        delay(1000);
        ESP.restart();
    }
    statusConfigPortalActive.store(false, std::memory_order_relaxed);
    refreshWifiSnapshot();
    // Wi-Fi modem sleep keeps the IDF default here; audioTask widens it to
    // WIFI_PS_NONE only while USB audio is streaming (see kWifiIdleSleepMs).
    WiFi.setAutoReconnect(true);
    const auto audioResult = xTaskCreatePinnedToCore(
        audioTask, "sos-audio", 8192, nullptr, 4, &audioTaskHandle, 0);
    const auto netResult = xTaskCreatePinnedToCore(
        networkTask, "sos-network", 16384, nullptr, 3, &networkTaskHandle, 1);
    if (audioResult != pdPASS || netResult != pdPASS) {
        statusError.store(true, std::memory_order_relaxed);
        Serial.println("Audio task creation failed; restart after checking free heap");
    }
}

void loop() {
    static uint32_t wifiResetPressedMs = 0;
    static bool wifiResetArmed = false;
    delay(25);

    const uint32_t now = millis();
    static uint32_t lastWifiSnapshotMs = 0;
    if (uint32_t(now - lastWifiSnapshotMs) >= kWifiSnapshotIntervalMs) {
        lastWifiSnapshotMs = now;
        refreshWifiSnapshot();
    }

    if (digitalRead(kWifiResetPin) == LOW) {
        if (!wifiResetPressedMs) wifiResetPressedMs = now;
        if (!wifiResetArmed &&
            uint32_t(now - wifiResetPressedMs) >= kWifiResetHoldMs) {
            wifiResetArmed = true;
            statusResetArmed.store(true, std::memory_order_relaxed);
            Serial.println("Wi-Fi reset armed; release BOOT to restart provisioning");
        }
    } else {
        wifiResetPressedMs = 0;
        if (wifiResetArmed) {
            Serial.println("Clearing saved Wi-Fi credentials");
            wifiResetInProgress.store(true, std::memory_order_release);
            WiFi.setAutoReconnect(false);
            delay(100);
            WiFi.disconnect(false, false, 1000);
            delay(100);
            const bool erased = WiFi.eraseAP();
            WiFi.mode(WIFI_OFF);
            Serial.println(erased
                ? "Saved Wi-Fi credentials cleared"
                : "Failed to clear saved Wi-Fi credentials");
            delay(250);
            ESP.restart();
        }
    }

    // One aggregated name=value stats line per second (see printStatsLine).
    static uint32_t lastStatsMs = 0;
    if (uint32_t(now - lastStatsMs) >= 1000) {
        lastStatsMs = now;
        printStatsLine();
    }
}
