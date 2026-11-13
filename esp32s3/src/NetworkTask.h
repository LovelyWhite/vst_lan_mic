#pragma once

namespace lanmic {

// Opus decoder state, malloc'd in setup() before Wi-Fi provisioning fragments
// the internal heap; consumed (and freed) by networkTask.
extern void* opusDecoderMemory;

void networkTask(void*);

} // namespace lanmic
