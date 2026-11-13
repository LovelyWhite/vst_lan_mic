#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_heap_caps.h>
#include <opus.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <freertos/idf_additions.h>
#include <cassert>
#include <cstdio>
#include <new>

#include "AudioTasks.h"
#include "Console.h"
#include "DeviceIdentity.h"
#include "NetworkTask.h"
#include "Serial.h"
#include "Status.h"
#include "UsbAudio.h"

namespace lanmic {

char identity[64]{}, usbSerial[24]{};
uint32_t bootSession = 0;
void* opusDecoderMemory = nullptr;

namespace {

constexpr uint8_t kWifiResetPin = 0;
constexpr uint32_t kWifiResetHoldMs = 3000;
constexpr uint32_t kWifiSnapshotIntervalMs = 1000;

// Reserve internal RAM before heap fragmentation; Opus scratch is hot, PCM queues are not.
// Network/audio stacks are static too: a plain xTaskCreate after the provisioning
// portal fails silently (unchecked pdFAIL) and the task never runs.
alignas(portBYTE_ALIGNMENT) StackType_t encoderTaskStack[49152 / sizeof(StackType_t)];
StaticTask_t encoderTaskControl;
alignas(portBYTE_ALIGNMENT) StackType_t networkTaskStack[32768 / sizeof(StackType_t)];
StaticTask_t networkTaskControl;
alignas(portBYTE_ALIGNMENT) StackType_t audioTaskStack[16384 / sizeof(StackType_t)];
StaticTask_t audioTaskControl;
static_assert(sizeof(encoderTaskStack) == 49152, "Keep the measured encoder stack budget");
// ESP-IDF interprets the static-create depth in BYTES (verified by disassembling
// the linked libfreertos.a), so the old 8 KB was the real budget and the kernel
// watermark caught a genuine overflow in the USB gain + playout call chain.
static_assert(sizeof(audioTaskStack) == 16384, "Keep the doubled audio stack budget");

} // namespace
} // namespace lanmic

using namespace lanmic;

#ifdef LANMIC_ENABLE_SERIAL
USBCDC USBSerial; // Definition for the extern declaration in Serial.h.
#endif

BoundedCdcPrint cdcSerial; // Definition for the extern declaration in Serial.h.

void setup() {
    // CDC registers first so it owns interfaces 0-1; the audio class follows.
    SERIAL_INIT();
    pinMode(kStatusLedPin, OUTPUT);
    setStatusLed(false);
    xTaskCreatePinnedToCore(
        statusLedTask, "status-led", 2048, nullptr, 1, nullptr, 0);
    void* queueMemory = heap_caps_malloc(
        sizeof(AudioQueues), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!queueMemory) {
        statusError.store(true, std::memory_order_relaxed);
        SERIAL_PRINTF("[D] Audio queues PSRAM allocation failed: need=%u,maxBlock=%u\n",
            unsigned(sizeof(AudioQueues)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        return;
    }
    assert(esp_ptr_external_ram(queueMemory));
    assert(reinterpret_cast<uintptr_t>(queueMemory) % alignof(AudioQueues) == 0);
    assert(esp_ptr_internal(encoderTaskStack));
    assert(esp_ptr_internal(networkTaskStack) && esp_ptr_internal(audioTaskStack));
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
    loadDeviceConfig();
    // Started before USB audio comes up, so a broken endpoint setting (a
    // usb.begin() failure below) or the blocking provisioning portal can still
    // be fixed over serial. Core 1, priority 1: off core 0's audio/network
    // tasks and below encoderTask (2).
    xTaskCreatePinnedToCore(consoleTask, "sos-console", 4096, nullptr, 1, nullptr, 0);

    audio_tools::USBAudioConfig config;
    config.sample_rate = sos::kSampleRate;
    config.channels = 2;
    config.bits_per_sample = 16;
    config.product = identity;
    config.manufacturer = "suxitech";
    config.serial = usbSerial;
    config.enable_ep_in = enableEpIn;
    config.enable_ep_out = enableEpOut;
    config.enable_feedback_ep = false;
    config.enable_multi_sample_rate = false;
    // The descriptor reserves 49 samples, but flow control sends exactly
    // 48 stereo samples (192 bytes) per 1 ms frame at 48 kHz.
    config.enable_ep_in_flow_control = true;
    config.fifo_packets = 32;
    config.rx_start_fill_percent = 0;
    // Downlink volume comes from the playback Feature Unit in readBytes();
    // the mic Feature Unit keeps its own state, applied in AudioTasks.
    config.volume_active = true;
    config.self_powered = false;
    config.max_power_ma = 250;
    config.begin_usb = true;
    if (!usb.begin(config)) {
        statusError.store(true, std::memory_order_relaxed);
        SERIAL_PRINTF("[D] USB audio initialization failed\n");
        return;
    }
    pinMode(kWifiResetPin, INPUT_PULLUP);
    // Reserve the uplink decoder's contiguous internal RAM up front: after the
    // provisioning portal (WebServer, DNSServer, scans) comes and goes, the
    // heap is fragmented and this allocation is what fails (Opus -7).
    const int opusDecoderBytes = opus_decoder_get_size(sos::kOpusChannels);
    opusDecoderMemory = opusDecoderBytes > 0
        ? heap_caps_malloc(size_t(opusDecoderBytes), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : nullptr;
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
    SERIAL_PRINTF("[D] Open Wi-Fi provisioning AP: %s\n", deviceName);
    if (!wifiManager.autoConnect(deviceName)) {
        statusError.store(true, std::memory_order_relaxed);
        SERIAL_PRINTF("[D] Wi-Fi provisioning failed to start; rebooting\n");
        delay(1000);
        ESP.restart();
    }
    statusConfigPortalActive.store(false, std::memory_order_relaxed);
    refreshWifiSnapshot();
    // Wi-Fi modem sleep keeps the IDF default here; audioTask widens it to
    // WIFI_PS_NONE only while USB audio is streaming (see kWifiIdleSleepMs).
    WiFi.setAutoReconnect(true);
    xTaskCreateStaticPinnedToCore(audioTask, "sos-audio", sizeof(audioTaskStack),
                                  nullptr, 4, audioTaskStack, &audioTaskControl, 0);
    networkTaskHandle = xTaskCreateStaticPinnedToCore(
        networkTask, "sos-network", sizeof(networkTaskStack), nullptr, 3,
        networkTaskStack, &networkTaskControl, 0);
    encoderTaskHandle = xTaskCreateStaticPinnedToCore(
        encoderTask, "sos-encoder", sizeof(encoderTaskStack), nullptr, 2,
        encoderTaskStack, &encoderTaskControl, 1);
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
            SERIAL_PRINTF("[D] Wi-Fi reset armed; release BOOT to restart provisioning\n");
        }
    } else {
        wifiResetPressedMs = 0;
        if (wifiResetArmed) {
            SERIAL_PRINTF("[D] Clearing saved Wi-Fi credentials\n");
            wifiResetInProgress.store(true, std::memory_order_release);
            WiFi.setAutoReconnect(false);
            delay(100);
            WiFi.disconnect(false, false, 1000);
            delay(100);
            const bool erased = WiFi.eraseAP();
            WiFi.mode(WIFI_OFF);
            SERIAL_PRINTF("[D] %s\n", erased ? "Saved Wi-Fi credentials cleared" : "Failed to clear saved Wi-Fi credentials");
            delay(250);
            ESP.restart();
        }
    }
}
