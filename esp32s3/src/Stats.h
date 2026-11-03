#pragma once

#include <atomic>
#include <cstdint>

#include "Log.h"

namespace lanmic {

// Hot tasks update atomics; loop() prints at 1 Hz. Counters accumulate since
// boot; window extrema are exchanged and reset by the reporter.
struct DebugStats {
    // wait(): overrun == anchor; missed deadlines re-anchor without catch-up.
    std::atomic<uint32_t> audioPacerOverrun{0}, audioPacerAnchor{0};
    // waitNotify(): overrun counts missed polling periods; anchor also counts
    // notifications, so anchor >= overrun. Normal deadline wakeups count neither.
    std::atomic<uint32_t> netPacerOverrun{0}, netPacerAnchor{0};
    // Per-pass maxima (microseconds, 1 s window; excludes pacer/backoff sleep).
    // Wall time, not CPU: these tasks are preempted by audioTask and the
    // Wi-Fi/lwIP threads, so a max is one descheduled pass, not a typical cost.
    std::atomic<uint32_t> audioLoopMaxUs{0}, netLoopMaxUs{0};
    // Per-phase maxima, all inside networkTask except netEncodeMaxUs, which
    // encoderTask writes around opus_encode. netAudioRxMaxUs is the UDP uplink
    // receive (recvfrom + the pass's bounded PLC decodes + one real decode +
    // playout push), not USB RX; USB RX lives in audioLoopMaxUs.
    std::atomic<uint32_t> netStateMaxUs{0};
    std::atomic<uint32_t> netControlMaxUs{0}, netAudioRxMaxUs{0}, netDownlinkMaxUs{0};
    std::atomic<uint32_t> netEncodeMaxUs{0}, netSendMaxUs{0};
    // Network diagnostics use cumulative timing, unaffected by serial window resets.
    std::atomic<uint32_t> receiveWorkUs{0}, encodeWorkUs{0};
    std::atomic<uint32_t> rawQueued{0}; // Sampled by the queue's consumer.
    // Downlink queues: push failure = a downlink frame dropped on a full
    // raw/encoded queue; pop failure = woken by an enqueue notification whose
    // frame the previous pass already sent. Costs one extra no-work pass, no
    // frame loss, so it grows steadily with downlink playtime and is not an
    // error: rate matters, and only versus dl_enc/dl_sent.
    std::atomic<uint32_t> queuePushFail{0}, queuePopFail{0};
    std::atomic<uint32_t> rawPushFail{0}, encodedPushFail{0};
    std::atomic<uint32_t> rawTrimFrames{0}, encodedTrimFrames{0};
    // PLC-concealed frames / uplink Opus decoder resets (both cumulative since boot).
    std::atomic<uint32_t> plcFrames{0}, uplinkResets{0};
    // Failed UDP sends (control replies + audio downlink, cumulative).
    std::atomic<uint32_t> udpEndPacketFail{0};
    // Downlink sendto calls returning WouldBlock with a frame pending;
    // downlinkPollBlocked counts the same event for the diagnostics payload.
    std::atomic<uint32_t> downlinkSendWouldBlock{0};
    // Device -> DAW pipeline: cumulative counts and window timing maxima.
    std::atomic<uint32_t> capturedFrames{0}, encodedFrames{0}, sentFrames{0};
    std::atomic<uint32_t> encodeErrors{0}, downlinkPollBlocked{0};
    std::atomic<int32_t> lastEncodeError{0};
    std::atomic<uint32_t> captureGapMaxMs{0}, encodeGapMaxMs{0}, sendGapMaxMs{0};
    std::atomic<uint32_t> encodeWaitMaxMs{0}, sendWaitMaxMs{0}, sendAgeMaxMs{0};
    // errno of the most recent sendto hard error (0 = none yet).
    std::atomic<int32_t> controlSendErrno{0}, downlinkSendErrno{0};
    // Breakdown of socket rebuild reasons (cumulative).
    std::atomic<uint32_t> controlReadReset{0}, controlSendReset{0};
    std::atomic<uint32_t> audioReadReset{0}, downlinkSendReset{0};
    // Wi-Fi modem sleep state transitions (entering/leaving each count once, cumulative).
    std::atomic<uint32_t> wifiSleepToggles{0};
    // USB TX FIFO free-space window values (bytes; min = fullest, max = emptiest).
    std::atomic<int32_t> usbTxFifoMinFree{INT32_MAX};
    std::atomic<int32_t> usbTxFifoMaxFree{-1};
};
extern DebugStats stats; // Single stats block: hot tasks write relaxed atomics, loop() reads at 1 Hz.

// Publish a window max/min via CAS so a better sample landing between the
// reporter's read and reset can never be clobbered by a stale value.
template <typename T>
void statsMax(std::atomic<T>& cell, T value) {
    T current = cell.load(std::memory_order_relaxed);
    while (value > current &&
           !cell.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {}
}

inline void statsMin(std::atomic<int32_t>& cell, int32_t value) {
    int32_t current = cell.load(std::memory_order_relaxed);
    while (value < current &&
           !cell.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {}
}

#ifdef LANMIC_ENABLE_SERIAL
// 1Hz stats run on low-priority loop; no per-frame prints. UART transmission
// at 115200 baud takes >100ms; low priority avoids blocking realtime tasks.
// Stack/heap sampled every 5s. Inline name=value fields, no header. Key units:
// t_ms: timestamp; *_ovr/*_anch/*_fail/*_frames/*_rst/*_tog: cumulative counts
// (n_ovr = missed polling cycles; n_anch includes notification re-anchors, differs from a_* metrics, see waitNotify);
// *_loop_max_us/n_*_us: microsecond max latencies; usb_tx_xfer: ISO-IN transfers in window;
// stats_window_ms: actual sample interval; usb_fifo_*: free bytes (-1=unsampled);
// rssi_dbm: Wi-Fi RSSI (-128=disconnected); *_stack_min_b: min remaining task stack bytes (0=task not created);
// health_age_ms: age of cached stack/heap samples in ms.
void printStatsLine();
#endif

} // namespace lanmic
