#include "Transport.h"

#include <opus.h>

namespace {
uint32_t makeSession() {
    const auto n = uint32_t(juce::Random::getSystemRandom().nextInt());
    return n == 0 ? 1 : n;
}
uint32_t nowMs() { return juce::Time::getMillisecondCounter(); }
constexpr uint32_t kMaxPlcFrames = 300 / sos::kOpusFrameDurationMs;
}

Transport::Transport(bool sending)
    : Thread(sending ? "vst2esp network" : "esp2vst network"),
      sending_(sending), session_(makeSession()) {
    startThread();
}

Transport::~Transport() {
    signalThreadShouldExit();
    // The worker only waits up to 2 ms on its socket; do not kill it mid-operation.
    stopThread(-1);
}

void Transport::setPreferredDevice(const juce::String& value) {
    const juce::ScopedLock lock(settingsLock_);
    const auto cleaned = value.trim().substring(0, 64);
    if (preferred_ == cleaned) return;
    preferred_ = cleaned;
    ++preferenceVersion_;
}

juce::String Transport::preferredDevice() const {
    const juce::ScopedLock lock(settingsLock_);
    return preferred_;
}

std::vector<Transport::Device> Transport::devices() const {
    const juce::ScopedLock lock(settingsLock_);
    return devices_;
}

juce::String Transport::status() const {
    juce::String text;
    switch (state_.load()) {
        case State::Searching: text = "Searching on the local network"; break;
        case State::Connecting: text = "Connecting"; break;
        case State::Connected: text = "Connected"; break;
        case State::Busy: text = "Device direction is in use by another instance"; break;
        case State::SocketError: text = "UDP socket error; retrying"; break;
    }
    const juce::ScopedLock lock(settingsLock_);
    if (selected_.isNotEmpty()) text += " | " + selected_;
    return text;
}

void Transport::enqueue(const sos::Frame& pcm, uint32_t now, uint32_t sequence) {
    const Outgoing frame{pcm, now, sequence};
    if (!outgoing_.push(&frame, 1)) ++dropped;
}

void Transport::invalidateAudio() { ++audioEpoch_; }

void Transport::prepareReceive() {
    const auto epoch = audioEpoch_.load(std::memory_order_acquire);
    if (epoch != consumedEpoch_) {
        incoming_.reset();
        consumedEpoch_ = epoch;
    }
    incoming_.prepare();
}

