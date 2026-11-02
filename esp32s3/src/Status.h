#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstdint>

namespace lanmic {

constexpr uint8_t kStatusLedPin = 18;
constexpr bool kStatusLedActiveHigh = true;

extern std::atomic<bool> wifiResetInProgress;
extern std::atomic<bool> statusConfigPortalActive;
extern std::atomic<bool> statusAudioStreaming;
extern std::atomic<bool> statusResetArmed;
extern std::atomic<bool> statusError;
extern std::atomic<uint32_t> wifiSnapshotConnected;
extern std::atomic<uint32_t> wifiSnapshotIpRaw;
extern std::atomic<int32_t> wifiSnapshotRssi;

void setStatusLed(bool on);
void refreshWifiSnapshot();
void statusLedTask(void*);

} // namespace lanmic
