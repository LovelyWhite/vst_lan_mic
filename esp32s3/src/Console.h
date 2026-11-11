#pragma once

#include <Arduino.h>
#include <cstddef>

namespace lanmic {

// Settings kept in NVS (namespace "lanmic"), loaded once by loadDeviceConfig()
// before the USB audio descriptor and the provisioning AP are built.
// consoleTask reads "command [argument]" lines from USB CDC and answers with
// one pipe-delimited line per command starting with OK/OKR/ERR, so a script
// can tell replies apart from the "[D] ..." diagnostics other tasks print.
// The field order is documented in README.md.
// deviceName is the user-facing name: it is the provisioning AP's SSID and the
// display name the VST3 plugin shows. The eFuse-based identity stays fixed and
// keeps doing the device recognition.
extern char deviceName[33];
extern bool enableEpIn, enableEpOut;

void loadDeviceConfig();
void consoleTask(void*);
// Re-initialize USB audio after an endpoint switch (defined in main.cpp);
// the host re-enumerates, no reboot needed.
void restartUsbAudio();

} // namespace lanmic
