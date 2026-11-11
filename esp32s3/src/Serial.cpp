#include "Serial.h"

#include <esp_arduino_version.h>
#include <freertos/semphr.h>
#include <cassert>

#if CONFIG_IDF_TARGET_ESP32S3 && ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(3, 1, 1)
#include <soc/usb_reg.h>
#include <soc/usb_struct.h>
#endif

namespace {

#if CONFIG_IDF_TARGET_ESP32S3 && ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(3, 1, 1)
// The DCD hands diepctl.TXFNUM out from an activation-order counter that a bus
// reset never clears, while both dieptxf[epnum-1] and the IN ISR's
// USB0.fifo[epnum] are keyed by endpoint number. A re-enumeration can therefore
// leave CDC's IN ep4 transmitting from FIFO1, whose region (dieptxf[0], a
// power-on default) nothing ever fills: ep_in NAKs forever, its transfer never
// completes, dtknqr4_fifoemptymsk keeps bit 4 set, and the TXFEMP storm that
// follows overflows ep4's real 16-word FIFO into the audio FIFO next to it.
// Re-point TXFNUM at the FIFO the DCD already sized for ep4 -- the correction
// UsbAudio.h applies to the audio IN endpoints. Written only with EPENA clear,
// the state dcd_edpt_open itself programs TXFNUM in, so an already latched
// transfer is left to the next enumeration.
// shortcut: 0x84/ep4 is CDC's ep_in in core 3.1.1; ep5 (notification) is never
// transmitted here, so its own counter-assigned TXFNUM is left alone.
void fixCdcTxFifoNum() {
    volatile uint32_t& ctl = USB0.in_ep_reg[4].diepctl;
    if (ctl & USB_D_EPENA1_M) return;
    const uint32_t want = 4u << USB_D_TXFNUM1_S;
    if ((ctl & USB_D_TXFNUM1_M) == want) return;
    ctl = (ctl & ~USB_D_TXFNUM1_M) | want;
    assert(((ctl & USB_D_TXFNUM1_M) >> USB_D_TXFNUM1_S) == 4u);
    Serial.printf("[D] cdc ep4 txfnum -> 4\n");
}
#else
void fixCdcTxFifoNum() {}
#endif

} // namespace

// See Serial.h: TinyUSB's own TX FIFO calls are used instead of USBCDC::write(),
// which loops without a timeout or yield as soon as a chunk is accepted only
// partially (the FIFO holds what the host has not drained). Every wait for room
// blocks in vTaskDelay here, keeping IDLE0 runnable and the watchdog fed even
// when USB audio ISO traffic starves CDC IN.
void BoundedCdcPrint::send() {
    static SemaphoreHandle_t txLock = xSemaphoreCreateMutex();
    const size_t bytes = len;
    len = 0;
    if (!bytes || !txLock ||
        xSemaphoreTake(txLock, pdMS_TO_TICKS(kLineBudgetMs)) != pdTRUE) {
        return; // the port is busy with another line or went dead: drop ours
    }
    // Before the first flush below re-arms ep_in: a transfer submitted while
    // TXFNUM names a FIFO nobody fills never reaches the wire.
    fixCdcTxFifoNum();
    const uint32_t started = millis();
    size_t off = 0;
    // No tud_cdc_connected() gate: the host asserts DTR when it opens the port, so
    // the gate never guarded a real absence -- it only turned a FIFO the host was
    // still draining into a whole-line drop. FIFO space plus the budget is the
    // actual backpressure.
    while (off < bytes) {
        const size_t before = off;
        const uint32_t space = tud_cdc_write_available();
        if (space) {
            const size_t left = bytes - off;
            off += tud_cdc_write(line + off, space < left ? space : left);
        }
        // Flush on a full FIFO too, exactly as USBCDC::write() does: flush is what
        // re-arms ep_in, and a transfer completed while nobody flushed leaves the
        // bytes sitting in the FIFO with no way back onto the wire.
        tud_cdc_write_flush();
        if (off == before) {
            if (uint32_t(millis() - started) >= kLineBudgetMs) break;
            vTaskDelay(1); // 10 ms ticks; the host drains CDC between waits
        }
    }
    xSemaphoreGive(txLock);
    // Reported on UART0 only: CDC is the channel that failed, and write() returns 1
    // for a buffered line, so Print never sees an error for a short reply.
    if (off < bytes) {
        Serial.printf("[D] cdc short %u/%u\n", unsigned(off), unsigned(bytes));
        // Drop what never reached the wire: those bytes were not transmitted, so
        // this cannot corrupt what the host already got, and leaving them is what
        // latches the port -- a full FIFO makes every later reply short, and it
        // stays mute until a power cycle.
        if (!tud_cdc_write_available()) tud_cdc_write_clear();
    }
}

size_t BoundedCdcPrint::write(uint8_t c) {
    if (dropping) {
        if (c == '\n') dropping = false;
    } else if (len == sizeof(line)) {
        len = 0; // over-long line: discard it whole, never half of it
        dropping = c != '\n';
    } else {
        line[len++] = char(c);
        if (c == '\n') send();
    }
    return 1; // buffered: Print gets no error for a line dropped on the wire
}

void BoundedCdcPrint::flush() {
    if (len) send(); // send() hands every chunk to TinyUSB itself
}
