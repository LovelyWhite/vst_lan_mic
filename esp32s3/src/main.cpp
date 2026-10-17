#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_arduino_version.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp32-hal-tinyusb.h>
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

#if __has_include("WifiConfig.h")
#include "WifiConfig.h"
#endif
#ifndef SOS_WIFI_SSID
#define SOS_WIFI_SSID ""
#define SOS_WIFI_PASSWORD ""
#endif

namespace {
audio_tools::USBAudioStream usb;
sos::Playout microphone;
struct CapturedFrame {
    sos::OpusFrame pcm{};
    uint32_t capturedMs = 0, sequence = 0;
};
sos::SpscRing<CapturedFrame, 8> captured;
std::atomic<uint32_t> uplinkEpoch{0}, lastUplinkMs{0};
std::atomic<uint32_t> capturedDrops{0}, sentPackets{0}, receivedPackets{0};
// #region debug-point A-L:audio-path-metrics
std::atomic<uint32_t> debugMicMin{UINT32_MAX}, debugMicMax{0};
std::atomic<uint32_t> debugUsbTxMin{UINT32_MAX}, debugUsbTxMax{0};
std::atomic<uint32_t> debugUsbRxMin{UINT32_MAX}, debugUsbRxMax{0};
std::atomic<uint32_t> debugCapturePeak{0}, debugSendBatchMax{0}, debugFrameAgeMax{0};
std::atomic<uint32_t> debugUdpFailures{0}, debugUdpFailureBurst{0}, debugUdpFailureBurstMax{0};
std::atomic<uint32_t> debugUplinkLost{0}, debugUplinkGapMax{0}, debugStaleDrops{0}, debugSuperseded{0};
std::atomic<uint32_t> debugUplinkArrivalMax{0}, debugUplinkSourceMax{0};
std::atomic<uint32_t> debugNetworkStackMin{UINT32_MAX};
std::atomic<uint32_t> debugOpusErrors{0}, debugOpusBytesMin{UINT32_MAX}, debugOpusBytesMax{0};
// #endregion
char identity[64]{}, usbSerial[24]{};
uint32_t bootSession = 0;

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
    CapturedFrame frame;
    size_t frameUsed = 0;
    uint32_t captureSequence = 0;
    std::array<uint8_t, 192> speakerBytes{};
    std::array<int16_t, 96> micSamples{};
    for (;;) {
        const bool txActive = usb.mounted() && usb.isStreamingActiveTx();
        const bool rxActive = usb.mounted() && usb.isStreamingActiveRx();
        // #region debug-point B-C-E:buffer-levels
        const uint32_t micLevel = microphone.queue.size();
        const uint32_t usbTxLevel = uint32_t(usb.bufferTx().available());
        const uint32_t usbRxLevel = uint32_t(usb.bufferRx().available());
        debugMicMin.store(std::min(debugMicMin.load(), micLevel));
        debugMicMax.store(std::max(debugMicMax.load(), micLevel));
        debugUsbTxMin.store(std::min(debugUsbTxMin.load(), usbTxLevel));
        debugUsbTxMax.store(std::max(debugUsbTxMax.load(), usbTxLevel));
        debugUsbRxMin.store(std::min(debugUsbRxMin.load(), usbRxLevel));
        debugUsbRxMax.store(std::max(debugUsbRxMax.load(), usbRxLevel));
        debugCapturePeak.store(std::max(debugCapturePeak.load(), captured.size()));
        // #endregion
        const auto newEpoch = uplinkEpoch.load(std::memory_order_acquire);
        const bool fresh = uint32_t(millis() - lastUplinkMs.load()) < sos::kAudioTimeoutMs;
        if (newEpoch != epoch || txActive != txWasActive || (uplinkWasFresh && !fresh)) {
            microphone.reset();
            epoch = newEpoch;
        }
        txWasActive = txActive;
        uplinkWasFresh = fresh;

        // Keep 8 ms queued for USB scheduling jitter. The network playout
        // remains the main latency buffer.
        // Checking free space avoids Audio Tools' blocking write retry path.
        if (txActive) {
            microphone.prepare();
            for (int batch = 0; batch < 8 && usb.bufferTx().available() < 1536; ++batch) {
                if (usb.availableForWrite() < int(sizeof(micSamples))) break;
                for (size_t i = 0; i < 48; ++i) {
                    const int16_t mono = fresh ? sos::pcm16(microphone.next()) : 0;
                    micSamples[2 * i] = mono;
                    micSamples[2 * i + 1] = mono;
                }
                usb.write(reinterpret_cast<uint8_t*>(micSamples.data()), sizeof(micSamples));
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
            for (size_t i = 0; i + 3 < got; i += 4) {
                const uint16_t l = sos::get16(speakerBytes.data() + i);
                const uint16_t r = sos::get16(speakerBytes.data() + i + 2);
                const int32_t left = l < 32768 ? int32_t(l) : int32_t(l) - 65536;
                const int32_t right = r < 32768 ? int32_t(r) : int32_t(r) - 65536;
                frame.pcm[frameUsed++] = int16_t((left + right) / 2);
                if (frameUsed == sos::kOpusFrameSamples) {
                    frame.capturedMs = millis();
                    frame.sequence = captureSequence++;
                    if (!captured.push(&frame, 1)) ++capturedDrops;
                    frameUsed = 0;
                }
            }
        }
        vTaskDelay(1);
    }
}

void networkTask(void*) {
    WiFiUDP control, audio;
    Peer sender, receiver;
    sos::SequenceTracker sequence;
    uint32_t lastRetry = 0, lastValidAudio = 0;
    uint32_t lastUplinkArrival = 0, lastUplinkSource = 0;
    uint32_t nextDownlinkAttemptMs = 0;
    uint32_t downlinkBackoffMs = 10;
    bool socketsReady = false;
    CapturedFrame pendingDownlink;
    IPAddress boundIp;
    std::array<uint8_t, sos::kMaxPacketSize + 1> packet{};
    const int opusEncoderBytes = opus_encoder_get_size(1);
    void* opusMemory = opusEncoderBytes > 0
        ? heap_caps_malloc(size_t(opusEncoderBytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : nullptr;
    auto* opusEncoder = static_cast<OpusEncoder*>(opusMemory);
    int opusStatus = opusEncoder
        ? opus_encoder_init(opusEncoder, sos::kSampleRate, 1, OPUS_APPLICATION_AUDIO)
        : OPUS_ALLOC_FAIL;
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(sos::kOpusBitrate));
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_VBR(0));
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(5));
    if (opusStatus == OPUS_OK)
        opusStatus = opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    if (opusStatus != OPUS_OK) {
        Serial.printf("Opus encoder initialization failed: %d\n", opusStatus);
        if (opusMemory) heap_caps_free(opusMemory);
        vTaskDelete(nullptr);
        return;
    }

