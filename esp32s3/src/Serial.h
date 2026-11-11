#pragma once

// ---- Serial output control macros ------------------------------------------
// Serial output goes to native USB CDC (TinyUSB) as a composite device
// alongside the USB audio class, per
// https://github.com/pschatzmann/arduino-audio-tools/issues/2396.
// In core 3.x, ARDUINO_USB_MODE=0 is the TinyUSB/OTG mode required by both.
// Define LANMIC_ENABLE_SERIAL to enable all serial output.
// Undefine or comment out for silent builds (e.g. field deployments).
// Every translation unit includes this header, so the toggle stays global.
#include <USBCDC.h>
#include <esp32-hal-tinyusb.h> // TinyUSB's non-blocking CDC TX FIFO calls
#include <Arduino.h>
#define LANMIC_ENABLE_SERIAL

// The object itself is defined in main.cpp; with ARDUINO_USB_CDC_ON_BOOT=0
// the core provides no USBSerial instance of its own.
extern USBCDC USBSerial;
// The global USBCDC constructor already registered the CDC interfaces before
// setup() runs, so the stack (started by usb.begin()) builds a composite
// CDC+audio descriptor; USBSerial.begin() only arms the TX/RX plumbing.
#define SERIAL_INIT()          {USBSerial.begin(115200); Serial.begin(115200);}

// Core 3.1.1's USBCDC::write() must not be used here: it hands a chunk to
// TinyUSB, and the moment TinyUSB accepts only part of it the loop retries with
// no timeout and no yield while the TX FIFO stays full, starving IDLE0 until the
// task watchdog aborts the printing task (seen on sos-console, CPU 0). cdcSerial
// therefore calls TinyUSB's non-blocking TX FIFO API directly: it buffers a whole
// line, pushes only what the FIFO reports as free, and waits with vTaskDelay in
// between. Bytes the FIFO still holds when kLineBudgetMs runs out are dropped, not
// kept: they never reached the wire, and keeping them latches the port -- a full
// FIFO makes every later reply short, so the host stops hearing anything at all
// after the first timeout. One task's output cannot interleave byte-by-byte with
// another's, and a short line is reported on UART0 only
// ("[D] cdc short <sent>/<total>"). Core 3.1.1's dwc2 DCD can additionally leave
// CDC's IN endpoint transmitting from a TX FIFO nobody fills after a
// re-enumeration, so send() re-points it before each line (see Serial.cpp).
// shortcut: kLineBytes covers the longest reply (IPv6 addresses plus an
// escaped SSID); longer lines are dropped, not split.
struct BoundedCdcPrint : Print {
    // Bound this low on purpose: while send() waits, consoleTask is not draining
    // the CDC rx_queue, and a full queue blocks the "usbd" task in _onRX() and with
    // it the audio ISO IN transfers. A host that is reading takes 116 bytes in a few
    // ms, so 250 ms only ever costs a reply that was not going to arrive anyway.
    static constexpr uint32_t kLineBudgetMs = 250;
    static constexpr size_t kLineBytes = 512;

    size_t write(uint8_t c) override;
    void flush() override; // pushes a line that has no newline yet

private:
    char line[kLineBytes]{};
    size_t len = 0;
    bool dropping = false; // tail of an over-long line

    void send();
};
extern BoundedCdcPrint cdcSerial;

// UART0 only by design: 64 bytes is the whole CDC TX FIFO, and diagnostics
// printed to CDC starve the command replies that share it (seen as
// "cdc drop 17/116" right after three idle-log lines filled 47 of those 64).
// Console replies go through cdcSerial explicitly and own the CDC channel.
#ifdef LANMIC_ENABLE_SERIAL
  #define SERIAL_PRINTF(...)     {cdcSerial.printf(__VA_ARGS__); Serial.printf(__VA_ARGS__);}
#else
  #define SERIAL_PRINTF(...)     ((void)0)
#endif
