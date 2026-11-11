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
#include <Arduino.h>
#define LANMIC_ENABLE_SERIAL

// The object itself is defined in main.cpp; with ARDUINO_USB_CDC_ON_BOOT=0
// the core provides no USBSerial instance of its own.
extern USBCDC USBSerial;
// The global USBCDC constructor already registered the CDC interfaces before
// setup() runs, so the stack (started by usb.begin()) builds a composite
// CDC+audio descriptor; USBSerial.begin() only arms the TX/RX plumbing.
#define SERIAL_INIT()          {USBSerial.begin(115200); Serial.begin(115200);}

// Core 3.1.1's USBCDC::write() busy-spins on the empty TinyUSB TX FIFO with
// no timeout while the host does not drain CDC IN (USB audio ISO traffic
// starves it during playback); the spin starves IDLE0 and the task watchdog
// aborts the printing task (seen on sos-console, CPU 0). cdcSerial never
// enters that loop: it buffers a whole line and pushes it in chunks no larger
// than the space the FIFO reports, waiting with vTaskDelay in between. A line
// that is not fully delivered within kLineBudgetMs is discarded whole, so the
// host-side framer never has to resynchronise on a half line, and one task's
// output can no longer interleave byte-by-byte with another's.
// shortcut: kLineBytes covers the longest reply (IPv6 addresses plus an
// escaped SSID); longer lines are dropped, not split.
struct BoundedCdcPrint : Print {
    static constexpr uint32_t kLineBudgetMs = 3000;
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

#ifdef LANMIC_ENABLE_SERIAL
  #define SERIAL_PRINTF(...)     {cdcSerial.printf(__VA_ARGS__); Serial.printf(__VA_ARGS__);}
#else
  #define SERIAL_PRINTF(...)     ((void)0)
#endif
