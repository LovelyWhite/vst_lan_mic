#include "AudioTasks.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <opus.h>
#include <algorithm>
#include <array>

#include "Pacer.h"
#include "Serial.h"
#include "Status.h"
#include "UsbAudio.h"

namespace lanmic {
namespace {

// How long the USB bridge must stay quiet before Wi-Fi modem sleep is
// re-enabled; holding the radio awake costs real current while idle.
constexpr uint32_t kWifiIdleSleepMs = 5000;
constexpr uint32_t kMaxCapturedBacklogFrames = 5;

} // namespace

AudioQueues* audioQueues = nullptr; // PSRAM, initialized before any audio task starts.
// Encoder task is the only producer, networkTask is the only consumer.
sos::SpscRing<EncodedFrame, 16> encodedDownlink;
sos::SpscRing<CapturedFrame, 8> captured;
std::atomic<uint32_t> uplinkEpoch{0}, lastUplinkMs{0};
std::atomic<uint32_t> downlinkEpoch{0}, downlinkReceiverActive{0};
TaskHandle_t encoderTaskHandle = nullptr, networkTaskHandle = nullptr;

void audioTask(void*) {
    auto& microphone = audioQueues->microphone;
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
    for (;;) {
        const bool txActive = usb.mounted() && usb.isStreamingActiveTx();
        const bool rxActive = usb.mounted() && usb.isStreamingActiveRx();
        statusAudioStreaming.store(txActive || rxActive, std::memory_order_relaxed);
        // Modem sleep trades a little Wi-Fi latency for a large idle power
        // saving, so it only returns after the USB bridge has been quiet.
        if (txActive || rxActive) {
            lastStreamActiveMs = millis();
            if (wifiModemSleeping) {
                wifiModemSleeping = false;
                WiFi.setSleep(false);
                SERIAL_PRINTF("[D] WiFi active\n");
            }
        } else if (!wifiModemSleeping &&
                   uint32_t(millis() - lastStreamActiveMs) >= kWifiIdleSleepMs) {
            WiFi.setSleep(true);
            wifiModemSleeping = true;
            SERIAL_PRINTF("[D] WiFi sleep\n");
        }
        const auto newEpoch = uplinkEpoch.load(std::memory_order_acquire);
        const bool fresh = uint32_t(millis() - lastUplinkMs.load(std::memory_order_acquire)) <
                           sos::kAudioTimeoutMs;
        if (newEpoch != epoch || txActive != txWasActive || (uplinkWasFresh && !fresh)) {
            microphone.reset();
            epoch = newEpoch;
        }
        if (txActive != txWasActive)
            SERIAL_PRINTF("[D] USB tx %s\n", txActive ? "active" : "idle");
        txWasActive = txActive;
        uplinkWasFresh = fresh;

        // Keep the 48-packet FIFO nearly full to absorb ~47 ms scheduling stalls.
        // Feed bufferTx() directly: write() can block or silently discard data.
        // This task is the only writer; the ISR only drains, so checked free
        // space cannot shrink before writing.
        if (txActive) {
            microphone.prepare();
            for (int batch = 0; batch < 8 &&
                 usb.availableForWrite() >= int(sizeof(micSamples)); ++batch) {
                if (fresh) {
                    // True stereo uplink: pass the DAW's L/R through to USB IN
                    // instead of duplicating one channel.
                    for (size_t i = 0; i < 48; ++i) {
                        float stereo[2];
                        microphone.next(stereo);
                        micSamples[2 * i] = sos::pcm16(stereo[0]);
                        micSamples[2 * i + 1] = sos::pcm16(stereo[1]);
                    }
                } else {
                    // Same all-zero frame the per-sample path produced, without
                    // 96 float->PCM16 conversions per batch.
                    micSamples.fill(0);
                }
                usb.bufferTx().writeArray(
                    reinterpret_cast<uint8_t*>(micSamples.data()),
                    int(sizeof(micSamples)));
            }
        } else {
            microphone.reset();
        }

        if (rxActive != rxWasActive) {
            SERIAL_PRINTF("[D] USB rx %s\n", rxActive ? "active" : "idle");
            frameUsed = 0;
            // Only this task consumes bufferRx(). The cap is required: while the
            // host keeps streaming, the ISR refills faster than this drains, so an
            // unbounded loop could hold the 1 ms task off its grid indefinitely.
            // shortcut: drops <= 8*192 B; an older backlog plays through next tick.
            for (int batch = 0; batch < 8 && usb.available() > 0; ++batch)
                usb.readBytes(speakerBytes.data(), speakerBytes.size());
        }
        rxWasActive = rxActive;
        for (int batch = 0; rxActive && batch < 8 && usb.available() >= 4; ++batch) {
            const size_t bytes = std::min(size_t(usb.available()), speakerBytes.size()) & ~size_t(3);
            const size_t got = usb.readBytes(speakerBytes.data(), bytes);
            for (size_t i = 0; i + 3 < got; i += 4) {
                frame.pcm[frameUsed++] = int16_t(sos::get16(speakerBytes.data() + i));
                frame.pcm[frameUsed++] = int16_t(sos::get16(speakerBytes.data() + i + 2));
                if (frameUsed == frame.pcm.size()) {
                    frame.capturedMs = millis();
                    frame.sequence = captureSequence++;
                    if (!downlinkReceiverActive.load(std::memory_order_acquire)) {
                        // Don't buffer raw PCM with no receiver, so recovery never sends stale audio.
                    } else if (captured.push(&frame, 1) && encoderTaskHandle) {
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
        pacer.wait();
    }
}

void encoderTask(void*) {
    // Keep the large frame buffers off the task stack; Opus encode itself
    // still needs stack headroom for its internal scratch work.
    static CapturedFrame input;
    static EncodedFrame output;
    uint32_t epoch = downlinkEpoch.load(std::memory_order_acquire);

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
        SERIAL_PRINTF("[D] Opus encoder initialization failed: %d\n", opusStatus);
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
        }

        const uint32_t backlog = captured.size();
        if (backlog > kMaxCapturedBacklogFrames) {
            captured.discard(backlog - kMaxCapturedBacklogFrames);
        }

        if (!captured.pop(input)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        const int encoded = opus_encode(
            opusEncoder, input.pcm.data(), int(sos::kOpusFrameSamples),
            output.payload.data(), int(output.payload.size()));
        if (encoded <= 0) continue;

        output.capturedMs = input.capturedMs;
        output.sequence = input.sequence;
        output.payloadBytes = uint16_t(encoded);
        encodedDownlink.push(&output, 1);
        if (networkTaskHandle) xTaskNotifyGive(networkTaskHandle);
    }
}

} // namespace lanmic
