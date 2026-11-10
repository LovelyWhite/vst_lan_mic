#pragma once

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <esp32-hal-tinyusb.h>
#include <cassert>
#include <cstdint>

// Audio Tools supplies its own class driver. Older cores use the CUSTOM slot;
// Arduino 3.3.12 and newer expose the AUDIO slot directly.
#if ESP_ARDUINO_VERSION_MAJOR != 2 && ESP_ARDUINO_VERSION_MAJOR != 3
#error Review the USB audio adapter for this Arduino major version.
#endif
#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#define USB_INTERFACE_AUDIO USB_INTERFACE_CUSTOM
#endif
#include <AudioTools/Communication/USB/USBAudioStream.h>
#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#undef USB_INTERFACE_AUDIO
#endif

#if CONFIG_IDF_TARGET_ESP32S3 && ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(3, 1, 1)
#include <soc/usb_reg.h>
#include <soc/usb_struct.h>
#endif

namespace lanmic {

#if CONFIG_IDF_TARGET_ESP32S3 && ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(3, 1, 1)
class UsbAudioBackend : public audio_tools::USBAudioBackendTinyUSB {
    bool isoActivateEndpoint(
        uint8_t rhport, const audio_tools::UsbEndpointDescriptorView& ep) override {
        const bool isIn = ep.direction() == audio_tools::UsbDir::In;
        const uint8_t number = ep.bEndpointAddress & 0x0f;
        if (isIn && (rhport != 0 || number == 0 || number > 4)) return false;
        if (!USBAudioBackendTinyUSB::isoActivateEndpoint(rhport, ep)) return false;
        if (isIn) {
            // shortcut: Arduino 3.1.1 increments TXFNUM on each activation; review when upgrading its DCD.
            // dfifo_alloc() and handle_epin_slave() instead address the FIFO by endpoint number.
            auto& ctl = USB0.in_ep_reg[number].diepctl;
            ctl = (ctl & ~USB_D_TXFNUM1_M) | (uint32_t(number) << USB_D_TXFNUM1_S);
            assert(((ctl & USB_D_TXFNUM1_M) >> USB_D_TXFNUM1_S) == number);
            assert((USB0.dieptxf[number - 1] >> 16) * 4 >= ep.packetSize());
        }
        return true;
    }
};

class UsbAudioStream : public audio_tools::USBAudioStream {
    UsbAudioBackend backend_;
    audio_tools::USBAudioBackend& backend() override { return backend_; }
};
extern UsbAudioStream usb;
#else
extern audio_tools::USBAudioStream usb;
#endif

} // namespace lanmic
