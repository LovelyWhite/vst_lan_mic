#include "UsbAudio.h"

namespace lanmic {

#if CONFIG_IDF_TARGET_ESP32S3 && ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(3, 1, 1)
UsbAudioStream usb;
#else
audio_tools::USBAudioStream usb;
#endif

} // namespace lanmic
