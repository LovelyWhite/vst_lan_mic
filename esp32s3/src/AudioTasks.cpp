#include "AudioTasks.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <opus.h>
#include <algorithm>
#include <array>

#include "Log.h"
#include "Pacer.h"
#include "Stats.h"
#include "Status.h"
#include "UsbAudio.h"

namespace lanmic {
namespace {

// How long the USB bridge must stay quiet before Wi-Fi modem sleep is
// re-enabled; holding the radio awake costs real current while idle.
constexpr uint32_t kWifiIdleSleepMs = 5000;
// ISO-IN transfers complete once per millisecond while the host keeps the
// streaming alt setting open, so a 100 ms silence in the ISR-side completion
// counter means the endpoint is closed (or the cable pulled; mounted() covers
// that instantly).
constexpr uint32_t kUsbTxActiveTimeoutMs = 100;
constexpr uint32_t kMaxCapturedBacklogFrames = 5;

} // namespace

AudioQueues* audioQueues = nullptr; // PSRAM, initialized before any audio task starts.
// Encoder task is the only producer, networkTask is the only consumer.
sos::SpscRing<EncodedFrame, 16> encodedDownlink;
std::atomic<uint32_t> uplinkEpoch{0}, lastUplinkMs{0};
std::atomic<uint32_t> downlinkEpoch{0}, downlinkReceiverActive{0};
TaskHandle_t audioTaskHandle = nullptr, encoderTaskHandle = nullptr, networkTaskHandle = nullptr; // For the 1 Hz stack high-water query

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
        const int64_t audioLoopStartUs = esp_timer_get_time(); // Pass start, for work-time stats only.
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
                    // True stereo uplink: pass the DAW's L/R through to USB IN
                    // instead of duplicating one channel.
                    float stereo[2]{0.0f, 0.0f};
                    if (fresh) microphone.next(stereo);
                    micSamples[2 * i] = sos::pcm16(stereo[0]);
                    micSamples[2 * i + 1] = sos::pcm16(stereo[1]);
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
                        // Don't buffer raw PCM with no receiver, so recovery never sends stale audio.
                    } else if (!captured.push(&frame, 1)) {
                        ++stats.queuePushFail;
                        ++stats.rawPushFail;
                    } else if (encoderTaskHandle) {
                        // Event-driven downlink: enqueue wakes encoderTask, which
                        // wakes networkTask to send after encoding, keeping Opus
                        // encode out of the network task.
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

} // namespace lanmic
