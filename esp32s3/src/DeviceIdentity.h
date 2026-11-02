#pragma once

#include <cstdint>

namespace lanmic {

// Filled by setup() before any task that answers discovery or stamps packets.
extern char identity[64];
extern char usbSerial[24];
extern uint32_t bootSession;

} // namespace lanmic
