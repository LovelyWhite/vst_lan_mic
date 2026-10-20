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
constexpr char kProvisioningHead[] = R"HTML(
<style>
:root{color-scheme:light}
body{background:#f5f7f8!important;color:#263238!important;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif!important}
.wrap{max-width:460px!important;padding:24px!important}
h1{color:#168f81!important;letter-spacing:.04em}
h3{color:#607d80!important}
button{background:#168f81!important;color:#fff!important;border:0!important;border-radius:10px!important;font-weight:600!important}
input,select{background:#fff!important;color:#263238!important;border:1px solid #b8c9cc!important;border-radius:8px!important}
a{color:#13776d!important}
form[action="/"]{display:none!important}
</style>
<script>
document.addEventListener("DOMContentLoaded",function(){
  document.title="LAN MIC";
  if(location.pathname==="/"){
    location.replace("/wifi");
    return;
  }
  var walker=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);
  var nodes=[],node;
  while(node=walker.nextNode())nodes.push(node);
  nodes.forEach(function(n){
    n.nodeValue=n.nodeValue
      .replace(/WiFiManager/g,"LAN MIC")
      .replace(/ESP(?:32|8266)?/g,"LAN MIC");
  });
});
</script>
)HTML";

audio_tools::USBAudioStream usb;
constexpr uint32_t kUsbUnityGainQ16 = 65536;
std::atomic<uint32_t> usbSpeakerLeftGainQ16{kUsbUnityGainQ16};
std::atomic<uint32_t> usbSpeakerRightGainQ16{kUsbUnityGainQ16};
std::atomic<uint32_t> usbSpeakerLeftMuted{0};
std::atomic<uint32_t> usbSpeakerRightMuted{0};
sos::Playout microphone;
struct CapturedFrame {
    sos::OpusFrame pcm{};
    uint32_t capturedMs = 0, sequence = 0;
};
sos::SpscRing<CapturedFrame, 8> captured;
std::atomic<uint32_t> uplinkEpoch{0}, lastUplinkMs{0};
std::atomic<uint32_t> capturedDrops{0}, sentPackets{0}, receivedPackets{0};
std::atomic<bool> wifiResetInProgress{false};
// #region debug-point P-Q-R-S:audio-path-metrics
std::atomic<uint32_t> debugMicMin{UINT32_MAX}, debugMicMax{0};
std::atomic<uint32_t> debugUsbTxMin{UINT32_MAX}, debugUsbTxMax{0};
std::atomic<uint32_t> debugUsbRxMin{UINT32_MAX}, debugUsbRxMax{0};
std::atomic<uint32_t> debugAudioLoopGapMax{0};
std::atomic<uint32_t> debugUsbTxProgressGapMax{0}, debugUsbTxAdvanceMax{0};
std::atomic<uint32_t> debugUsbTxDoneMin{UINT32_MAX}, debugUsbTxDoneMax{0};
std::atomic<uint32_t> debugCapturePeak{0}, debugSendBatchMax{0}, debugFrameAgeMax{0};
std::atomic<uint32_t> debugUdpFailures{0}, debugUdpFailureBurst{0}, debugUdpFailureBurstMax{0};
std::atomic<uint32_t> debugUplinkLost{0}, debugUplinkGapMax{0}, debugUplinkBridged{0};
std::atomic<uint32_t> debugStaleDrops{0}, debugSuperseded{0};
std::atomic<uint32_t> debugUplinkArrivalMax{0}, debugUplinkSourceMax{0};
std::atomic<uint32_t> debugNetworkStackMin{UINT32_MAX};
std::atomic<uint32_t> debugOpusErrors{0}, debugOpusBytesMin{UINT32_MAX}, debugOpusBytesMax{0};
// #endregion
char identity[64]{}, usbSerial[24]{};
uint32_t bootSession = 0;

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
    CapturedFrame frame;
    size_t frameUsed = 0;
    uint32_t captureSequence = 0;
    std::array<uint8_t, 192> speakerBytes{};
    std::array<int16_t, 96> micSamples{};
    int32_t speakerLeftGainQ16 = int32_t(kUsbUnityGainQ16);
    int32_t speakerRightGainQ16 = int32_t(kUsbUnityGainQ16);
    uint32_t lastAudioLoopMs = millis();
    uint32_t lastUsbTxProgressMs = lastAudioLoopMs;
    uint32_t lastUsbTxCount = usb.getTxXferCount();
    auto smoothGain = [](int32_t current, int32_t target) {
        const int32_t difference = target - current;
        if (difference == 0) return current;
        int32_t step = difference / 240; // About 5 ms at 48 kHz.
        if (step == 0) step = difference > 0 ? 1 : -1;
        return current + step;
    };
    for (;;) {
        const uint32_t audioLoopNow = millis();
        const bool txActive = usb.mounted() && usb.isStreamingActiveTx();
        const bool rxActive = usb.mounted() && usb.isStreamingActiveRx();
        // #region debug-point Q:playout-levels
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
        // #region debug-point R:usb-in-progress
        debugAudioLoopGapMax.store(std::max(
            debugAudioLoopGapMax.load(), uint32_t(audioLoopNow - lastAudioLoopMs)));
        lastAudioLoopMs = audioLoopNow;
        const uint32_t usbTxCount = usb.getTxXferCount();
        if (txActive && usbTxCount != lastUsbTxCount) {
            debugUsbTxProgressGapMax.store(std::max(
                debugUsbTxProgressGapMax.load(),
                uint32_t(audioLoopNow - lastUsbTxProgressMs)));
            debugUsbTxAdvanceMax.store(std::max(
                debugUsbTxAdvanceMax.load(), uint32_t(usbTxCount - lastUsbTxCount)));
            const uint32_t completedBytes = usb.getTxXferredLast();
            if (completedBytes != 0) {
                debugUsbTxDoneMin.store(std::min(
                    debugUsbTxDoneMin.load(), completedBytes));
                debugUsbTxDoneMax.store(std::max(
                    debugUsbTxDoneMax.load(), completedBytes));
            }
            lastUsbTxProgressMs = audioLoopNow;
        } else if (!txActive) {
            lastUsbTxProgressMs = audioLoopNow;
        }
        lastUsbTxCount = usbTxCount;
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
                frame.pcm[frameUsed++] = int16_t((scaledLeft + scaledRight) / 2);
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
    // Keep the complete encoder state in internal RAM; do not fall back to PSRAM.
    void* opusMemory = opusEncoderBytes > 0
        ? heap_caps_malloc(size_t(opusEncoderBytes), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
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
        if (wifiResetInProgress.load(std::memory_order_acquire)) {
            if (socketsReady) {
                control.stop();
                audio.stop();
                socketsReady = false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
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
                    // #region debug-point P-S:uplink-gaps
                    debugUplinkLost.store(sequence.lost);
                    debugUplinkGapMax.store(sequence.maxGap);
                    debugUplinkBridged.store(sequence.bridged);
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
    config.fifo_packets = 16;
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
        Serial.println("USB audio initialization failed");
        return;
    }
    pinMode(kWifiResetPin, INPUT_PULLUP);
    WiFiManager wifiManager;
    wifiManager.setDebugOutput(false);
    wifiManager.setConnectTimeout(15);
    wifiManager.setConfigPortalTimeout(0);
    wifiManager.setCustomHeadElement(kProvisioningHead);
    wifiManager.setShowStaticFields(false);
    wifiManager.setShowDnsFields(false);
    wifiManager.setShowInfoErase(false);
    Serial.printf("Open Wi-Fi provisioning AP: %s\n", provisioningSsid);
    if (!wifiManager.autoConnect(provisioningSsid)) {
        Serial.println("Wi-Fi provisioning failed to start; rebooting");
        delay(1000);
        ESP.restart();
    }
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    const auto audioResult = xTaskCreatePinnedToCore(audioTask, "sos-audio", 8192, nullptr, 4, nullptr, 1);
    const auto netResult = xTaskCreatePinnedToCore(networkTask, "sos-network", 16384, nullptr, 2, nullptr, 0);
    if (audioResult != pdPASS || netResult != pdPASS)
        Serial.println("Audio task creation failed; restart after checking free heap");
}

void loop() {
    static uint32_t lastDebugMs = millis();
    static uint32_t wifiResetPressedMs = 0;
    static bool wifiResetArmed = false;
    delay(25);

    const uint32_t now = millis();
    if (digitalRead(kWifiResetPin) == LOW) {
        if (!wifiResetPressedMs) wifiResetPressedMs = now;
        if (!wifiResetArmed &&
            uint32_t(now - wifiResetPressedMs) >= kWifiResetHoldMs) {
            wifiResetArmed = true;
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

    const bool usbAudioActive =
        usb.isStreamingActiveTx() || usb.isStreamingActiveRx();
    if (usbAudioActive || uint32_t(now - lastDebugMs) < 5000) return;
    lastDebugMs = now;
    // #region debug-point P-Q-R-S:serial-snapshot
    Serial.printf("[DEBUG] session=daw-phone-stutter run=post-fix-periodic hypotheses=P,Q,R,S,T,U,V "
        "wifi=%d rssi=%d usb=%d tx=%d rx=%d up=%lu down=%lu under=%lu trim=%lu overflow=%lu drop=%lu "
        "mic=%lu..%lu usbTx=%lu..%lu usbTxPacket=%u usbRx=%lu..%lu "
        "audioLoopGapMax=%lu usbTxGapMax=%lu usbTxAdvanceMax=%lu usbTxDone=%lu..%lu "
        "usbVol=%u/%u usbMute=%u/%u "
        "usbRxDrop=%lu txXfer=%lu rxXfer=%lu "
        "capPeak=%lu sendBatch=%lu frameAgeMax=%lu stale=%lu superseded=%lu "
        "upLost=%lu upGap=%lu upBridge=%lu "
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
        (unsigned long)debugAudioLoopGapMax.exchange(0),
        (unsigned long)debugUsbTxProgressGapMax.exchange(0),
        (unsigned long)debugUsbTxAdvanceMax.exchange(0),
        (unsigned long)debugUsbTxDoneMin.exchange(UINT32_MAX),
        (unsigned long)debugUsbTxDoneMax.exchange(0),
        unsigned((usbSpeakerLeftGainQ16.load() * 100 + 32768) >> 16),
        unsigned((usbSpeakerRightGainQ16.load() * 100 + 32768) >> 16),
        unsigned(usbSpeakerLeftMuted.load()), unsigned(usbSpeakerRightMuted.load()),
        (unsigned long)usb.getRxDroppedBytes(), (unsigned long)usb.getTxXferCount(),
        (unsigned long)usb.getRxXferCount(), (unsigned long)debugCapturePeak.exchange(0),
        (unsigned long)debugSendBatchMax.exchange(0), (unsigned long)debugFrameAgeMax.exchange(0),
        (unsigned long)debugStaleDrops.load(), (unsigned long)debugSuperseded.load(),
        (unsigned long)debugUplinkLost.load(),
        (unsigned long)debugUplinkGapMax.load(),
        (unsigned long)debugUplinkBridged.load(),
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
