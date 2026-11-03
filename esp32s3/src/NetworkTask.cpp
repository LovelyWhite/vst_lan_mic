#include "NetworkTask.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <opus.h>
#include <algorithm>
#include <array>
#include <cstring>

#include "AudioTasks.h"
#include "DeviceIdentity.h"
#include "Log.h"
#include "NonBlockingUdp.h"
#include "Pacer.h"
#include "Stats.h"
#include "Status.h"

namespace lanmic {
namespace {

constexpr int kMaxControlPacketsPerNetLoop = 2;
constexpr int kMaxAudioPacketsPerNetLoop = 1;
constexpr int kMaxDownlinkFramesPerNetLoop = 1;
constexpr uint32_t kMaxEncodedBacklogFrames = 8;
// Opus PLC synthesizes at most 300 ms of concealment; longer gaps reset the
// decoder instead of padding with noise.
constexpr uint32_t kMaxPlcFrames = 300 / sos::kOpusFrameDurationMs;
// Concealment frames synthesized per pass. A full 300 ms gap would otherwise
// run 15 Opus decodes in one pass; the remainder is spread over later passes,
// which the 300 ms playout target absorbs.
constexpr uint32_t kMaxPlcPerPass = 3;

constexpr uint32_t plcFramesForGap(uint32_t gap) {
    return gap <= kMaxPlcFrames ? gap : 0;
}
static_assert(plcFramesForGap(0) == 0 && plcFramesForGap(1) == 1,
              "Only missing frames need PLC");
static_assert(plcFramesForGap(kMaxPlcFrames) == kMaxPlcFrames &&
              plcFramesForGap(kMaxPlcFrames + 1) == 0 &&
              plcFramesForGap(0x7fffffffu) == 0,
              "Long gaps must reset without a PLC burst");

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

} // namespace

void networkTask(void*) {
    auto& microphone = audioQueues->microphone;
    // Socket polling cadence (discovery keepalive + uplink receive + lease
    // checks). Downlink sends do not ride on it: they are driven by
    // encoderTask's enqueue notifications (see waitNotify). With no socket it
    // drops to 50 ms (reconnection is timer-driven anyway). The jitter that
    // 5 ms polling adds to the 20 ms uplink frame period is fully absorbed by
    // the 300 ms uplink target buffer.
    Pacer pacer{5000};
    NonBlockingUdp control, audio;
    NonBlockingUdp::Packet received;
    Peer sender, receiver;
    bool uplinkSequenceValid = false;
    uint32_t nextUplinkSequence = 0;
    // Concealment owed to playout, and the uplink frame staged behind it: the
    // frame is decoded only once its PLC frames are out, so ordering holds.
    uint32_t plcPending = 0, heldSequence = 0;
    uint16_t heldPayloadBytes = 0;
    uint32_t lastRetry = 0, lastValidAudio = 0;
    uint32_t nextDownlinkAttemptMs = 0;
    uint32_t downlinkBackoffMs = 10;
    uint32_t lastSentMs = millis();
    uint32_t lastDiagnosticsMs = 0, diagnosticsSequence = 0;
    bool socketsReady = false;
    bool notified = false; // True when the last waitNotify woke on an enqueue notification (lets the consumer detect the empty-queue race).
    EncodedFrame pendingDownlink;
    IPAddress boundIp, currentIp;
    std::array<uint8_t, sos::kMaxPacketSize + 1> packet{};

    // Uplink decoder mirrors the plugin's stereo 20 ms encoder profile; like the
    // encoder it stays in internal RAM.
    alignas(4) static sos::OpusFrame decoded{};
    // Dedicated to the uplink so a staged frame survives control/downlink reuse
    // of packet.
    alignas(4) static std::array<uint8_t, sos::kMaxPacketSize + 1> audioPacket{};
    const int opusDecoderBytes = opus_decoder_get_size(sos::kOpusChannels);
    void* opusDecoderMemory = opusDecoderBytes > 0
        ? heap_caps_malloc(size_t(opusDecoderBytes), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : nullptr;
    auto* opusDecoder = static_cast<OpusDecoder*>(opusDecoderMemory);
    int opusDecoderStatus = opusDecoder
        ? opus_decoder_init(opusDecoder, sos::kSampleRate, sos::kOpusChannels)
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
        plcPending = 0;
        heldPayloadBytes = 0;
        ++stats.uplinkResets;
        opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
    };

    // Emits at most kMaxPlcPerPass concealment frames; callers only invoke it
    // when plcPending is 0 or has just been set, so the per-pass cap holds.
    auto emitPlc = [&] {
        for (uint32_t i = 0; i < kMaxPlcPerPass && plcPending != 0; ++i) {
            if (decodeIntoFrame(nullptr, 0) != int(sos::kOpusFrameSamples)) {
                resetUplink(); // Also drops the frame staged behind it.
                return;
            }
            stats.plcFrames.fetch_add(1, std::memory_order_relaxed);
            microphone.push(decoded);
            --plcPending;
        }
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
        const int64_t netLoopStartUs = esp_timer_get_time(); // Pass start, for work-time stats only.
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
            for (int n = 0; n < kMaxControlPacketsPerNetLoop; ++n) {
                const int got = control.read(packet.data(), packet.size(), received);
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
                const auto sent = control.sendTo(ip, port, packet.data(), bytes);
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
            // Concealment left over from an earlier pass goes out first so its
            // frames stay ahead of the real frame staged behind them.
            emitPlc();
            for (int n = 0; n < kMaxAudioPacketsPerNetLoop; ++n) {
                if (heldPayloadBytes == 0) {
                    const int got = audio.read(audioPacket.data(), audioPacket.size(), received);
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
                    if (got != length || !sos::decode(audioPacket.data(), size_t(got), h) ||
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
                    plcPending = plcFramesForGap(gap);
                    heldSequence = h.sequence;
                    heldPayloadBytes = h.payloadBytes;
                    emitPlc(); // Small gaps still finish inside this pass.
                }
                if (plcPending != 0) break; // Longer gaps defer the staged frame.
                const int samples = decodeIntoFrame(audioPacket.data() + sos::kHeaderSize,
                                                    heldPayloadBytes);
                if (samples != int(sos::kOpusFrameSamples)) {
                    resetUplink();
                    continue;
                }
                microphone.push(decoded);
                nextUplinkSequence = heldSequence + 1;
                heldPayloadBytes = 0;
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
            ulTaskNotifyTake(pdTRUE, 0); // Clear stale notifications after frame drops to avoid an empty wakeup.
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
                if (!encodedDownlink.peek(pendingDownlink)) break; // Queue drained.
                sos::Header h{sos::Kind::AudioOpus, 0, bootSession,
                              pendingDownlink.sequence, pendingDownlink.capturedMs,
                              pendingDownlink.payloadBytes};
                const auto bytes = sos::encode(packet.data(), h);
                memcpy(packet.data() + sos::kHeaderSize,
                       pendingDownlink.payload.data(),
                       pendingDownlink.payloadBytes);
                const int64_t downlinkStepUs = esp_timer_get_time();
                const auto sent = audio.sendTo(receiver.ip, receiver.port,
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
                        // Frame was pending and the socket refused it: the
                        // meaning diagnostics report as pollBlocked.
                        ++stats.downlinkPollBlocked;
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
            control.sendTo(receiver.ip, receiver.port, packet.data(), bytes);
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

} // namespace lanmic
