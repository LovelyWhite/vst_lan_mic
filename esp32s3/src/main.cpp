#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <esp_arduino_version.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
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

namespace {
constexpr uint8_t kWifiResetPin = 0;
constexpr uint32_t kWifiResetHoldMs = 3000;
// How long the USB bridge must stay quiet before Wi-Fi modem sleep is
// re-enabled; holding the radio awake costs real current while idle.
constexpr uint32_t kWifiIdleSleepMs = 5000;
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
char identity[64]{}, usbSerial[24]{};
uint32_t bootSession = 0;

void setStatusLed(bool on) {
    digitalWrite(kStatusLedPin, on == kStatusLedActiveHigh ? HIGH : LOW);
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
        } else if (WiFi.status() != WL_CONNECTED) {
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

void audioTask(void*) {
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
            }
        } else if (!wifiModemSleeping &&
                   uint32_t(millis() - lastStreamActiveMs) >= kWifiIdleSleepMs) {
            WiFi.setSleep(true);
            wifiModemSleeping = true;
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
                    captured.push(&frame, 1);
                    frameUsed = 0;
                }
            }
        }
        // Serve USB at 1 kHz only while a direction is live; an idle bridge
        // slows to 200 Hz, adding at most 5 ms to start-of-stream detection,
        // which the 300 ms network playout buffer hides.
        vTaskDelay((txActive || rxActive) ? 1 : pdMS_TO_TICKS(5));
    }
}

void networkTask(void*) {
    WiFiUDP control, audio;
    Peer sender, receiver;
    bool uplinkSequenceValid = false;
    uint32_t nextUplinkSequence = 0;
    uint32_t lastRetry = 0, lastValidAudio = 0;
    uint32_t nextDownlinkAttemptMs = 0;
    uint32_t downlinkBackoffMs = 10;
    bool socketsReady = false;
    CapturedFrame pendingDownlink;
    IPAddress boundIp;
    std::array<uint8_t, sos::kMaxPacketSize + 1> packet{};
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
        opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
    };

    auto invalidateSender = [&] {
        sender = Peer{};
        resetUplink();
        ++uplinkEpoch;
    };
    for (;;) {
        if (wifiResetInProgress.load(std::memory_order_acquire)) {
            if (socketsReady) {
                control.stop();
                audio.stop();
                socketsReady = false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const uint32_t now = millis();
        if (WiFi.status() != WL_CONNECTED ||
            (socketsReady && boundIp != WiFi.localIP())) {
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
            const bool controlReady = control.begin(sos::kDiscoveryPort);
            const bool audioReady = audio.begin(sos::kUplinkPort);
            socketsReady = controlReady && audioReady;
            if (!socketsReady) {
                control.stop(); audio.stop();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            boundIp = WiFi.localIP();
            Serial.printf("Wi-Fi ready: %s, device %s\n", boundIp.toString().c_str(), identity);
        }

        if (sender.port && !sender.active(now)) invalidateSender();
        if (receiver.port && !receiver.active(now)) receiver = Peer{};

        if (socketsReady) {
            for (int n = 0; n < 8; ++n) {
                const int length = control.parsePacket();
                if (!length) break;
                const IPAddress ip = control.remoteIP();
                const uint16_t port = control.remotePort();
                const int got = control.read(packet.data(), packet.size());
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
                if (control.beginPacket(ip, port)) {
                    control.write(packet.data(), bytes);
                    control.endPacket();
                }
            }
            for (int n = 0; n < 16; ++n) {
                const int length = audio.parsePacket();
                if (!length) break;
                const IPAddress ip = audio.remoteIP();
                const uint16_t port = audio.remotePort();
                const int got = audio.read(packet.data(), packet.size());
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
                if (gap > kMaxPlcFrames)
                    opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
                bool decodeOk = true;
                const uint32_t plcFrames = std::min(gap, kMaxPlcFrames);
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
        // congestion (endPacket fails) drop the frame -- retransmitting into
        // a congested queue amplifies the loss.
        const uint32_t attemptNow = millis();
        if (!socketsReady || !receiver.active(attemptNow)) {
            captured.clear();
            nextDownlinkAttemptMs = attemptNow;
            downlinkBackoffMs = 10;
        } else if (int32_t(attemptNow - nextDownlinkAttemptMs) >= 0) {
            if (captured.pop(pendingDownlink)) {
                const int encoded = opus_encode(
                    opusEncoder, pendingDownlink.pcm.data(), int(sos::kOpusFrameSamples),
                    packet.data() + sos::kHeaderSize, int(sos::kOpusMaxBytes));
                if (encoded <= 0) {
                    nextDownlinkAttemptMs = attemptNow + sos::kOpusFrameDurationMs;
                } else {
                    sos::Header h{sos::Kind::AudioOpus, 0, bootSession,
                                  pendingDownlink.sequence, pendingDownlink.capturedMs,
                                  uint16_t(encoded)};
                    const auto bytes = sos::encode(packet.data(), h);
                    const bool begun = audio.beginPacket(receiver.ip, receiver.port);
                    if (begun) audio.write(packet.data(), bytes);
                    if (begun && audio.endPacket()) {
                        downlinkBackoffMs = 10;
                        nextDownlinkAttemptMs = attemptNow + 4;
                    } else {
                        nextDownlinkAttemptMs = attemptNow + downlinkBackoffMs;
                        downlinkBackoffMs =
                            std::min(downlinkBackoffMs * 2, uint32_t(100));
                    }
                }
            }
        }
        // With a subscriber the socket poll stays at 1 kHz. With no sockets
        // there is nothing to parse and reconnect is timer-driven, so 20 Hz
        // is plenty; otherwise keep 5 ms discovery responsiveness without
        // spinning Core 0 against Wi-Fi/lwIP at 1 kHz.
        vTaskDelay((sender.port != 0 || receiver.port != 0) ? 1 :
            socketsReady ? pdMS_TO_TICKS(5) : pdMS_TO_TICKS(50));
    }
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
    // Wi-Fi modem sleep keeps the IDF default here; audioTask widens it to
    // WIFI_PS_NONE only while USB audio is streaming (see kWifiIdleSleepMs).
    WiFi.setAutoReconnect(true);
    const auto audioResult = xTaskCreatePinnedToCore(audioTask, "sos-audio", 8192, nullptr, 4, nullptr, 1);
    const auto netResult = xTaskCreatePinnedToCore(networkTask, "sos-network", 16384, nullptr, 2, nullptr, 0);
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
}
