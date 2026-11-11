#include "Console.h"

#include <WiFi.h>
#include <Preferences.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "DeviceIdentity.h"
#include "Protocol.h"
#include "Serial.h"

namespace lanmic {

char deviceName[33]{};
bool enableEpIn = true, enableEpOut = true;

namespace {

constexpr char kNvsNamespace[] = "lanmic";
// shortcut: the key name is legacy (it stored the provisioning SSID before the
// field became the device display name); renaming it would lose saved values.
constexpr char kKeyName[] = "prov_ssid";
constexpr char kKeyEpIn[] = "ep_in";
constexpr char kKeyEpOut[] = "ep_out";
constexpr size_t kLineMax = 64;

bool parseSwitch(const char* text, bool& value) {
    if (!text || !*text) return false;
    if (!strcasecmp(text, "0") || !strcasecmp(text, "off") ||
        !strcasecmp(text, "false")) {
        value = false;
        return true;
    }
    if (!strcasecmp(text, "1") || !strcasecmp(text, "on") ||
        !strcasecmp(text, "true")) {
        value = true;
        return true;
    }
    return false;
}

// Field bodies: the | separator and backslash are backslash-escaped,
// everything else (UTF-8 included) passes through. dst needs 3 bytes.
// shortcut: console input is printable ASCII, so control bytes only ever
// arrive from an over-the-air Wi-Fi SSID; revisit if parsing must be strict.
size_t pipeEscapeChar(char* dst, unsigned char c) {
    if (c == '|' || c == '\\') {
        dst[0] = '\\';
        dst[1] = char(c);
        return 2;
    }
    dst[0] = char(c);
    return 1;
}

// One pipe-delimited line per reply: "OK|cmd|f1|f2", "OKR|..." (needs a
// reboot) or "ERR|cmd|reason". Fields are positional with a fixed order per
// command, so the key names can go and the lines stay short; scripts tell
// replies apart from the [D] diagnostics that the other tasks keep printing
// by the OK/OKR/ERR prefix. Field order is documented in README.
struct Reply {
    Print& out;

    Reply(Print& o, const char* kind, const char* cmd) : out(o) {
        out.print(kind);
        field(cmd);
    }
    void field(const char* value) {
        char esc[3];
        out.print('|');
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value);
             *p; ++p) {
            out.write(reinterpret_cast<const uint8_t*>(esc), pipeEscapeChar(esc, *p));
        }
    }
    void num(long long value) {
        out.print('|');
        out.printf("%lld", value);
    }
    void boolean(bool value) {
        out.print(value ? "|1" : "|0");
    }
    void close() {
        out.println();
    }
};

// shortcut: the grammar is static, so the command list is spelled out as
// text; build it with Reply if the commands ever become data.
void printHelp(Print& out) {
    Reply r{out, "OK", "help"};
    r.field("help,info,name,epin,epout,erase,reboot");
    r.close();
}

void printInfo(Print& out, const char* cmd) {
    const bool connected = WiFi.status() == WL_CONNECTED;
    Reply r{out, "OK", cmd};
    r.field(identity);
    r.field(usbSerial);
    r.num(bootSession);
    r.num(sos::kVersion);
    r.num(millis());
    if (connected) {
        r.field(WiFi.localIP().toString().c_str());
        r.num(WiFi.RSSI());
        r.field(WiFi.SSID().c_str());
        r.field(WiFi.gatewayIP().toString().c_str());
    } else { // empty fields keep the positional layout stable
        r.field("");
        r.field("");
        r.field("");
        r.field("");
    }
    r.field(deviceName);
    r.boolean(enableEpIn);
    r.boolean(enableEpOut);
    r.close();
}

void replyError(Print& out, const char* cmd, const char* reason) {
    Reply r{out, "ERR", cmd};
    r.field(reason);
    r.close();
}

