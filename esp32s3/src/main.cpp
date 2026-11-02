#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_arduino_version.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp32-hal-tinyusb.h>
#include <freertos/idf_additions.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
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

#if CONFIG_IDF_TARGET_ESP32S3 && ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(3, 1, 1)
#include <soc/usb_reg.h>
#include <soc/usb_struct.h>
#endif

// ---- Output control macros -------------------------------------------------
// Define LANMIC_ENABLE_SERIAL to enable all Serial output.
// Undefine or comment out for silent builds (e.g. field deployments).
// #define LANMIC_ENABLE_SERIAL

#ifdef LANMIC_ENABLE_SERIAL
  #define S_INIT()               Serial.begin(115200)
  #define S_PRINTF(...)          Serial.printf(__VA_ARGS__)
#else
  #define S_INIT()               ((void)0)
  #define S_PRINTF(...)          ((void)0)
#endif

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
constexpr int kMaxControlPacketsPerNetLoop = 2;
constexpr int kMaxAudioPacketsPerNetLoop = 1;
constexpr int kMaxDownlinkFramesPerNetLoop = 1;
constexpr uint32_t kMaxCapturedBacklogFrames = 5;
constexpr uint32_t kMaxEncodedBacklogFrames = 8;
constexpr uint8_t kStatusLedPin = 18;
constexpr bool kStatusLedActiveHigh = true;
// Opus PLC synthesizes at most 300 ms of concealment; longer gaps reset the
// decoder instead of padding with noise.
constexpr uint32_t kMaxPlcFrames = 300 / sos::kOpusFrameDurationMs;

constexpr uint32_t plcFramesForGap(uint32_t gap) {
    return gap <= kMaxPlcFrames ? gap : 0;
}
static_assert(plcFramesForGap(0) == 0 && plcFramesForGap(1) == 1,
              "Only missing frames need PLC");
static_assert(plcFramesForGap(kMaxPlcFrames) == kMaxPlcFrames &&
              plcFramesForGap(kMaxPlcFrames + 1) == 0 &&
              plcFramesForGap(0x7fffffffu) == 0,
              "Long gaps must reset without a PLC burst");

#if CONFIG_IDF_TARGET_ESP32S3 && ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(3, 1, 1)
class UsbAudioBackend : public audio_tools::USBAudioBackendTinyUSB {
    bool isoActivateEndpoint(
        uint8_t rhport, const audio_tools::UsbEndpointDescriptorView& ep) override {
        const bool isIn = ep.direction() == audio_tools::UsbDir::In;
        const uint8_t number = ep.bEndpointAddress & 0x0f;
        if (isIn && (rhport != 0 || number == 0 || number > 4)) return false;
        if (!USBAudioBackendTinyUSB::isoActivateEndpoint(rhport, ep)) return false;
        if (isIn) {
            // shortcut: Arduino 3.1.1 increments TXFNUM on each activation; review when upgrading its DCD.
            // dfifo_alloc() and handle_epin_slave() instead address the FIFO by endpoint number.
            auto& ctl = USB0.in_ep_reg[number].diepctl;
            ctl = (ctl & ~USB_D_TXFNUM1_M) | (uint32_t(number) << USB_D_TXFNUM1_S);
            assert(((ctl & USB_D_TXFNUM1_M) >> USB_D_TXFNUM1_S) == number);
            assert((USB0.dieptxf[number - 1] >> 16) * 4 >= ep.packetSize());
        }
        return true;
    }
};

class UsbAudioStream : public audio_tools::USBAudioStream {
    UsbAudioBackend backend_;
    audio_tools::USBAudioBackend& backend() override { return backend_; }
};
UsbAudioStream usb;
#else
audio_tools::USBAudioStream usb;
#endif
constexpr uint32_t kUsbUnityGainQ16 = 65536;
std::atomic<uint32_t> usbSpeakerLeftGainQ16{kUsbUnityGainQ16};
std::atomic<uint32_t> usbSpeakerRightGainQ16{kUsbUnityGainQ16};
std::atomic<uint32_t> usbSpeakerLeftMuted{0};
std::atomic<uint32_t> usbSpeakerRightMuted{0};
struct CapturedFrame {
    alignas(4) sos::OpusFrame pcm{};
    uint32_t capturedMs = 0, sequence = 0;
};
struct EncodedFrame {
    std::array<uint8_t, sos::kOpusMaxBytes> payload{};
    uint32_t capturedMs = 0, sequence = 0;
    uint32_t encodedMs = 0; // Local timing only; not part of the wire header.
    uint16_t payloadBytes = 0;
};
struct AudioQueues {
    sos::MonoPlayout microphone; // Uplink (DAW -> phone) stays mono.
    sos::SpscRing<CapturedFrame, 8> captured;
};
AudioQueues* audioQueues = nullptr; // PSRAM, initialized before any audio task starts.
// Encoder task is the only producer, networkTask is the only consumer.
sos::SpscRing<EncodedFrame, 16> encodedDownlink;
std::atomic<uint32_t> uplinkEpoch{0}, lastUplinkMs{0};
std::atomic<uint32_t> downlinkEpoch{0}, downlinkReceiverActive{0};
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

