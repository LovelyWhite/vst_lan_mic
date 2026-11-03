#include "Stats.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstdio>

#include "AudioTasks.h"
#include "Status.h"
#include "UsbAudio.h"

namespace lanmic {

DebugStats stats; // Single stats block: hot tasks write relaxed atomics, loop() reads at 1 Hz.

#ifdef LANMIC_ENABLE_SERIAL
void printStatsLine() {
    static uint32_t lastUsbTxCount = 0, lastStatsMs = 0;
    const uint32_t now = millis();
    const uint32_t statsWindowMs = uint32_t(now - lastStatsMs);
    lastStatsMs = now;
    const uint32_t capturedFrames = stats.capturedFrames.load(std::memory_order_relaxed);
    const uint32_t encodedFrames = stats.encodedFrames.load(std::memory_order_relaxed);
    const uint32_t sentFrames = stats.sentFrames.load(std::memory_order_relaxed);
    const uint32_t captureGapMs = stats.captureGapMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t encodeGapMs = stats.encodeGapMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t sendGapMs = stats.sendGapMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t encodeWaitMs = stats.encodeWaitMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t sendWaitMs = stats.sendWaitMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t sendAgeMs = stats.sendAgeMaxMs.exchange(0, std::memory_order_relaxed);
    const uint32_t encodeErrors = stats.encodeErrors.load(std::memory_order_relaxed);
    const int32_t encodeError = stats.lastEncodeError.load(std::memory_order_relaxed);
    const uint32_t pollBlocked = stats.downlinkPollBlocked.load(std::memory_order_relaxed);
    const uint32_t receiverActive = downlinkReceiverActive.load(std::memory_order_acquire);
    const uint32_t receiverEpoch = downlinkEpoch.load(std::memory_order_acquire);
    // Sample USB counters together before stack scans can delay this low-priority task.
    const uint32_t usbTxCount = usb.getTxXferCount(); // ISR-side cumulative completions (volatile word read, cross-task safe).
    const uint32_t usbTxReadBytes = usb.getTxFifoReadTotal();
    const uint32_t usbRxCount = usb.getRxXferCount();
    const uint32_t usbRxBytes = usb.getRxTotalBytes();
    const uint32_t usbRxDroppedBytes = usb.getRxDroppedBytes();
    const uint32_t usbTxXferDelta = uint32_t(usbTxCount - lastUsbTxCount);
    lastUsbTxCount = usbTxCount;
    // Swap-out resets the window cells; a better sample landing between the
    // swap and a concurrent writer is lost for at most one window.
    const uint32_t audioLoopUs =
        stats.audioLoopMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netLoopUs =
        stats.netLoopMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netStateUs =
        stats.netStateMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netControlUs =
        stats.netControlMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netAudioRxUs =
        stats.netAudioRxMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netDownlinkUs =
        stats.netDownlinkMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netEncodeUs =
        stats.netEncodeMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t netSendUs =
        stats.netSendMaxUs.exchange(0, std::memory_order_relaxed);
    const uint32_t rawPushFail =
        stats.rawPushFail.load(std::memory_order_relaxed);
    const uint32_t encodedPushFail =
        stats.encodedPushFail.load(std::memory_order_relaxed);
    const uint32_t rawTrimFrames =
        stats.rawTrimFrames.load(std::memory_order_relaxed);
    const uint32_t encodedTrimFrames =
        stats.encodedTrimFrames.load(std::memory_order_relaxed);
    const uint32_t controlReadReset =
        stats.controlReadReset.load(std::memory_order_relaxed);
    const uint32_t controlSendReset =
        stats.controlSendReset.load(std::memory_order_relaxed);
    const uint32_t audioReadReset =
        stats.audioReadReset.load(std::memory_order_relaxed);
    const uint32_t downlinkSendReset =
        stats.downlinkSendReset.load(std::memory_order_relaxed);
    const int32_t controlSendErrno =
        stats.controlSendErrno.load(std::memory_order_relaxed);
    const int32_t downlinkSendErrno =
        stats.downlinkSendErrno.load(std::memory_order_relaxed);
    const int32_t fifoMin =
        stats.usbTxFifoMinFree.exchange(INT32_MAX, std::memory_order_relaxed);
    const int32_t fifoMax =
        stats.usbTxFifoMaxFree.exchange(-1, std::memory_order_relaxed);
    const long rssi = wifiSnapshotRssi.load(std::memory_order_acquire);
    static bool healthSampled = false;
    static uint32_t lastHealthMs = 0;
    static uint32_t audioStackMin = 0, encoderStackMin = 0, netStackMin = 0;
    static uint32_t heapFree = 0, heapMinimum = 0, heapLargest = 0;
    if (!healthSampled || uint32_t(now - lastHealthMs) >= 5000) {
        // High-water marks are bytes on this core; 0 means no task handle.
        audioStackMin = audioTaskHandle
            ? uint32_t(uxTaskGetStackHighWaterMark(audioTaskHandle)) : 0u;
        encoderStackMin = encoderTaskHandle
            ? uint32_t(uxTaskGetStackHighWaterMark(encoderTaskHandle)) : 0u;
        netStackMin = networkTaskHandle
            ? uint32_t(uxTaskGetStackHighWaterMark(networkTaskHandle)) : 0u;
        heapFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        heapMinimum = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        heapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        lastHealthMs = now;
        healthSampled = true;
    }
    // Avoid Print::printf's temporary allocation and second formatting pass.
    static char line[2048];
    const int length = std::snprintf(line, sizeof(line),
        "t_ms=%lu,stats_window_ms=%lu,a_ovr=%u,a_anch=%u,n_ovr=%u,n_anch=%u,"
        "a_loop_max_us=%u,n_loop_max_us=%u,usb_tx_xfer=%u,"
        "n_state_us=%u,n_ctl_us=%u,n_rx_us=%u,n_dl_us=%u,"
        "n_enc_us=%u,n_send_us=%u,"
        "usb_fifo_min=%d,usb_fifo_max=%d,q_push_fail=%u,q_raw_fail=%u,"
        "q_enc_fail=%u,q_trim=%u,q_enc_trim=%u,q_pop_fail=%u,"
        "plc_frames=%u,ul_rst=%u,udp_send_fail=%u,dl_send_wouldblock=%u,"
        "ctl_send_errno=%ld,dl_send_errno=%ld,"
        "sock_ctl_read_rst=%u,sock_ctl_send_rst=%u,"
        "sock_rx_read_rst=%u,sock_dl_send_rst=%u,"
        "wifi_sleep_tog=%u,rssi_dbm=%ld,a_stack_min_b=%u,e_stack_min_b=%u,n_stack_min_b=%u,"
        "heap=%u,minHeap=%u,maxBlock=%u,health_age_ms=%u,"
        "usb_rx_xfer_total=%lu,usb_rx_bytes_total=%lu,usb_rx_drop_bytes=%lu,"
        "usb_tx_read_bytes_total=%lu,play_underrun=%lu,play_trim=%lu,play_overflow=%lu,"
        "dl_cap=%u,dl_enc=%u,dl_sent=%u,"
        "dl_cap_gap_ms=%u,dl_enc_gap_ms=%u,dl_send_gap_ms=%u,"
        "dl_enc_wait_ms=%u,dl_send_wait_ms=%u,dl_age_ms=%u,"
        "dl_enc_err=%u,dl_enc_code=%ld,dl_poll_blocked=%u,dl_active=%u,dl_epoch=%u\n",
        (unsigned long)now,
        (unsigned long)statsWindowMs,
        stats.audioPacerOverrun.load(std::memory_order_relaxed),
        stats.audioPacerAnchor.load(std::memory_order_relaxed),
        stats.netPacerOverrun.load(std::memory_order_relaxed),
        stats.netPacerAnchor.load(std::memory_order_relaxed),
        audioLoopUs, netLoopUs, usbTxXferDelta,
        netStateUs, netControlUs, netAudioRxUs, netDownlinkUs,
        netEncodeUs, netSendUs,
        fifoMin == INT32_MAX ? -1 : int(fifoMin),
        int(fifoMax),
        stats.queuePushFail.load(std::memory_order_relaxed),
        rawPushFail,
        encodedPushFail,
        rawTrimFrames,
        encodedTrimFrames,
        stats.queuePopFail.load(std::memory_order_relaxed),
        stats.plcFrames.load(std::memory_order_relaxed),
        stats.uplinkResets.load(std::memory_order_relaxed),
        stats.udpEndPacketFail.load(std::memory_order_relaxed),
        stats.downlinkSendWouldBlock.load(std::memory_order_relaxed),
        long(controlSendErrno),
        long(downlinkSendErrno),
        controlReadReset,
        controlSendReset,
        audioReadReset,
        downlinkSendReset,
        stats.wifiSleepToggles.load(std::memory_order_relaxed),
        rssi, audioStackMin, encoderStackMin, netStackMin,
        unsigned(heapFree), unsigned(heapMinimum), unsigned(heapLargest),
        unsigned(uint32_t(now - lastHealthMs)),
        (unsigned long)usbRxCount,
        (unsigned long)usbRxBytes,
        (unsigned long)usbRxDroppedBytes,
        (unsigned long)usbTxReadBytes,
        (unsigned long)(audioQueues ? audioQueues->microphone.underruns.load(std::memory_order_relaxed) : 0),
        (unsigned long)(audioQueues ? audioQueues->microphone.trims.load(std::memory_order_relaxed) : 0),
        (unsigned long)(audioQueues ? audioQueues->microphone.overflows.load(std::memory_order_relaxed) : 0),
        capturedFrames, encodedFrames, sentFrames,
        captureGapMs, encodeGapMs, sendGapMs,
        encodeWaitMs, sendWaitMs, sendAgeMs,
        encodeErrors, long(encodeError), pollBlocked, receiverActive, receiverEpoch);
    if (length <= 0 || size_t(length) >= sizeof(line)) {
        Serial.println("stats_format_error=1");
        return;
    }
    Serial.write(reinterpret_cast<const uint8_t*>(line), size_t(length));
}
#endif

} // namespace lanmic
