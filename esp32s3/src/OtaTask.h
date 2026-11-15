#pragma once

namespace lanmic {

// Silent boot-time OTA, called inline from setup() right after Wi-Fi is up:
// fetch manifest.json over plain HTTP, compare its "version" against
// LANMIC_VERSION and, when newer, stream firmware.bin into the inactive OTA
// partition (status LED flashes, progress prints per 10%) and reboot before
// any audio or network task starts. One check per boot; anything the check
// misses waits for the next power cycle.
void otaBootUpdate();

} // namespace lanmic