// Hot tasks update atomics; loop() prints at 1 Hz. Counters accumulate since
// boot; window extrema are exchanged and reset by the reporter.
struct DebugStats {
    // wait(): overrun == anchor; missed deadlines re-anchor without catch-up.
    std::atomic<uint32_t> audioPacerOverrun{0}, audioPacerAnchor{0};
    // waitNotify(): overrun counts missed polling periods; anchor also counts
    // notifications, so anchor >= overrun. Normal deadline wakeups count neither.
    std::atomic<uint32_t> netPacerOverrun{0}, netPacerAnchor{0};
    // 单轮循环工作耗时最大值（微秒，1 秒窗口；不含 pacer/backoff 睡眠）。
    std::atomic<uint32_t> audioLoopMaxUs{0}, netLoopMaxUs{0};
    // networkTask 分段耗时最大值（微秒，1 秒窗口）。
    std::atomic<uint32_t> netStateMaxUs{0}, netPollMaxUs{0};
    std::atomic<uint32_t> netControlMaxUs{0}, netAudioRxMaxUs{0}, netDownlinkMaxUs{0};
    std::atomic<uint32_t> netEncodeMaxUs{0}, netSendMaxUs{0};
    // Network diagnostics use cumulative timing, unaffected by serial window resets.
    std::atomic<uint32_t> receiveWorkUs{0}, encodeWorkUs{0};
    std::atomic<uint32_t> rawQueued{0}; // Sampled by the queue's consumer.
    // 下行队列：push 失败=原始/编码队满丢下行帧；pop 失败=被入队通知
    // 唤醒时编码队列已空（帧已被上一轮消费的通知竞态，应恒接近 0）。
    std::atomic<uint32_t> queuePushFail{0}, queuePopFail{0};
    std::atomic<uint32_t> rawPushFail{0}, encodedPushFail{0};
    std::atomic<uint32_t> rawTrimFrames{0}, encodedTrimFrames{0};
    // PLC 补偿合成的帧数 / 上行 Opus 解码器复位次数（均自启动累计）。
    std::atomic<uint32_t> plcFrames{0}, uplinkResets{0};
    // UDP 发送失败次数（control 应答 + audio 下行两路，累计）。
    std::atomic<uint32_t> udpEndPacketFail{0};
    // Actual downlink sendto calls returning WouldBlock, not select snapshots.
    std::atomic<uint32_t> downlinkSendWouldBlock{0};
    // Device -> DAW pipeline: cumulative counts and window timing maxima.
    std::atomic<uint32_t> capturedFrames{0}, encodedFrames{0}, sentFrames{0};
    std::atomic<uint32_t> encodeErrors{0}, downlinkPollBlocked{0};
    std::atomic<int32_t> lastEncodeError{0};
    std::atomic<uint32_t> captureGapMaxMs{0}, encodeGapMaxMs{0}, sendGapMaxMs{0};
    std::atomic<uint32_t> encodeWaitMaxMs{0}, sendWaitMaxMs{0}, sendAgeMaxMs{0};
    // 最近一次 sendto hard error 的 errno（0=尚无）。
    std::atomic<int32_t> controlSendErrno{0}, downlinkSendErrno{0};
    // socket 重建原因细分（累计）。
    std::atomic<uint32_t> socketPollReset{0}, controlReadReset{0}, controlSendReset{0};
    std::atomic<uint32_t> audioReadReset{0}, downlinkSendReset{0};
    // WiFi modem sleep 状态切换次数（进/出省电各计一次，累计）。
    std::atomic<uint32_t> wifiSleepToggles{0};
    // USB TX FIFO 可写空间窗口值（字节；min=最满时，max=最空时）。
    std::atomic<int32_t> usbTxFifoMinFree{INT32_MAX};
    std::atomic<int32_t> usbTxFifoMaxFree{-1};
};
DebugStats stats; // 唯一统计块：热任务只写 relaxed 原子，loop 任务 1 Hz 读取。
TaskHandle_t audioTaskHandle = nullptr, encoderTaskHandle = nullptr, networkTaskHandle = nullptr; // 供 1 Hz 栈高水位查询
// Reserve internal RAM before heap fragmentation; Opus scratch is hot, PCM queues are not.
alignas(portBYTE_ALIGNMENT) StackType_t encoderTaskStack[49152 / sizeof(StackType_t)];
StaticTask_t encoderTaskControl;
static_assert(sizeof(encoderTaskStack) == 49152, "Keep the measured encoder stack budget");