    auto invalidateSender = [&] {
        sender = Peer{};
        sequence.reset();
        lastUplinkArrival = lastUplinkSource = 0;
        ++uplinkEpoch;
    };
    for (;;) {
        // #region debug-point F:network-stack
        debugNetworkStackMin.store(std::min(debugNetworkStackMin.load(),
            uint32_t(uxTaskGetStackHighWaterMark(nullptr))));
        // #endregion
        const uint32_t now = millis();
        if (WiFi.status() != WL_CONNECTED ||
            (socketsReady && boundIp != WiFi.localIP())) {
            if (socketsReady) {
                control.stop(); audio.stop();
                socketsReady = false;
                invalidateSender();
                receiver = Peer{};
            }
            if (uint32_t(now - lastRetry) >= 5000 && strlen(SOS_WIFI_SSID) > 0) {
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
                            sequence.reset();
                            lastUplinkArrival = lastUplinkSource = 0;
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
                    h.kind != sos::Kind::Audio || !sender.active(now) ||
                    !sender.matches(ip, port, h.session)) continue;
                if (uint32_t(now - lastValidAudio) >= sos::kAudioTimeoutMs) sequence.reset();
                if (sequence.accept(h.sequence, sos::decodePcm(packet.data() + sos::kHeaderSize),
                                    [](const sos::Frame& f) { microphone.push(f); })) {
                    if (lastUplinkArrival != 0) {
                        debugUplinkArrivalMax.store(std::max(
                            debugUplinkArrivalMax.load(), uint32_t(now - lastUplinkArrival)));
                        debugUplinkSourceMax.store(std::max(
                            debugUplinkSourceMax.load(),
                            uint32_t(h.timestampMs - lastUplinkSource)));
                    }
                    lastUplinkArrival = now;
                    lastUplinkSource = h.timestampMs;
                    lastValidAudio = now;
                    lastUplinkMs.store(now, std::memory_order_release);
                    ++receivedPackets;
                    // #region debug-point B-C:uplink-gaps
                    debugUplinkLost.store(sequence.lost);
                    debugUplinkGapMax.store(sequence.maxGap);
                    // #endregion
                }
            }
        }

