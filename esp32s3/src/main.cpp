#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_arduino_version.h>
#include <esp_system.h>
#include <esp32-hal-tinyusb.h>

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
    sos::Frame pcm{};
    uint32_t capturedMs = 0, sequence = 0;
};
sos::SpscRing<CapturedFrame, 16> captured;
std::atomic<uint32_t> uplinkEpoch{0}, lastUplinkMs{0};
std::atomic<uint32_t> capturedDrops{0}, sentPackets{0}, receivedPackets{0};
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
        const auto newEpoch = uplinkEpoch.load(std::memory_order_acquire);
        const bool fresh = uint32_t(millis() - lastUplinkMs.load()) < sos::kAudioTimeoutMs;
        if (newEpoch != epoch || txActive != txWasActive || (uplinkWasFresh && !fresh)) {
            microphone.reset();
            epoch = newEpoch;
        }
        txWasActive = txActive;
        uplinkWasFresh = fresh;

        // Cap the USB software FIFO at 2 ms, not its full allocated capacity.
        // Checking free space avoids Audio Tools' blocking write retry path.
        if (txActive) {
            microphone.prepare();
            for (int batch = 0; batch < 2 && usb.bufferTx().available() < 384; ++batch) {
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
                if (frameUsed == sos::kFrameSamples) {
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
    bool socketsReady = false;
    IPAddress boundIp;
    std::array<uint8_t, sos::kMaxPacketSize + 1> packet{};

    auto invalidateSender = [&] {
        sender = Peer{};
        sequence.reset();
        ++uplinkEpoch;
    };
    for (;;) {
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
                            ++uplinkEpoch;
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
                    lastValidAudio = now;
                    lastUplinkMs.store(now, std::memory_order_release);
                    ++receivedPackets;
                }
            }
        }

        // Always drain capture, including while disconnected: never replay stale audio.
        CapturedFrame frame;
        for (int n = 0; n < 16 && captured.pop(frame); ++n) {
            if (!socketsReady || !receiver.active(now) ||
                uint32_t(now - frame.capturedMs) > 30) continue;
            sos::Header h{sos::Kind::Audio, 0, bootSession, frame.sequence, frame.capturedMs,
                          uint16_t(sos::kFrameBytes)};
            const auto bytes = sos::encode(packet.data(), h);
            sos::encodePcm(packet.data() + sos::kHeaderSize, frame.pcm);
            if (audio.beginPacket(receiver.ip, receiver.port)) {
                audio.write(packet.data(), bytes);
                if (audio.endPacket()) ++sentPackets;
            }
        }
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
    config.enable_ep_in_flow_control = false; // Exactly 48 stereo samples per SOF.
    config.fifo_packets = 8;
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
    const auto netResult = xTaskCreatePinnedToCore(networkTask, "sos-network", 8192, nullptr, 2, nullptr, 0);
    if (audioResult != pdPASS || netResult != pdPASS)
        Serial.println("Audio task creation failed; restart after checking free heap");
}

void loop() {
    delay(5000);
    Serial.printf("wifi=%d usb=%d tx=%d rx=%d up=%lu down=%lu under=%lu trim=%lu drop=%lu\n",
        int(WiFi.status()), int(usb.mounted()), int(usb.isStreamingActiveTx()),
        int(usb.isStreamingActiveRx()), (unsigned long)receivedPackets.load(),
        (unsigned long)sentPackets.load(), (unsigned long)microphone.underruns.load(),
        (unsigned long)microphone.trims.load(), (unsigned long)capturedDrops.load());
}
