#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sos {
constexpr uint32_t kMagic = 0x31534f53;
constexpr uint8_t kVersion = 6;
constexpr uint16_t kDiscoveryPort = 45678;
constexpr uint16_t kUplinkPort = 45679;
constexpr uint32_t kSampleRate = 48000;
// Fourteen USB milliseconds per packet reduce packet pressure while keeping
// the complete IPv4 datagram below the 1500-byte Ethernet/Wi-Fi MTU.
constexpr size_t kFrameSamples = 672;
constexpr uint32_t kFrameDurationMs = 14;
constexpr size_t kOpusFrameSamples = 960;
constexpr uint32_t kOpusFrameDurationMs = 20;
constexpr int kOpusBitrate = 192000;
constexpr size_t kOpusMaxBytes = 512;
constexpr size_t kHeaderSize = 28;
constexpr size_t kFrameBytes = kFrameSamples * 2;
constexpr size_t kMaxPacketSize = kHeaderSize + kFrameBytes;
constexpr uint32_t kLeaseMs = 3500;
constexpr uint32_t kAudioTimeoutMs = 500;
constexpr const char* kDeviceName = "vst_lan_mic";

enum class Kind : uint8_t {
    Discover = 1, Offer = 2, Subscribe = 3, Ack = 4, Audio = 5, AudioOpus = 6
};
enum Role : uint16_t { Sender = 1, Receiver = 2, Busy = 0x8000 };

inline uint16_t get16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t get32(const uint8_t* p) {
    return uint32_t(get16(p)) | (uint32_t(get16(p + 2)) << 16);
}
inline void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
    put16(p, uint16_t(v)); put16(p + 2, uint16_t(v >> 16));
}

struct Header {
    Kind kind = Kind::Discover;
    uint16_t flags = 0;
    uint32_t session = 0;
    uint32_t sequence = 0;
    uint32_t timestampMs = 0;
    uint16_t payloadBytes = 0;
};

inline size_t encode(uint8_t* p, const Header& h) {
    put32(p, kMagic);
    p[4] = kVersion;
    p[5] = uint8_t(h.kind);
    put16(p + 6, h.flags);
    put32(p + 8, h.session);
    put32(p + 12, h.sequence);
    put32(p + 16, h.timestampMs);
    put32(p + 20, kSampleRate);
    put16(p + 24, h.payloadBytes);
    put16(p + 26, 0);
    return kHeaderSize + h.payloadBytes;
}

inline bool decode(const uint8_t* p, size_t size, Header& h) {
    if (size < kHeaderSize || size > kMaxPacketSize ||
        get32(p) != kMagic || p[4] != kVersion ||
        get32(p + 20) != kSampleRate || get16(p + 26) != 0 ||
        p[5] < uint8_t(Kind::Discover) || p[5] > uint8_t(Kind::AudioOpus))
        return false;
    h = {Kind(p[5]), get16(p + 6), get32(p + 8), get32(p + 12),
         get32(p + 16), get16(p + 24)};
    if (size != kHeaderSize + h.payloadBytes) return false;
    switch (h.kind) {
        case Kind::Audio:
            return h.flags == 0 && h.session != 0 && h.payloadBytes == kFrameBytes;
        case Kind::AudioOpus:
            return h.flags == 0 && h.session != 0 &&
                   h.payloadBytes > 0 && h.payloadBytes <= kOpusMaxBytes;
        case Kind::Offer:
            return h.flags == 0 && h.session != 0 &&
                   h.payloadBytes > 0 && h.payloadBytes <= 64;
        case Kind::Subscribe:
            return (h.flags == Sender || h.flags == Receiver) &&
                   h.session != 0 && h.payloadBytes == 0;
        case Kind::Ack:
            return ((h.flags & ~Busy) == Sender || (h.flags & ~Busy) == Receiver) &&
                   h.session != 0 && h.payloadBytes == 0;
        case Kind::Discover:
            return h.flags == 0 && h.payloadBytes == 0;
    }
    return false;
}

using Frame = std::array<int16_t, kFrameSamples>;
using OpusFrame = std::array<int16_t, kOpusFrameSamples>;
inline void encodePcm(uint8_t* p, const Frame& frame) {
    for (size_t i = 0; i < frame.size(); ++i)
        put16(p + i * 2, uint16_t(frame[i]));
}
inline Frame decodePcm(const uint8_t* p) {
    Frame frame{};
    for (size_t i = 0; i < frame.size(); ++i) {
        const auto v = get16(p + i * 2);
        frame[i] = int16_t(v < 32768 ? int32_t(v) : int32_t(v) - 65536);
    }
    return frame;
}
} // namespace sos