        // On congestion, back off and resume from the newest frame. Retrying an
        // old frame creates a send/log storm and can never catch real time.
        uint32_t sentThisLoop = 0;
        const uint32_t attemptNow = millis();
        if (!socketsReady || !receiver.active(attemptNow)) {
            captured.clear();
            nextDownlinkAttemptMs = attemptNow;
            downlinkBackoffMs = 10;
        } else if (int32_t(attemptNow - nextDownlinkAttemptMs) >= 0) {
            const uint32_t queued = captured.size();
            if (queued > 1) {
                captured.discard(queued - 1);
                debugSuperseded.fetch_add(queued - 1);
            }
            if (captured.pop(pendingDownlink)) {
                // #region debug-point D:network-burst
                const uint32_t frameAge = uint32_t(attemptNow - pendingDownlink.capturedMs);
                debugFrameAgeMax.store(std::max(debugFrameAgeMax.load(), frameAge));
                // #endregion
                if (frameAge > 300) {
                    ++debugStaleDrops;
                    nextDownlinkAttemptMs = attemptNow + 1;
                } else {
                    const int encoded = opus_encode(
                        opusEncoder, pendingDownlink.pcm.data(), int(sos::kOpusFrameSamples),
                        packet.data() + sos::kHeaderSize, int(sos::kOpusMaxBytes));
                    if (encoded <= 0) {
                        ++debugOpusErrors;
                        nextDownlinkAttemptMs = attemptNow + sos::kOpusFrameDurationMs;
                    } else {
                        debugOpusBytesMin.store(std::min(debugOpusBytesMin.load(), uint32_t(encoded)));
                        debugOpusBytesMax.store(std::max(debugOpusBytesMax.load(), uint32_t(encoded)));
                        ++sentThisLoop;
                        sos::Header h{sos::Kind::AudioOpus, 0, bootSession,
                                      pendingDownlink.sequence, pendingDownlink.capturedMs,
                                      uint16_t(encoded)};
                        const auto bytes = sos::encode(packet.data(), h);
                        const bool begun = audio.beginPacket(receiver.ip, receiver.port);
                        if (begun) audio.write(packet.data(), bytes);
                        // #region debug-point A:udp-send-result
                        if (begun && audio.endPacket()) {
                            ++sentPackets;
                            debugUdpFailureBurst.store(0);
                            downlinkBackoffMs = 10;
                            nextDownlinkAttemptMs = attemptNow + 4;
                        } else {
                            ++debugUdpFailures;
                            const uint32_t burst = debugUdpFailureBurst.fetch_add(1) + 1;
                            debugUdpFailureBurstMax.store(
                                std::max(debugUdpFailureBurstMax.load(), burst));
                            nextDownlinkAttemptMs = attemptNow + downlinkBackoffMs;
                            downlinkBackoffMs =
                                std::min(downlinkBackoffMs * 2, uint32_t(100));
                        }
                        // #endregion
                    }
                }
            }
        }
        // #region debug-point D:network-burst
        debugSendBatchMax.store(std::max(debugSendBatchMax.load(), sentThisLoop));
        // #endregion
        vTaskDelay(1);
    }
}
} // namespace

