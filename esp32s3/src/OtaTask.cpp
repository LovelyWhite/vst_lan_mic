#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <cassert>
#include <cctype>
#include <esp_heap_caps.h>

#include "OtaTask.h"
#include "Serial.h"
#include "Status.h"
#include "Version.h"

namespace lanmic {
namespace {

// shortcut: plain HTTP, no signature check — a MITM on the LAN could serve a
// fake image. TLS is not an option here: mbedTLS allocates its ~50 KB of
// handshake buffers from internal DRAM only (MBEDTLS_EXTERNAL_MEM_ALLOC is
// off in the framework sdkconfig) and only ~9 KB are free while audio runs.
constexpr const char *kManifestUrl =
    "http://lan-mic.suxitech.cn/firmware/manifest.json";
constexpr const char *kFirmwareUrl =
    "http://lan-mic.suxitech.cn/firmware/firmware.bin";
constexpr size_t kManifestMaxBytes = 4096;
constexpr size_t kChunkBytes = 4096;

// Static: otaBootUpdate runs on the 8 KB loopTask stack, where a 4 KB local
// array would risk overflow. Only the boot thread ever touches it.
uint8_t firmwareChunk[kChunkBytes];

// Leading "v?MAJOR.MINOR.PATCH" triplet. Returns the digit/dot count
// consumed, 0 when the text does not start with a number, so an error page
// can never read as a version. Missing trailing segments stay 0.
size_t parseVersion(const char *s, uint32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    const char *p = (*s == 'v' || *s == 'V') ? s + 1 : s;
    size_t consumed = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (!isdigit(static_cast<unsigned char>(*p))) break;
        const char *start = p;
        while (isdigit(static_cast<unsigned char>(*p)))
            out[i] = out[i] * 10u + static_cast<uint32_t>(*p++ - '0');
        consumed += static_cast<size_t>(p - start);
        if (*p != '.') break;
        ++p;
        ++consumed;
    }
    return consumed;
}

int compareVersion(const uint32_t a[3], const uint32_t b[3]) {
    for (size_t i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

void selfCheck() {
    uint32_t a[3], b[3];
    assert(parseVersion("v1.0.0", a) == 5 && a[0] == 1);
    assert(parseVersion("1.2.10", b) == 6 && b[1] == 2 && b[2] == 10);
    assert(compareVersion(a, b) < 0 && compareVersion(b, a) > 0);
    assert(parseVersion("1.0", a) == 3 && a[1] == 0 && a[2] == 0);
    assert(parseVersion("2", b) == 1 && b[0] == 2 && b[1] == 0);
    assert(parseVersion("", a) == 0 && parseVersion("<html>", b) == 0);
    uint32_t c[3], d[3];
    assert(parseVersion("1.0.0", c) == 5 && parseVersion("v1.0.0", d) == 5);
    assert(compareVersion(c, d) == 0);
    assert(parseVersion("2.0.0", d) == 5 && compareVersion(c, d) < 0);
}

// The build script keeps exactly one "version" key in the manifest, so a
// find-and-parse is enough; a JSON library would be overkill for one field.
bool fetchManifest(WiFiClient &sock, uint32_t remote[3]) {
    HTTPClient http;
    http.setConnectTimeout(10000);
    if (!http.begin(sock, kManifestUrl)) return false;
    bool ok = false;
    const int code = http.GET();
    const int len = http.getSize();
    if (code < 0) {
        // http's own error codes: DNS/connection failures come back here.
        SERIAL_PRINTF("[D] ota: manifest %s, heap=%u int=%u\n",
                      http.errorToString(code).c_str(), unsigned(ESP.getFreeHeap()),
                      unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    } else if (code != HTTP_CODE_OK || len <= 0 || size_t(len) > kManifestMaxBytes) {
        SERIAL_PRINTF("[D] ota: manifest http=%d len=%d\n", code, len);
    } else {
        const String body = http.getString();
        const int pos = body.indexOf("\"version\"");
        if (pos >= 0) {
            const int quote = body.indexOf('"', body.indexOf(':', pos + 9) + 1);
            const int end = quote > 0 ? body.indexOf('"', quote + 1) : -1;
            if (quote > 0 && end > quote)
                ok = parseVersion(body.substring(quote + 1, end).c_str(), remote) > 0;
        }
        if (!ok) SERIAL_PRINTF("[D] ota: manifest has no version field\n");
    }
    http.end();
    return ok;
}

bool downloadFirmware(WiFiClient &sock) {
    HTTPClient http;
    http.setConnectTimeout(10000);
    if (!http.begin(sock, kFirmwareUrl)) return false;
    bool ok = false;
    const int code = http.GET();
    const int len = http.getSize();
    if (code == HTTP_CODE_OK && len > 0 && ESP.getFreeSketchSpace() >= len &&
        Update.begin(size_t(len), U_FLASH)) {
        NetworkClient *stream = http.getStreamPtr();
        size_t got = 0;
        uint32_t stalls = 0;
        int lastPct = -10;
        ok = true;
        while (got < size_t(len)) {
            if (!stream->available()) {
                // available() only returns 0 between TCP segments; 5 s of
                // silence with bytes still pending means the link died.
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++stalls > 500) { ok = false; break; }
                continue;
            }
            stalls = 0;
            const int n = stream->readBytes(firmwareChunk,
                sizeof(firmwareChunk) < size_t(len) - got ? sizeof(firmwareChunk) : size_t(len) - got);
            if (n <= 0 || Update.write(firmwareChunk, size_t(n)) != size_t(n)) {
                ok = false;
                break;
            }
            got += size_t(n);
            const int pct = int(got * 100 / size_t(len));
            if (pct >= lastPct + 10) {
                lastPct = pct;
                SERIAL_PRINTF("[D] ota: wrote %d%%\n", pct);
            }
        }
        ok = ok && got == size_t(len) && Update.end();
        if (!ok) Update.abort();
    }
    http.end();
    return ok;
}

void checkOnce(WiFiClient &sock, const uint32_t local[3]) {
    uint32_t remote[3];
    if (!fetchManifest(sock, remote)) {
        SERIAL_PRINTF("[D] ota: manifest fetch failed\n");
        return;
    }
    if (compareVersion(remote, local) <= 0) return;
    statusOtaUpdating.store(true, std::memory_order_relaxed);
    SERIAL_PRINTF("[D] ota: %u.%u.%u -> %u.%u.%u, downloading\n",
                  local[0], local[1], local[2], remote[0], remote[1], remote[2]);
    if (!downloadFirmware(sock)) {
        SERIAL_PRINTF("[D] ota: download failed (err=%u)\n", unsigned(Update.getError()));
        statusOtaUpdating.store(false, std::memory_order_relaxed);
        return;
    }
    // No audio-window wait: setup() has not started any streaming task yet.
    SERIAL_PRINTF("[D] ota: update staged, rebooting\n");
    ESP.restart();
}

} // namespace

void otaBootUpdate() {
    selfCheck();
    uint32_t local[3];
    parseVersion(LANMIC_VERSION, local);
    WiFiClient sock;
    checkOnce(sock, local);
    // One check per boot; a missed update is picked up at the next power cycle.
}

} // namespace lanmic