void Transport::run() {
    int decoderStatus = OPUS_OK;
    OpusDecoder* decoder = sending_
        ? nullptr
        : opus_decoder_create(sos::kSampleRate, 1, &decoderStatus);
    if (!sending_ && (!decoder || decoderStatus != OPUS_OK)) {
        if (decoder) opus_decoder_destroy(decoder);
        ++socketErrors;
        state_ = State::SocketError;
        return;
    }

    while (!threadShouldExit()) {
        juce::DatagramSocket socket(true);
        if (!socket.bindToPort(0)) {
            state_ = State::SocketError;
            ++socketErrors;
            wait(500);
            continue;
        }
        uint32_t version = preferenceVersion_.load();
        juce::String peerIp, preferred = preferredDevice();
        uint32_t peerBoot = 0, selectedAt = 0, lastAck = 0, lastAudio = 0;
        uint32_t lastDiscover = nowMs() - 1000, lastSubscribe = nowMs() - 1000;
        uint32_t nextAudioSend = nowMs();
        bool wasEnabled = false, haveAck = false, haveAudio = false;
        bool sequenceValid = false;
        uint32_t nextSequence = 0;
        std::array<uint8_t, sos::kMaxPacketSize + 1> packet{};
        sos::OpusFrame decoded{};
        const uint16_t role = sending_ ? sos::Sender : sos::Receiver;

        auto resetReceiveState = [&] {
            sequenceValid = false;
            if (decoder) opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        };
        auto clearPeer = [&] {
            outgoing_.clear();
            nextAudioSend = nowMs();
            peerIp.clear();
            peerBoot = 0;
            haveAck = haveAudio = false;
            state_ = State::Searching;
            resetReceiveState();
            invalidateAudio();
            const juce::ScopedLock lock(settingsLock_);
            selected_.clear();
        };
        auto sendControl = [&](const juce::String& ip, sos::Kind kind, uint16_t flags) {
            sos::Header h{kind, flags, session_, 0, nowMs(), 0};
            const auto bytes = int(sos::encode(packet.data(), h));
            if (socket.waitUntilReady(false, 0) != 1 ||
                socket.write(ip, sos::kDiscoveryPort, packet.data(), bytes) != bytes) ++socketErrors;
        };
        clearPeer();

        while (!threadShouldExit()) {
            const auto now = nowMs();
            const bool enabled = enabled_.load(std::memory_order_acquire);
            const auto newVersion = preferenceVersion_.load();
            if (newVersion != version) {
                version = newVersion;
                preferred = preferredDevice();
                clearPeer();
                lastDiscover = now - 1000;
            }
            if (enabled != wasEnabled) {
                outgoing_.clear();
                nextAudioSend = now;
                haveAck = haveAudio = false;
                resetReceiveState();
                invalidateAudio();
                state_ = peerIp.isEmpty() ? State::Searching : State::Connecting;
                selectedAt = now;
                lastSubscribe = now - 1000;
                wasEnabled = enabled;
            }
            if (peerIp.isNotEmpty() && enabled &&
                uint32_t(now - (haveAck ? lastAck : selectedAt)) >= sos::kLeaseMs) {
                clearPeer();
            }
            if (haveAudio && uint32_t(now - lastAudio) >= sos::kAudioTimeoutMs) {
                haveAudio = false;
                resetReceiveState();
                invalidateAudio();
            }
            if (uint32_t(now - lastDiscover) >= 1000) {
                lastDiscover = now;
                // Limited broadcast plus per-interface directed broadcasts
                // cover machines with Ethernet, Wi-Fi, and VPN adapters.
                sendControl("255.255.255.255", sos::Kind::Discover, 0);
                for (const auto& address : juce::IPAddress::getAllAddresses()) {
                    if (address.isIPv6 || address.address[0] == 127) continue;
                    const auto broadcast = juce::IPAddress::getInterfaceBroadcastAddress(address);
                    if (!broadcast.isNull() && broadcast != juce::IPAddress::broadcast())
                        sendControl(broadcast.toString(), sos::Kind::Discover, 0);
                }
                const juce::ScopedLock lock(settingsLock_);
                devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                    [now](const Device& d) { return uint32_t(now - d.seenMs) >= 5000; }),
                    devices_.end());
            }
            if (enabled && peerIp.isNotEmpty() && uint32_t(now - lastSubscribe) >= 1000) {
                lastSubscribe = now;
                sendControl(peerIp, sos::Kind::Subscribe, role);
            }

            const int ready = socket.waitUntilReady(true, 2);
            if (ready < 0) {
                ++socketErrors;
                clearPeer();
                state_ = State::SocketError;
                break;
            }
            for (int n = 0; n < 32 && socket.waitUntilReady(true, 0) == 1; ++n) {
                juce::String source;
                int port = 0;
                const int bytes = socket.read(packet.data(), int(packet.size()), false, source, port);
                sos::Header h;
                if (bytes <= 0 || !sos::decode(packet.data(), size_t(bytes), h)) continue;
                if (h.kind == sos::Kind::Offer && port == sos::kDiscoveryPort) {
                    const auto identity = juce::String::fromUTF8(
                        reinterpret_cast<const char*>(packet.data() + sos::kHeaderSize), h.payloadBytes);
                    if (!identity.startsWith("vst_lan_mic-") ||
                        !identity.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"))
                        continue;
                    {
                        const juce::ScopedLock lock(settingsLock_);
                        auto found = std::find_if(devices_.begin(), devices_.end(),
                            [&](const Device& d) { return d.identity == identity; });
                        if (found != devices_.end()) *found = {identity, source, now};
                        else if (devices_.size() < 32) devices_.push_back({identity, source, now});
                    }
                    if (peerIp.isEmpty() && (preferred.isEmpty() || preferred == identity)) {
                        peerIp = source;
                        peerBoot = h.session;
                        selectedAt = now;
                        lastSubscribe = now - 1000;
                        state_ = State::Connecting;
                        const juce::ScopedLock lock(settingsLock_);
                        selected_ = identity + " @ " + source;
                    } else if (source == peerIp && h.session != peerBoot) {
                        clearPeer(); // Reboot: require discovery and a fresh lease.
                    }
                } else if (h.kind == sos::Kind::Ack && source == peerIp &&
                           port == sos::kDiscoveryPort && h.session == session_ &&
                           (h.flags & ~sos::Busy) == role && enabled) {
                    lastAck = now;
                    haveAck = true;
                    const bool busy = (h.flags & sos::Busy) != 0;
                    if (busy && state_.load() != State::Busy) {
                        resetReceiveState();
                        invalidateAudio();
                    }
                    state_ = busy ? State::Busy : State::Connected;
                } else if (!sending_ && enabled && connected() &&
                           h.kind == sos::Kind::AudioOpus &&
                           source == peerIp && port == sos::kUplinkPort && h.session == peerBoot) {
                    uint32_t gap = 0;
                    if (sequenceValid) {
                        gap = h.sequence - nextSequence;
                        if (gap >= 0x80000000u) continue; // Duplicate or late packet.
                    }
                    if (gap > 0) {
                        lost.fetch_add(gap);
                        if (gap > kMaxPlcFrames)
                            opus_decoder_ctl(decoder, OPUS_RESET_STATE);
                    }

                    bool decodeOk = true;
                    const uint32_t plcFrames = std::min(gap, kMaxPlcFrames);
                    for (uint32_t i = 0; i < plcFrames; ++i) {
                        const int samples = opus_decode(
                            decoder, nullptr, 0, decoded.data(),
                            int(decoded.size()), 0);
                        if (samples != int(decoded.size())) {
                            decodeOk = false;
                            break;
                        }
                        if (!incoming_.queue.push(decoded.data(), uint32_t(samples))) {
                            ++dropped;
                            invalidateAudio();
                        }
                    }

                    const int samples = decodeOk
                        ? opus_decode(decoder, packet.data() + sos::kHeaderSize,
                                      h.payloadBytes, decoded.data(),
                                      int(decoded.size()), 0)
                        : OPUS_INTERNAL_ERROR;
                    if (samples != int(decoded.size())) {
                        ++dropped;
                        resetReceiveState();
                        invalidateAudio();
                        continue;
                    }
                    if (!incoming_.queue.push(decoded.data(), uint32_t(samples))) {
                        ++dropped;
                        // A paused DAW must not replay old queued samples on resume.
                        invalidateAudio();
                    }
                    nextSequence = h.sequence + 1;
                    sequenceValid = true;
                    lastAudio = now;
                    haveAudio = true;
                    ++received;
                }
            }

            const uint32_t sendNow = nowMs();
            if (sending_ && int32_t(sendNow - nextAudioSend) >= 0 && outgoing_.size() > 0) {
                const uint32_t queued = outgoing_.size();
                if (queued > 1) {
                    outgoing_.discard(queued - 1);
                    dropped.fetch_add(queued - 1);
                }
                Outgoing frame;
                if (outgoing_.pop(frame)) {
                    nextAudioSend += sos::kFrameDurationMs;
                    if (int32_t(sendNow - nextAudioSend) >= 0)
                        nextAudioSend = sendNow + sos::kFrameDurationMs;
                    if (!enabled || !connected() ||
                        uint32_t(sendNow - frame.timeMs) > 300) {
                        ++dropped;
                    } else {
                        sos::Header h{sos::Kind::Audio, 0, session_, frame.sequence, frame.timeMs,
                                      uint16_t(sos::kFrameBytes)};
                        const auto bytes = int(sos::encode(packet.data(), h));
                        sos::encodePcm(packet.data() + sos::kHeaderSize, frame.pcm);
                        if (socket.waitUntilReady(false, 0) == 1 &&
                            socket.write(peerIp, sos::kUplinkPort, packet.data(), bytes) == bytes)
                            ++sent;
                        else
                            ++socketErrors;
                    }
                }
            }
        }
        invalidateAudio();
        if (!threadShouldExit()) wait(100);
    }
    if (decoder) opus_decoder_destroy(decoder);
}