void setup() {
    Serial.begin(115200); // UART bridge, not native USB CDC.
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(usbSerial, sizeof(usbSerial), "%012llX", (unsigned long long)mac);
    snprintf(identity, sizeof(identity), "%s-%s", sos::kDeviceName, usbSerial);
    bootSession = esp_random();
    if (!bootSession) bootSession = 1;

    audio_tools::USBAudioConfig config;
    config.sample_rate = sos::kSampleRate;
    config.channels = 2;
    config.bits_per_sample = 16;
    config.product = sos::kDeviceName;
    config.manufacturer = "Studio One Stream";
    config.serial = usbSerial;
    config.enable_ep_in = config.enable_ep_out = true;
    config.enable_feedback_ep = false;
    config.enable_multi_sample_rate = false;
    // The descriptor reserves 49 samples, but flow control sends exactly
    // 48 stereo samples (192 bytes) per 1 ms frame at 48 kHz.
    config.enable_ep_in_flow_control = true;
    config.fifo_packets = 16;
    config.rx_start_fill_percent = 0;
    config.volume_active = false;
    config.self_powered = false;
    config.max_power_ma = 250;
    config.begin_usb = true;
    if (!usb.begin(config)) {
        Serial.println("USB audio initialization failed");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    if (strlen(SOS_WIFI_SSID) > 0) WiFi.begin(SOS_WIFI_SSID, SOS_WIFI_PASSWORD);
    else Serial.println("Configure include/WifiConfig.h before using network audio");
    const auto audioResult = xTaskCreatePinnedToCore(audioTask, "sos-audio", 8192, nullptr, 4, nullptr, 1);
    const auto netResult = xTaskCreatePinnedToCore(networkTask, "sos-network", 16384, nullptr, 2, nullptr, 0);
    if (audioResult != pdPASS || netResult != pdPASS)
        Serial.println("Audio task creation failed; restart after checking free heap");
}

void loop() {
    delay(5000);
    // #region debug-point A-L:serial-snapshot
    Serial.printf("[DEBUG] session=audio-crackle-stability run=post-fix-15 hypotheses=A,B,C,D,E,F,G,H,I,J,K,L "
        "wifi=%d rssi=%d usb=%d tx=%d rx=%d up=%lu down=%lu under=%lu trim=%lu overflow=%lu drop=%lu "
        "mic=%lu..%lu usbTx=%lu..%lu usbTxPacket=%u usbRx=%lu..%lu "
        "usbRxDrop=%lu txXfer=%lu rxXfer=%lu "
        "capPeak=%lu sendBatch=%lu frameAgeMax=%lu stale=%lu superseded=%lu upLost=%lu upGap=%lu "
        "upArrivalMax=%lu upSourceMax=%lu "
        "udpFail=%lu udpFailBurst=%lu opusBytes=%lu..%lu opusErr=%lu "
        "netStackMin=%lu queueCap=%u target=%u "
        "heap=%u minHeap=%u maxBlock=%u "
        "psramFree=%u psramTotal=%u\n",
        int(WiFi.status()), WiFi.RSSI(), int(usb.mounted()), int(usb.isStreamingActiveTx()),
        int(usb.isStreamingActiveRx()), (unsigned long)receivedPackets.load(),
        (unsigned long)sentPackets.load(), (unsigned long)microphone.underruns.load(),
        (unsigned long)microphone.trims.load(), (unsigned long)microphone.overflows.load(),
        (unsigned long)capturedDrops.load(),
        (unsigned long)debugMicMin.exchange(UINT32_MAX), (unsigned long)debugMicMax.exchange(0),
        (unsigned long)debugUsbTxMin.exchange(UINT32_MAX), (unsigned long)debugUsbTxMax.exchange(0),
        unsigned(usb.getTxFrameBytesLast()),
        (unsigned long)debugUsbRxMin.exchange(UINT32_MAX), (unsigned long)debugUsbRxMax.exchange(0),
        (unsigned long)usb.getRxDroppedBytes(), (unsigned long)usb.getTxXferCount(),
        (unsigned long)usb.getRxXferCount(), (unsigned long)debugCapturePeak.exchange(0),
        (unsigned long)debugSendBatchMax.exchange(0), (unsigned long)debugFrameAgeMax.exchange(0),
        (unsigned long)debugStaleDrops.load(), (unsigned long)debugSuperseded.load(),
        (unsigned long)debugUplinkLost.load(),
        (unsigned long)debugUplinkGapMax.load(),
        (unsigned long)debugUplinkArrivalMax.exchange(0),
        (unsigned long)debugUplinkSourceMax.exchange(0),
        (unsigned long)debugUdpFailures.load(), (unsigned long)debugUdpFailureBurstMax.load(),
        (unsigned long)debugOpusBytesMin.exchange(UINT32_MAX),
        (unsigned long)debugOpusBytesMax.exchange(0), (unsigned long)debugOpusErrors.load(),
        (unsigned long)debugNetworkStackMin.load(),
        unsigned(sos::Playout::queueCapacity), unsigned(sos::Playout::target),
        ESP.getFreeHeap(), ESP.getMinFreeHeap(),
        ESP.getMaxAllocHeap(), ESP.getFreePsram(), ESP.getPsramSize());
    // #endregion
}
