#pragma once

#include <Arduino.h>
#include <cstdint>
#include <freertos/idf_additions.h>

namespace lanmic {

// Derive tick duration from FreeRTOS config rather than assuming 1 kHz.
constexpr int64_t kTickUs = 1000000 / int64_t(configTICK_RATE_HZ);

// Tick-anchored pacing avoids adding loop work to every relative delay;
// overruns re-anchor rather than replaying every missed deadline.
struct Pacer {
  TickType_t periodTicks;
  TickType_t nextTick;
  // Counter semantics are documented in DebugStats.
  uint32_t overrun = 0, anchor = 0;

  explicit Pacer(int64_t periodUs) {
    periodTicks = usToTicks(periodUs);
    nextTick = xTaskGetTickCount();
  }

  static TickType_t usToTicks(int64_t us) {
    return TickType_t((us + kTickUs - 1) / kTickUs); // Round up.
  }

  void setPeriodUs(int64_t us) {
    periodTicks = usToTicks(us);
  }

  void wait() {
    TickType_t now = xTaskGetTickCount();
    if ((now - nextTick) < periodTicks) {
      // Not due yet; block until nextTick.
      xTaskDelayUntil(&nextTick, periodTicks);
    } else {
      // Already overdue; re-anchor instead of catching up.
      ++overrun;
      ++anchor;
      nextTick = now;
    }
  }

  // Return true on a notification, re-anchoring to the wakeup tick.
  // Notifications coalesce: pending frames still need later notifications or polls.
  bool waitNotify() {
    TickType_t now = xTaskGetTickCount();
    const TickType_t diff = TickType_t(now - nextTick);
    if (diff < periodTicks) {
      // period-diff stays in (0, period]; nextTick-now can spin at zero
      // or underflow into a huge timeout, stalling the consumer.
      const TickType_t remaining = TickType_t(periodTicks - diff);
      if (ulTaskNotifyTake(pdTRUE, remaining) != 0) {
        // Read the tick after waking; a pre-wait timestamp would drift.
        ++anchor;
        nextTick = xTaskGetTickCount();
        return true;
      }
      // Slept to the grid point: this pass consumed the slot, advance one period.
      nextTick = TickType_t(nextTick + periodTicks);
      return false;
    }
    // More than a full polling period late (backoff sleep or work overrun mid-loop): re-anchor, no catch-up.
    ++overrun;
    ++anchor;
    nextTick = now;
    return false;
  }
};

} // namespace lanmic
