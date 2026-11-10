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
#ifdef LANMIC_ENABLE_SERIAL
  #define SERIAL_PRINTF(...)     {USBSerial.printf(__VA_ARGS__); Serial.printf(__VA_ARGS__);}
#else
  #define SERIAL_PRINTF(...)     ((void)0)
#endif
