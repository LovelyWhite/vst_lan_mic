#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <freertos/idf_additions.h>
#include <cassert>
#include <cstdio>
#include <new>

#include "AudioTasks.h"
#include "DeviceIdentity.h"
#include "Log.h"
#include "NetworkTask.h"
#include "Status.h"
#include "UsbAudio.h"

namespace lanmic {

char identity[64]{}, usbSerial[24]{};
uint32_t bootSession = 0;

namespace {

constexpr uint8_t kWifiResetPin = 0;
constexpr uint32_t kWifiResetHoldMs = 3000;
constexpr uint32_t kWifiSnapshotIntervalMs = 1000;

// Reserve internal RAM before heap fragmentation; Opus scratch is hot, PCM queues are not.
alignas(portBYTE_ALIGNMENT) StackType_t encoderTaskStack[49152 / sizeof(StackType_t)];
StaticTask_t encoderTaskControl;
static_assert(sizeof(encoderTaskStack) == 49152, "Keep the measured encoder stack budget");

} // namespace
} // namespace lanmic

using namespace lanmic;

void setup() {
    S_INIT(); // UART bridge, not native USB CDC.
    pinMode(kStatusLedPin, OUTPUT);
    setStatusLed(false);
    const auto ledResult = xTaskCreatePinnedToCore(
        statusLedTask, "status-led", 2048, nullptr, 1, nullptr, 0);
    if (ledResult != pdPASS) {
        setStatusLed(true);
        S_PRINTF("Status LED task creation failed\n");
    }
    void* queueMemory = heap_caps_malloc(
        sizeof(AudioQueues), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!queueMemory) {
        statusError.store(true, std::memory_order_relaxed);
        S_PRINTF("Audio queues PSRAM allocation failed: need=%u,maxBlock=%u\n",
            unsigned(sizeof(AudioQueues)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        return;
    }
    assert(esp_ptr_external_ram(queueMemory));
    assert(reinterpret_cast<uintptr_t>(queueMemory) % alignof(AudioQueues) == 0);
    assert(esp_ptr_internal(encoderTaskStack));
    audioQueues = new (queueMemory) AudioQueues{};
    audio_tools::AudioToolsLogger.setLogLevel(audio_tools::AudioLogger::Error);
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(usbSerial, sizeof(usbSerial), "%012llX", (unsigned long long)mac);
    char deviceNumber[7]{};
    snprintf(deviceNumber, sizeof(deviceNumber), "%06llX",
             (unsigned long long)(mac & 0xFFFFFFULL));
    snprintf(identity, sizeof(identity), "%s-%s", sos::kDeviceName, deviceNumber);
    bootSession = esp_random();
    if (!bootSession) bootSession = 1;
    char provisioningSsid[33]{};
    snprintf(provisioningSsid, sizeof(provisioningSsid), "%s", identity);

    audio_tools::USBAudioConfig config;
    config.sample_rate = sos::kSampleRate;
    config.channels = 2;
    config.bits_per_sample = 16;
    config.product = identity;
    config.manufacturer = "suxitech";
    config.serial = usbSerial;
    config.enable_ep_in = config.enable_ep_out = true;
    config.enable_feedback_ep = false;
    config.enable_multi_sample_rate = false;
    // The descriptor reserves 49 samples, but flow control sends exactly
    // 48 stereo samples (192 bytes) per 1 ms frame at 48 kHz.
    config.enable_ep_in_flow_control = true;
    // 48 packets absorb stalls that exceeded the old 16-packet FIFO, avoiding
    // audible IN zero-padding; adds ~32 ms latency and also enlarges bufferRx().
    config.fifo_packets = 48;
    config.rx_start_fill_percent = 0;
    // Host volume controls the phone-playback (USB OUT) path in audioTask.
    // Enabling this library option would instead scale USB IN microphone data.
    config.volume_active = false;
    config.self_powered = false;
    config.max_power_ma = 250;
    config.begin_usb = true;
    usb.setReqEntityCallback([](audio_tools::USBAudioDeviceBase* device,
                                uint8_t,
                                const audio_tools::UsbSetupPacket& request,
                                uint8_t*) {
        const uint8_t entity = audio_tools::u16High(request.wIndex);
        if (entity != audio_tools::USBAudio2DescriptorBuilder::ENTITY_FU1 ||
            request.bRequest != audio_tools::kAudioCsReqCur)
            return true;
        const uint8_t control = audio_tools::u16High(request.wValue);
        const uint8_t channel = audio_tools::u16Low(request.wValue);
        if (control == audio_tools::AUDIO_FU_CTRL_VOLUME)
            onUsbVolume(device->volume(channel), channel);
        else if (control == audio_tools::AUDIO_FU_CTRL_MUTE)
            onUsbMute(device->isMute(channel), channel);
        return true;
    });
    if (!usb.begin(config)) {
        statusError.store(true, std::memory_order_relaxed);
        S_PRINTF("USB audio initialization failed\n");
        return;
    }
    pinMode(kWifiResetPin, INPUT_PULLUP);
    WiFiManager wifiManager;
    wifiManager.setDebugOutput(false);
    wifiManager.setConnectTimeout(6);
    wifiManager.setConfigPortalTimeout(0);
    wifiManager.setShowInfoUpdate(false);
    wifiManager.setTitle("LAN MIC");
    wifiManager.setAPCallback([](WiFiManager*) {
        statusConfigPortalActive.store(true, std::memory_order_relaxed);
    });
    wifiManager.setWebServerCallback([&]() {
    wifiManager.server->on("/", [&]() {
        wifiManager.server->sendHeader("Location", "/wifi", true);
        wifiManager.server->send(302, "text/plain", "");
      });
    });
    S_PRINTF("Open Wi-Fi provisioning AP: %s\n", provisioningSsid);
    if (!wifiManager.autoConnect(provisioningSsid)) {
        statusError.store(true, std::memory_order_relaxed);
        S_PRINTF("Wi-Fi provisioning failed to start; rebooting\n");
        delay(1000);
        ESP.restart();
    }
    statusConfigPortalActive.store(false, std::memory_order_relaxed);
    refreshWifiSnapshot();
    // Wi-Fi modem sleep keeps the IDF default here; audioTask widens it to
    // WIFI_PS_NONE only while USB audio is streaming (see kWifiIdleSleepMs).
    WiFi.setAutoReconnect(true);
    S_PRINTF("Before audio/network tasks: heap=%u,minHeap=%u,maxBlock=%u\n",
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    const auto audioResult = xTaskCreatePinnedToCore(
        audioTask, "sos-audio", 8192, nullptr, 4, nullptr, 0);
    // shortcut: initial alloca budgets for 20 ms audio, remeasure high-water marks before changing codec settings.
    // Encoding owns Core 1: under heavy Wi-Fi retransmits the high-priority
    // lwIP tasks would crowd a shared core, slowing encoding and growing
    // raw_trim; networkTask on Core 0 being preempted by audioTask (4) is the
    // safe direction, with both-direction buffers absorbing its jitter.
    const auto netResult = xTaskCreatePinnedToCore(
        networkTask, "sos-network", 32768, nullptr, 3, &networkTaskHandle, 0);
    encoderTaskHandle = xTaskCreateStaticPinnedToCore(
        encoderTask, "sos-encoder", sizeof(encoderTaskStack), nullptr, 3,
        encoderTaskStack, &encoderTaskControl, 1);
    const BaseType_t encoderResult = encoderTaskHandle ? pdPASS : errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    if (audioResult != pdPASS || encoderResult != pdPASS || netResult != pdPASS) {
        statusError.store(true, std::memory_order_relaxed);
        S_PRINTF("Audio/network task creation failed: audio=%ld,network=%ld,encoder=%ld,"
                 "heap=%u,minHeap=%u,maxBlock=%u,psramHeap=%u,psramMaxBlock=%u\n",
            long(audioResult), long(netResult), long(encoderResult),
            unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    }
}

void loop() {
    static uint32_t wifiResetPressedMs = 0;
    static bool wifiResetArmed = false;
    delay(25);

    const uint32_t now = millis();
    static uint32_t lastWifiSnapshotMs = 0;
    if (uint32_t(now - lastWifiSnapshotMs) >= kWifiSnapshotIntervalMs) {
        lastWifiSnapshotMs = now;
        refreshWifiSnapshot();
    }

    if (digitalRead(kWifiResetPin) == LOW) {
        if (!wifiResetPressedMs) wifiResetPressedMs = now;
        if (!wifiResetArmed &&
            uint32_t(now - wifiResetPressedMs) >= kWifiResetHoldMs) {
            wifiResetArmed = true;
            statusResetArmed.store(true, std::memory_order_relaxed);
            S_PRINTF("Wi-Fi reset armed; release BOOT to restart provisioning\n");
        }
    } else {
        wifiResetPressedMs = 0;
        if (wifiResetArmed) {
            S_PRINTF("Clearing saved Wi-Fi credentials\n");
            wifiResetInProgress.store(true, std::memory_order_release);
            WiFi.setAutoReconnect(false);
            delay(100);
            WiFi.disconnect(false, false, 1000);
            delay(100);
            const bool erased = WiFi.eraseAP();
            WiFi.mode(WIFI_OFF);
            S_PRINTF("%s\n", erased ? "Saved Wi-Fi credentials cleared" : "Failed to clear saved Wi-Fi credentials");
            delay(250);
            ESP.restart();
        }
    }
}