// Publish a window max/min via CAS so a better sample landing between the
// reporter's read and reset can never be clobbered by a stale value.
template <typename T>
void statsMax(std::atomic<T>& cell, T value) {
    T current = cell.load(std::memory_order_relaxed);
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

// Derive tick duration from FreeRTOS config rather than assuming 1 kHz.
constexpr int64_t kTickUs = 1000000 / int64_t(configTICK_RATE_HZ);

// Tick-anchored pacing avoids adding loop work to every relative delay;
// overruns re-anchor rather than replaying every missed deadline.
struct Pacer {
  TickType_t periodTicks;
  TickType_t nextTick;
  // Counter semantics are documented in DebugStats.
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

  // Return true on a notification, re-anchoring to the wakeup tick.
  // Notifications coalesce: pending frames still need later notifications or polls.
  bool waitNotify() {
    TickType_t now = xTaskGetTickCount();
    const TickType_t diff = TickType_t(now - nextTick);
    if (diff < periodTicks) {
      // period-diff stays in (0, period]; nextTick-now can spin at zero
      // or underflow into a huge timeout, stalling the consumer.
      const TickType_t remaining = TickType_t(periodTicks - diff);
      if (ulTaskNotifyTake(pdTRUE, remaining) != 0) {
        // Read the tick after waking; a pre-wait timestamp would drift.
        ++anchor;
        nextTick = xTaskGetTickCount();
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
    auto& microphone = audioQueues->microphone;
    auto& captured = audioQueues->captured;
    Pacer pacer{1000};
    uint32_t epoch = uplinkEpoch.load();
    bool txWasActive = false, rxWasActive = false, uplinkWasFresh = false;
    // Stereo frames doubled the buffer; keeping it off the 8 KB task stack.
    static CapturedFrame frame;
    size_t frameUsed = 0;
    uint32_t captureSequence = 0;
    uint32_t lastCapturedMs = millis();
    uint32_t lastStreamActiveMs = 0;
    bool wifiModemSleeping = true; // IDF default while associated.
    std::array<uint8_t, 192> speakerBytes{};
    std::array<int16_t, 96> micSamples{};
    int32_t speakerLeftGainQ16 = int32_t(kUsbUnityGainQ16);
    int32_t speakerRightGainQ16 = int32_t(kUsbUnityGainQ16);
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
        // isStreamingActiveTx() can stay false on ESP32-S3 despite live ISO-IN.
        // Use the ISR completion counter, which advances once per millisecond.
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

        // Keep the 48-packet FIFO nearly full to absorb ~47 ms scheduling stalls.
        // Feed bufferTx() directly: write() can block or silently discard data
        // because of the unreliable TX-active flag. This task is the only writer;
        // the ISR only drains, so checked free space cannot shrink before writing.
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
            lastCapturedMs = millis();
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
                    statsMax(stats.captureGapMaxMs, uint32_t(frame.capturedMs - lastCapturedMs));
                    lastCapturedMs = frame.capturedMs;
                    ++stats.capturedFrames;
                    frame.sequence = captureSequence++;
                    if (!downlinkReceiverActive.load(std::memory_order_acquire)) {
                        // 没有手机接收端时不积压原始 PCM，避免恢复后发送旧音频。
                    } else if (!captured.push(&frame, 1)) {
                        ++stats.queuePushFail;
                        ++stats.rawPushFail;
                    } else if (encoderTaskHandle) {
                        // 事件驱动下行：入队即唤醒 encoderTask，编码完成后
                        // 再唤醒 networkTask 发送，避免网络任务内做 Opus encode。
                        xTaskNotifyGive(encoderTaskHandle);
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

void encoderTask(void*) {
    auto& captured = audioQueues->captured;
    // Keep the large frame buffers off the task stack; Opus encode itself
    // still needs stack headroom for its internal scratch work.
    static CapturedFrame input;
    static EncodedFrame output;
    uint32_t epoch = downlinkEpoch.load(std::memory_order_acquire);
    uint32_t lastEncodedMs = millis();

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
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(0));
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    if (opusStatus != OPUS_OK) {
        S_PRINTF("Opus encoder initialization failed: %d\n", opusStatus);
        if (opusMemory) heap_caps_free(opusMemory);
        statusError.store(true, std::memory_order_relaxed);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (;;) {
        const uint32_t nextEpoch = downlinkEpoch.load(std::memory_order_acquire);
        if (nextEpoch != epoch ||
            !downlinkReceiverActive.load(std::memory_order_acquire)) {
            epoch = nextEpoch;
            captured.clear();
            opus_encoder_ctl(opusEncoder, OPUS_RESET_STATE);
            lastEncodedMs = millis();
        }

        const uint32_t backlog = captured.size();
        stats.rawQueued.store(backlog, std::memory_order_relaxed);
        if (backlog > kMaxCapturedBacklogFrames) {
            const uint32_t trim = backlog - kMaxCapturedBacklogFrames;
            captured.discard(trim);
            stats.rawTrimFrames.fetch_add(trim, std::memory_order_relaxed);
        }

        if (!captured.pop(input)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        statsMax(stats.encodeWaitMaxMs, uint32_t(millis() - input.capturedMs));
        const int64_t encodeStartUs = esp_timer_get_time();
        const int encoded = opus_encode(
            opusEncoder, input.pcm.data(), int(sos::kOpusFrameSamples),
            output.payload.data(), int(output.payload.size()));
        const uint32_t encodeUs = uint32_t(esp_timer_get_time() - encodeStartUs);
        statsMax(stats.netEncodeMaxUs, encodeUs);
        stats.encodeWorkUs.fetch_add(encodeUs, std::memory_order_relaxed);
        if (encoded <= 0) {
            ++stats.encodeErrors;
            stats.lastEncodeError.store(encoded, std::memory_order_relaxed);
            continue;
        }

        output.encodedMs = millis();
        statsMax(stats.encodeGapMaxMs, uint32_t(output.encodedMs - lastEncodedMs));
        lastEncodedMs = output.encodedMs;
        ++stats.encodedFrames;
        output.capturedMs = input.capturedMs;
        output.sequence = input.sequence;
        output.payloadBytes = uint16_t(encoded);
        if (!encodedDownlink.push(&output, 1)) {
            ++stats.queuePushFail;
            ++stats.encodedPushFail;
        }
        if (networkTaskHandle) xTaskNotifyGive(networkTaskHandle);
    }
}

void networkTask(void*) {
    auto& microphone = audioQueues->microphone;
    // Socket 轮询节拍（发现保活 + 上行接收 + 租约检查）。下行发送不依赖
    // 该节拍：由 encoderTask 的入队通知驱动（见 waitNotify）。无 socket 时
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
    uint32_t lastSentMs = millis();
    uint32_t lastDiagnosticsMs = 0, diagnosticsSequence = 0;
    bool socketsReady = false;
    bool notified = false; // 上一轮 waitNotify 是被入队通知唤醒的（供消费块区分竞态空队列）
    EncodedFrame pendingDownlink;
    IPAddress boundIp, currentIp;
    std::array<uint8_t, sos::kMaxPacketSize + 1> packet{};
    NonBlockingUdp::Ready controlReady, audioReady;

    // Uplink decoder mirrors the plugin's mono 20 ms encoder profile; like the
    // encoder it stays in internal RAM.
    alignas(4) static sos::Frame decoded{};
    const int opusDecoderBytes = opus_decoder_get_size(sos::kUplinkChannels);
    void* opusDecoderMemory = opusDecoderBytes > 0
        ? heap_caps_malloc(size_t(opusDecoderBytes), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : nullptr;
    auto* opusDecoder = static_cast<OpusDecoder*>(opusDecoderMemory);
    int opusDecoderStatus = opusDecoder
        ? opus_decoder_init(opusDecoder, sos::kSampleRate, sos::kUplinkChannels)
        : OPUS_ALLOC_FAIL;
    if (opusDecoderStatus != OPUS_OK) {
        S_PRINTF("Opus decoder initialization failed: %d\n", opusDecoderStatus);
        if (opusDecoderMemory) heap_caps_free(opusDecoderMemory);
        statusError.store(true, std::memory_order_relaxed);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    auto decodeIntoFrame = [&](const uint8_t* payload, int payloadBytes) {
        return opus_decode(opusDecoder, payload, payloadBytes, decoded.data(),
                           int(sos::kOpusFrameSamples), 0);
    };
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
    auto invalidateReceiver = [&] {
        receiver = Peer{};
        downlinkReceiverActive.store(0, std::memory_order_release);
        ++downlinkEpoch;
        encodedDownlink.clear();
        if (encoderTaskHandle) xTaskNotifyGive(encoderTaskHandle);
    };
    auto resetSockets = [&] {
        control.stop();
        audio.stop();
        socketsReady = false;
        invalidateSender();
        receiver = Peer{};
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
        int64_t netPhaseStartUs = esp_timer_get_time();
        const bool wifiConnected =
            wifiSnapshotConnected.load(std::memory_order_acquire) != 0;
        currentIp = IPAddress(wifiSnapshotIpRaw.load(std::memory_order_acquire));
        if (!wifiConnected || (socketsReady && boundIp != currentIp)) {
            if (socketsReady) resetSockets();
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
            S_PRINTF("Wi-Fi ready: %s, device %s\n", boundIp.toString().c_str(), identity);
        }

        if (sender.port && !sender.active(now)) invalidateSender();
        if (receiver.port && !receiver.active(now)) invalidateReceiver();
        statsMax(stats.netStateMaxUs,
                 uint32_t(esp_timer_get_time() - netPhaseStartUs));

        if (socketsReady) {
            netPhaseStartUs = esp_timer_get_time();
            if (!NonBlockingUdp::poll(control, controlReady, audio, audioReady)) {
                ++stats.socketPollReset;
                resetSockets();
            }
            statsMax(stats.netPollMaxUs,
                     uint32_t(esp_timer_get_time() - netPhaseStartUs));
        }

        if (socketsReady) {
            netPhaseStartUs = esp_timer_get_time();
            for (int n = 0; n < kMaxControlPacketsPerNetLoop; ++n) {
                const int got = control.read(controlReady, packet.data(), packet.size(), received);
                if (got < 0) {
                    ++stats.controlReadReset;
                    resetSockets();
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
                            encodedDownlink.clear();
                            downlinkReceiverActive.store(1, std::memory_order_release);
                            ++downlinkEpoch;
                            if (encoderTaskHandle) xTaskNotifyGive(encoderTaskHandle);
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
                    ++stats.controlSendReset;
                    stats.controlSendErrno.store(control.lastError(),
                                                 std::memory_order_relaxed);
                    break;
                }
            }
            statsMax(stats.netControlMaxUs,
                     uint32_t(esp_timer_get_time() - netPhaseStartUs));
        }

        if (socketsReady) {
            netPhaseStartUs = esp_timer_get_time();
            for (int n = 0; n < kMaxAudioPacketsPerNetLoop; ++n) {
                const int got = audio.read(audioReady, packet.data(), packet.size(), received);
                if (got < 0) {
                    ++stats.audioReadReset;
                    resetSockets();
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
                    resetUplink();
                    // Only the audio consumer may clear and re-prime playout.
                    ++uplinkEpoch;
                }
                bool decodeOk = true;
                const uint32_t plcFrames = plcFramesForGap(gap);
                // shortcut: up to 15 PLC frames still run in one pass; split across loops if measured stalls persist.
                for (uint32_t i = 0; i < plcFrames; ++i) {
                    const int samples = decodeIntoFrame(nullptr, 0);
                    if (samples != int(sos::kOpusFrameSamples)) {
                        decodeOk = false;
                        break;
                    }
                    stats.plcFrames.fetch_add(1, std::memory_order_relaxed);
                    microphone.push(decoded);
                }
                const int samples = decodeOk
                    ? decodeIntoFrame(packet.data() + sos::kHeaderSize,
                                      h.payloadBytes)
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
            const uint32_t receiveUs = uint32_t(esp_timer_get_time() - netPhaseStartUs);
            statsMax(stats.netAudioRxMaxUs, receiveUs);
            stats.receiveWorkUs.fetch_add(receiveUs, std::memory_order_relaxed);
        }

        // Catch up at 4 ms per packet; WouldBlock retains the head for backoff.
        // Trim only excess backlog to bound latency without dropping every older frame.
        // Notifications wake bounded batches; 5 ms polling covers coalesced notifications.
        // queuePopFail counts only notified-but-empty races, not normal queue draining.
        netPhaseStartUs = esp_timer_get_time();
        const uint32_t attemptNow = millis();
        if (!socketsReady || !receiver.active(attemptNow)) {
            downlinkReceiverActive.store(0, std::memory_order_release);
            encodedDownlink.clear();
            ulTaskNotifyTake(pdTRUE, 0); // 丢帧后清掉残留通知，避免一次空唤醒
            nextDownlinkAttemptMs = attemptNow;
            downlinkBackoffMs = 10;
            lastSentMs = attemptNow;
        } else if (notified && encodedDownlink.size() == 0) {
            ++stats.queuePopFail;
        } else {
            const uint32_t encodedBacklog = encodedDownlink.size();
            if (encodedBacklog > kMaxEncodedBacklogFrames) {
                const uint32_t trim = encodedBacklog - kMaxEncodedBacklogFrames;
                encodedDownlink.discard(trim);
                stats.encodedTrimFrames.fetch_add(trim, std::memory_order_relaxed);
            }
            for (int n = 0; n < kMaxDownlinkFramesPerNetLoop; ++n) {
                const uint32_t sendNow = millis();
                if (int32_t(sendNow - nextDownlinkAttemptMs) < 0) break;
                if (!audioReady.write) {
                    if (encodedDownlink.size() != 0) ++stats.downlinkPollBlocked;
                    nextDownlinkAttemptMs = sendNow + downlinkBackoffMs;
                    downlinkBackoffMs =
                        std::min(downlinkBackoffMs * 2, uint32_t(100));
                    break;
                }
                if (!encodedDownlink.peek(pendingDownlink)) break; // 队列已排空
                sos::Header h{sos::Kind::AudioOpus, 0, bootSession,
                              pendingDownlink.sequence, pendingDownlink.capturedMs,
                              pendingDownlink.payloadBytes};
                const auto bytes = sos::encode(packet.data(), h);
                memcpy(packet.data() + sos::kHeaderSize,
                       pendingDownlink.payload.data(),
                       pendingDownlink.payloadBytes);
                const int64_t downlinkStepUs = esp_timer_get_time();
                const auto sent = audio.sendTo(audioReady, receiver.ip, receiver.port,
                                               packet.data(), bytes);
                statsMax(stats.netSendMaxUs,
                         uint32_t(esp_timer_get_time() - downlinkStepUs));
                if (sent == NonBlockingUdp::SendResult::Sent) {
                    const uint32_t sentMs = millis();
                    statsMax(stats.sendGapMaxMs, uint32_t(sentMs - lastSentMs));
                    statsMax(stats.sendWaitMaxMs, uint32_t(sentMs - pendingDownlink.encodedMs));
                    statsMax(stats.sendAgeMaxMs, uint32_t(sentMs - pendingDownlink.capturedMs));
                    lastSentMs = sentMs;
                    ++stats.sentFrames;
                    encodedDownlink.discard(1);
                    downlinkBackoffMs = 10;
                    nextDownlinkAttemptMs = sendNow + 4;
                } else {
                    if (sent == NonBlockingUdp::SendResult::Error) {
                        ++stats.udpEndPacketFail;
                        ++stats.downlinkSendReset;
                        stats.downlinkSendErrno.store(audio.lastError(),
                                                      std::memory_order_relaxed);
                        invalidateReceiver();
                    } else {
                        ++stats.downlinkSendWouldBlock;
                    }
                    nextDownlinkAttemptMs = sendNow + downlinkBackoffMs;
                    downlinkBackoffMs =
                        std::min(downlinkBackoffMs * 2, uint32_t(100));
                    break;
                }
            }
        }
        statsMax(stats.netDownlinkMaxUs,
                 uint32_t(esp_timer_get_time() - netPhaseStartUs));
        const uint32_t diagnosticsNow = millis();
        if (socketsReady && receiver.active(diagnosticsNow) &&
            uint32_t(diagnosticsNow - lastDiagnosticsMs) >= 1000) {
            lastDiagnosticsMs = diagnosticsNow;
            if (controlReady.write) {
                const sos::DeviceDiagnostics diagnostics{
                    stats.capturedFrames.load(std::memory_order_relaxed),
                    stats.encodedFrames.load(std::memory_order_relaxed),
                    stats.sentFrames.load(std::memory_order_relaxed),
                    stats.rawPushFail.load(std::memory_order_relaxed),
                    stats.encodedPushFail.load(std::memory_order_relaxed),
                    stats.rawTrimFrames.load(std::memory_order_relaxed),
                    stats.encodedTrimFrames.load(std::memory_order_relaxed),
                    stats.encodeErrors.load(std::memory_order_relaxed),
                    stats.downlinkSendWouldBlock.load(std::memory_order_relaxed),
                    stats.downlinkPollBlocked.load(std::memory_order_relaxed),
                    stats.plcFrames.load(std::memory_order_relaxed),
                    stats.receiveWorkUs.load(std::memory_order_relaxed),
                    stats.encodeWorkUs.load(std::memory_order_relaxed),
                    stats.rawQueued.load(std::memory_order_relaxed), encodedDownlink.size(),
                    wifiSnapshotRssi.load(std::memory_order_relaxed)};
                const sos::Header h{sos::Kind::Diagnostics, 0, bootSession,
                                    diagnosticsSequence++, diagnosticsNow,
                                    uint16_t(sos::kDiagnosticsBytes)};
                const auto bytes = sos::encode(packet.data(), h);
                sos::encodeDiagnostics(packet.data() + sos::kHeaderSize, diagnostics);
                // Best effort after audio: no retry, lease reset, or audio error accounting.
                control.sendTo(controlReady, receiver.ip, receiver.port, packet.data(), bytes);
            }
        }
        // Poll at 5 ms while bound, 50 ms otherwise; notifications can wake sooner.
        pacer.setPeriodUs(socketsReady ? 5000 : 50000);
        // Loop work time only (pacer/backoff sleeps excluded).
        statsMax(stats.netLoopMaxUs,
                 uint32_t(esp_timer_get_time() - netLoopStartUs));
        notified = pacer.waitNotify();
        stats.netPacerOverrun.store(pacer.overrun, std::memory_order_relaxed);
        stats.netPacerAnchor.store(pacer.anchor, std::memory_order_relaxed);
    }
}

// 1Hz stats run on low-priority loop; no per-frame prints. UART transmission
// at 115200 baud takes >100ms; low priority avoids blocking realtime tasks.
// Stack/heap sampled every 5s. Inline name=value fields, no header. Key units:
// t_ms: timestamp; *_ovr/*_anch/*_fail/*_frames/*_rst/*_tog: cumulative counts
// (n_ovr = missed polling cycles; n_anch includes notification re-anchors, differs from a_* metrics, see waitNotify);
// *_loop_max_us/n_*_us: microsecond max latencies; usb_tx_xfer: ISO-IN transfers in window;
// stats_window_ms: actual sample interval; usb_fifo_*: free bytes (-1=unsampled);
// rssi_dbm: Wi-Fi RSSI (-128=disconnected); *_stack_min_b: min remaining task stack bytes (0=task not created);
// health_age_ms: age of cached stack/heap samples in ms.
#ifdef LANMIC_ENABLE_SERIAL
void printStatsLine() {
    static uint32_t lastUsbTxCount = 0, lastStatsMs = 0;
    const uint32_t now = millis();
    const uint32_t statsWindowMs = uint32_t(now - lastStatsMs);
    lastStatsMs = now;
    const uint32_t capturedFrames = stats.capturedFrames.load(std::memory_order_relaxed);
    const uint32_t encodedFrames = stats.encodedFrames.load(std::memory_order_relaxed);
    const uint32_t sentFrames = stats.sentFrames.load(std::memory_order_relaxed);
    const uint32_t captureGapMs = stats.captureGapMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t encodeGapMs = stats.encodeGapMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t sendGapMs = stats.sendGapMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t encodeWaitMs = stats.encodeWaitMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t sendWaitMs = stats.sendWaitMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t sendAgeMs = stats.sendAgeMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t encodeErrors = stats.encodeErrors.load(std::memory_order_relaxed);
    const int32_t encodeError = stats.lastEncodeError.load(std::memory_order_relaxed);
    const uint32_t pollBlocked = stats.downlinkPollBlocked.load(std::memory_order_relaxed);
    const uint32_t receiverActive = downlinkReceiverActive.load(std::memory_order_acquire);
    const uint32_t receiverEpoch = downlinkEpoch.load(std::memory_order_acquire);
    // Sample USB counters together before stack scans can delay this low-priority task.
    const uint32_t usbTxCount = usb.getTxXferCount(); // ISR 侧累计完成数（volatile 字读取，跨任务安全）
    const uint32_t usbTxReadBytes = usb.getTxFifoReadTotal();
    const uint32_t usbRxCount = usb.getRxXferCount();
    const uint32_t usbRxBytes = usb.getRxTotalBytes();
    const uint32_t usbRxDroppedBytes = usb.getRxDroppedBytes();
    const uint32_t usbTxXferDelta = uint32_t(usbTxCount - lastUsbTxCount);
    lastUsbTxCount = usbTxCount;
    // Swap-out resets the window cells; a better sample landing between the
    // swap and a concurrent writer is lost for at most one window.
    const uint32_t audioLoopUs =
        stats.audioLoopMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netLoopUs =
        stats.netLoopMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netStateUs =
        stats.netStateMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netPollUs =
        stats.netPollMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netControlUs =
        stats.netControlMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netAudioRxUs =
        stats.netAudioRxMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netDownlinkUs =
        stats.netDownlinkMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netEncodeUs =
        stats.netEncodeMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netSendUs =
        stats.netSendMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t rawPushFail =
        stats.rawPushFail.load(std::memory_order_relaxed);
    const uint32_t encodedPushFail =
        stats.encodedPushFail.load(std::memory_order_relaxed);
    const uint32_t rawTrimFrames =
        stats.rawTrimFrames.load(std::memory_order_relaxed);
    const uint32_t encodedTrimFrames =
        stats.encodedTrimFrames.load(std::memory_order_relaxed);
    const uint32_t socketPollReset =
        stats.socketPollReset.load(std::memory_order_relaxed);
    const uint32_t controlReadReset =
        stats.controlReadReset.load(std::memory_order_relaxed);
    const uint32_t controlSendReset =
        stats.controlSendReset.load(std::memory_order_relaxed);
    const uint32_t audioReadReset =
        stats.audioReadReset.load(std::memory_order_relaxed);
    const uint32_t downlinkSendReset =
        stats.downlinkSendReset.load(std::memory_order_relaxed);
    const int32_t controlSendErrno =
        stats.controlSendErrno.load(std::memory_order_relaxed);
    const int32_t downlinkSendErrno =
        stats.downlinkSendErrno.load(std::memory_order_relaxed);
    const int32_t fifoMin =
        stats.usbTxFifoMinFree.exchange(INT32_MAX, std::memory_order_relaxed);
    const int32_t fifoMax =
        stats.usbTxFifoMaxFree.exchange(-1, std::memory_order_relaxed);
    const long rssi = wifiSnapshotRssi.load(std::memory_order_acquire);
    static bool healthSampled = false;
    static uint32_t lastHealthMs = 0;
    static uint32_t audioStackMin = 0, encoderStackMin = 0, netStackMin = 0;
    static uint32_t heapFree = 0, heapMinimum = 0, heapLargest = 0;
    if (!healthSampled || uint32_t(now - lastHealthMs) >= 5000) {
        // High-water marks are bytes on this core; 0 means no task handle.
        audioStackMin = audioTaskHandle
            ? uint32_t(uxTaskGetStackHighWaterMark(audioTaskHandle)) : 0u;
        encoderStackMin = encoderTaskHandle
            ? uint32_t(uxTaskGetStackHighWaterMark(encoderTaskHandle)) : 0u;
        netStackMin = networkTaskHandle
            ? uint32_t(uxTaskGetStackHighWaterMark(networkTaskHandle)) : 0u;
        heapFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        heapMinimum = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        heapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        lastHealthMs = now;
        healthSampled = true;
    }
    // Avoid Print::printf's temporary allocation and second formatting pass.
    static char line[2048];
    const int length = std::snprintf(line, sizeof(line),
        "t_ms=%lu,stats_window_ms=%lu,a_ovr=%u,a_anch=%u,n_ovr=%u,n_anch=%u,"
        "a_loop_max_us=%u,n_loop_max_us=%u,usb_tx_xfer=%u,"
        "n_state_us=%u,n_poll_us=%u,n_ctl_us=%u,n_rx_us=%u,n_dl_us=%u,"
        "n_enc_us=%u,n_send_us=%u,"
        "usb_fifo_min=%d,usb_fifo_max=%d,q_push_fail=%u,q_raw_fail=%u,"
        "q_enc_fail=%u,q_trim=%u,q_enc_trim=%u,q_pop_fail=%u,"
        "plc_frames=%u,ul_rst=%u,udp_send_fail=%u,dl_send_wouldblock=%u,"
        "ctl_send_errno=%ld,dl_send_errno=%ld,"
        "sock_poll_rst=%u,sock_ctl_read_rst=%u,sock_ctl_send_rst=%u,"
        "sock_rx_read_rst=%u,sock_dl_send_rst=%u,"
        "wifi_sleep_tog=%u,rssi_dbm=%ld,a_stack_min_b=%u,e_stack_min_b=%u,n_stack_min_b=%u,"
        "heap=%u,minHeap=%u,maxBlock=%u,health_age_ms=%u,"
        "usb_rx_xfer_total=%lu,usb_rx_bytes_total=%lu,usb_rx_drop_bytes=%lu,"
        "usb_tx_read_bytes_total=%lu,play_underrun=%lu,play_trim=%lu,play_overflow=%lu,"
        "dl_cap=%u,dl_enc=%u,dl_sent=%u,"
        "dl_cap_gap_ms=%u,dl_enc_gap_ms=%u,dl_send_gap_ms=%u,"
        "dl_enc_wait_ms=%u,dl_send_wait_ms=%u,dl_age_ms=%u,"
        "dl_enc_err=%u,dl_enc_code=%ld,dl_poll_blocked=%u,dl_active=%u,dl_epoch=%u\n",
        (unsigned long)now,
        (unsigned long)statsWindowMs,
        stats.audioPacerOverrun.load(std::memory_order_relaxed),
        stats.audioPacerAnchor.load(std::memory_order_relaxed),
        stats.netPacerOverrun.load(std::memory_order_relaxed),
        stats.netPacerAnchor.load(std::memory_order_relaxed),
        audioLoopUs, netLoopUs, usbTxXferDelta,
        netStateUs, netPollUs, netControlUs, netAudioRxUs, netDownlinkUs,
        netEncodeUs, netSendUs,
        fifoMin == INT32_MAX ? -1 : int(fifoMin),
        int(fifoMax),
        stats.queuePushFail.load(std::memory_order_relaxed),
        rawPushFail,
        encodedPushFail,
        rawTrimFrames,
        encodedTrimFrames,
        stats.queuePopFail.load(std::memory_order_relaxed),
        stats.plcFrames.load(std::memory_order_relaxed),
        stats.uplinkResets.load(std::memory_order_relaxed),
        stats.udpEndPacketFail.load(std::memory_order_relaxed),
        stats.downlinkSendWouldBlock.load(std::memory_order_relaxed),
        long(controlSendErrno),
        long(downlinkSendErrno),
        socketPollReset,
        controlReadReset,
        controlSendReset,
        audioReadReset,
        downlinkSendReset,
        stats.wifiSleepToggles.load(std::memory_order_relaxed),
        rssi, audioStackMin, encoderStackMin, netStackMin,
        unsigned(heapFree), unsigned(heapMinimum), unsigned(heapLargest),
        unsigned(uint32_t(now - lastHealthMs)),
        (unsigned long)usbRxCount,
        (unsigned long)usbRxBytes,
        (unsigned long)usbRxDroppedBytes,
        (unsigned long)usbTxReadBytes,
        (unsigned long)(audioQueues ? audioQueues->microphone.underruns.load(std::memory_order_relaxed) : 0),
        (unsigned long)(audioQueues ? audioQueues->microphone.trims.load(std::memory_order_relaxed) : 0),
        (unsigned long)(audioQueues ? audioQueues->microphone.overflows.load(std::memory_order_relaxed) : 0),
        capturedFrames, encodedFrames, sentFrames,
        captureGapMs, encodeGapMs, sendGapMs,
        encodeWaitMs, sendWaitMs, sendAgeMs,
        encodeErrors, long(encodeError), pollBlocked, receiverActive, receiverEpoch);
    if (length <= 0 || size_t(length) >= sizeof(line)) {
        Serial.println("stats_format_error=1");
        return;
    }
    Serial.write(reinterpret_cast<const uint8_t*>(line), size_t(length));
}
#endif
} // namespace

void setup() {
    S_INIT(); // UART bridge, not native USB CDC.
    pinMode(kStatusLedPin, OUTPUT);
    setStatusLed(false);
    const auto ledResult = xTaskCreatePinnedToCore(
        statusLedTask, "status-led", 2048, nullptr, 1, nullptr, 0);
    if (ledResult != pdPASS) {
        setStatusLed(true);
        S_PRINTF("Status LED task creation failed\n");
    }
    void* queueMemory = heap_caps_malloc(
        sizeof(AudioQueues), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!queueMemory) {
        statusError.store(true, std::memory_order_relaxed);
        S_PRINTF("Audio queues PSRAM allocation failed: need=%u,maxBlock=%u\n",
            unsigned(sizeof(AudioQueues)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        return;
    }
    assert(esp_ptr_external_ram(queueMemory));
    assert(reinterpret_cast<uintptr_t>(queueMemory) % alignof(AudioQueues) == 0);
    assert(esp_ptr_internal(encoderTaskStack));
    audioQueues = new (queueMemory) AudioQueues{};
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
    // 48 packets absorb stalls that exceeded the old 16-packet FIFO, avoiding
    // audible IN zero-padding; adds ~32 ms latency and also enlarges bufferRx().
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
        S_PRINTF("USB audio initialization failed\n");
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
    S_PRINTF("Open Wi-Fi provisioning AP: %s\n", provisioningSsid);
    if (!wifiManager.autoConnect(provisioningSsid)) {
        statusError.store(true, std::memory_order_relaxed);
        S_PRINTF("Wi-Fi provisioning failed to start; rebooting\n");
        delay(1000);
        ESP.restart();
    }
    statusConfigPortalActive.store(false, std::memory_order_relaxed);
    refreshWifiSnapshot();
    // Wi-Fi modem sleep keeps the IDF default here; audioTask widens it to
    // WIFI_PS_NONE only while USB audio is streaming (see kWifiIdleSleepMs).
    WiFi.setAutoReconnect(true);
    S_PRINTF("Before audio/network tasks: heap=%u,minHeap=%u,maxBlock=%u\n",
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    const auto audioResult = xTaskCreatePinnedToCore(
        audioTask, "sos-audio", 8192, nullptr, 4, &audioTaskHandle, 0);
    // shortcut: initial alloca budgets for 20 ms audio, remeasure high-water marks before changing codec settings.
    // 编码独占 Core 1：Wi-Fi 重传多时 lwIP 高优先级任务会挤占共享核心，
    // 拖慢编码导致 raw_trim 增长；networkTask 放 Core 0，被 audioTask(4)
    // 抢占是安全方向，双向缓冲吸收其调度抖动。
    const auto netResult = xTaskCreatePinnedToCore(
        networkTask, "sos-network", 32768, nullptr, 3, &networkTaskHandle, 0);
    encoderTaskHandle = xTaskCreateStaticPinnedToCore(
        encoderTask, "sos-encoder", sizeof(encoderTaskStack), nullptr, 3,
        encoderTaskStack, &encoderTaskControl, 1);
    const BaseType_t encoderResult = encoderTaskHandle ? pdPASS : errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    if (audioResult != pdPASS || encoderResult != pdPASS || netResult != pdPASS) {
        statusError.store(true, std::memory_order_relaxed);
        S_PRINTF("Audio/network task creation failed: audio=%ld,network=%ld,encoder=%ld,"
                 "heap=%u,minHeap=%u,maxBlock=%u,psramHeap=%u,psramMaxBlock=%u\n",
            long(audioResult), long(netResult), long(encoderResult),
            unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
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
            S_PRINTF("Wi-Fi reset armed; release BOOT to restart provisioning\n");
        }
    } else {
        wifiResetPressedMs = 0;
        if (wifiResetArmed) {
            S_PRINTF("Clearing saved Wi-Fi credentials\n");
            wifiResetInProgress.store(true, std::memory_order_release);
            WiFi.setAutoReconnect(false);
            delay(100);
            WiFi.disconnect(false, false, 1000);
            delay(100);
            const bool erased = WiFi.eraseAP();
            WiFi.mode(WIFI_OFF);
            S_PRINTF("%s\n", erased ? "Saved Wi-Fi credentials cleared" : "Failed to clear saved Wi-Fi credentials");
            delay(250);
            ESP.restart();
        }
    }

#ifdef LANMIC_ENABLE_SERIAL
    // Skip stack scans and counter exchanges entirely in silent builds.
    static uint32_t lastStatsMs = 0;
    if (uint32_t(now - lastStatsMs) >= 1000) {
        lastStatsMs = now;
        printStatsLine();
    }
#endif
}