void handleLine(Print& out, char* line, Preferences& prefs) {
    while (*line == ' ') ++line;
    char* argument = strchr(line, ' ');
    if (argument) *argument++ = '\0';

    if (!*line) {
        return;
    } else if (!strcasecmp(line, "help")) {
        printHelp(out);
    } else if (!strcasecmp(line, "info")) {
        printInfo(out, line);
    } else if (!strcasecmp(line, "name")) {
        if (!argument || !*argument) {
            Reply r{out, "OK", line};
            r.field(deviceName);
            r.close();
            return;
        }
        size_t length = strlen(argument);
        while (length && argument[length - 1] == ' ') argument[--length] = '\0';
        // '|' is the unescaped field separator of the UDP Offer payload.
        if (length == 0 || length >= sizeof(deviceName) || strchr(argument, '|')) {
            replyError(out, line, "name needs 1-32 bytes, no |");
            return;
        }
        if (!prefs.putString(kKeyName, argument)) {
            replyError(out, line, "NVS write failed");
            return;
        }
        snprintf(deviceName, sizeof(deviceName), "%s", argument);
        Reply r{out, "OKR", line};
        r.field(deviceName);
        r.close();
    } else if (!strcasecmp(line, "epin") || !strcasecmp(line, "epout")) {
        const bool isInput = strcasecmp(line, "epin") == 0;
        bool value = false;
        if (!parseSwitch(argument, value)) {
            Reply r{out, "OK", line};
            r.boolean(isInput ? enableEpIn : enableEpOut);
            r.close();
            return;
        }
        if (!prefs.putBool(isInput ? kKeyEpIn : kKeyEpOut, value)) {
            replyError(out, line, "NVS write failed");
            return;
        }
        if (isInput) enableEpIn = value; else enableEpOut = value;
        Reply r{out, "OKR", line};
        r.boolean(value);
        r.close();
    } else if (!strcasecmp(line, "erase")) {
        // Saved Wi-Fi credentials only; the device name and the USB
        // endpoint switches keep their NVS values. Same driver sequence as the
        // BOOT-button reset in loop(), but there is no immediate reboot here,
        // so the in-RAM STA config must go too: a later reconnect would store
        // it back into NVS and the credentials would come alive again.
        WiFi.setAutoReconnect(false);
        if (!WiFi.disconnect(false, true, 1000) || !WiFi.eraseAP()) {
            replyError(out, line, "Wi-Fi credential erase failed");
            return;
        }
        Reply r{out, "OKR", line};
        r.close();
    } else if (!strcasecmp(line, "reboot")) {
        Reply r{out, "OK", line};
        r.close();
        out.flush();
        delay(50);
        ESP.restart();
    } else {
        replyError(out, line, "unknown command");
    }
}

struct LineInput {
    Stream& in;
    Print& out;
    Preferences& prefs;
    char buf[kLineMax]{};
    size_t len = 0;
    bool dropping = false;

    void poll() {
        int c;
        while ((c = in.read()) >= 0) {
            // shortcut: printable ASCII only, a non-ASCII SSID has to be set
            // from the provisioning web page; revisit if UTF-8 input is needed.
            if (c == '\n' || c == '\r') {
                buf[len] = '\0';
                if (len) handleLine(out, buf, prefs);
                len = 0;
                dropping = false;
            } else if (!dropping && c >= ' ' && c <= '~') {
                if (len + 1 < kLineMax) buf[len++] = char(c);
                else dropping = true;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
};

// shortcut: assert-based self-check for the parser and the field encoder; move
// to a host test if the command grammar grows.
void selfCheck() {
    bool value = false;
    assert(parseSwitch("0", value) && !value);
    assert(parseSwitch("on", value) && value);
    assert(parseSwitch("TRUE", value) && value);
    assert(!parseSwitch("2", value));
    assert(!parseSwitch("", value));
    assert(!parseSwitch(nullptr, value));
    char esc[4];
    assert(pipeEscapeChar(esc, '|') == 2 && memcmp(esc, "\\|", 2) == 0);
    assert(pipeEscapeChar(esc, '\\') == 2 && memcmp(esc, "\\\\", 2) == 0);
    assert(pipeEscapeChar(esc, 'a') == 1 && esc[0] == 'a');
    assert(pipeEscapeChar(esc, 0xC3) == 1 && esc[0] == char(0xC3));
}

} // namespace

void loadDeviceConfig() {
    deviceName[0] = '\0';
    enableEpIn = enableEpOut = true;
    Preferences prefs;
    if (prefs.begin(kNvsNamespace, true)) {
        prefs.getString(kKeyName, deviceName, sizeof(deviceName));
        enableEpIn = prefs.getBool(kKeyEpIn, true);
        enableEpOut = prefs.getBool(kKeyEpOut, true);
        prefs.end();
    }
    if (!deviceName[0]) {
        snprintf(deviceName, sizeof(deviceName), "%s", identity);
    }
}

void consoleTask(void*) {
    selfCheck();
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace)) {
        SERIAL_PRINTF("[D] Console NVS open failed; settings are read-only\n");
    }
    // Replies go through cdcSerial so a host that stops draining CDC IN
    // (audio playback) cannot trap this task in USBCDC::write()'s spin loop.
    LineInput usbInput{USBSerial, cdcSerial, prefs};
    for (;;) {
        usbInput.poll();
        // 1 s keeps command latency plus a stalled reply well inside the web
        // page's 15 s timeout; polling an empty CDC RX queue costs nothing.
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

} // namespace lanmic
