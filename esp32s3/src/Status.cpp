#include "Status.h"

#include <WiFi.h>

namespace lanmic {

std::atomic<bool> wifiResetInProgress{false};
std::atomic<bool> statusConfigPortalActive{false};
std::atomic<bool> statusAudioStreaming{false};
std::atomic<bool> statusResetArmed{false};
std::atomic<bool> statusError{false};
std::atomic<uint32_t> wifiSnapshotConnected{0};
std::atomic<uint32_t> wifiSnapshotIpRaw{0};

void setStatusLed(bool on) {
    digitalWrite(kStatusLedPin, on == kStatusLedActiveHigh ? HIGH : LOW);
}

void refreshWifiSnapshot() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    wifiSnapshotConnected.store(connected ? 1u : 0u, std::memory_order_release);
    wifiSnapshotIpRaw.store(connected ? uint32_t(WiFi.localIP()) : 0u,
                            std::memory_order_release);
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
        } else if (!wifiSnapshotConnected.load(std::memory_order_acquire)) {
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

} // namespace lanmic
