#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstdint>

#include "AudioBuffer.h"

namespace lanmic {

struct CapturedFrame {
    alignas(4) sos::OpusFrame pcm{};
    uint32_t capturedMs = 0, sequence = 0;
};
struct EncodedFrame {
    std::array<uint8_t, sos::kOpusMaxBytes> payload{};
    uint32_t capturedMs = 0, sequence = 0;
    uint16_t payloadBytes = 0;
};
struct AudioQueues {
    sos::StereoPlayout microphone; // Uplink (DAW -> phone) keeps the DAW's L/R.
};
extern AudioQueues* audioQueues; // PSRAM, initialized before any audio task starts.
// Encoder task is the only producer, networkTask is the only consumer.
extern sos::SpscRing<EncodedFrame, 16> encodedDownlink;
// audioTask produces, encoderTask consumes. BSS global so the ring lands in
// internal RAM: a measured 3.8 KB PSRAM push cost ~106 us inside the 1 ms tick.
extern sos::SpscRing<CapturedFrame, 8> captured;
extern std::atomic<uint32_t> uplinkEpoch, lastUplinkMs;
extern std::atomic<uint32_t> downlinkEpoch, downlinkReceiverActive;
extern TaskHandle_t encoderTaskHandle, networkTaskHandle;

void audioTask(void*);
void encoderTask(void*);

} // namespace lanmic
