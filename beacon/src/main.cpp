// =============================================================================
//  databox beacon  —  ESP32
// =============================================================================
//  A small standalone "beacon" that mirrors the state of the whole room. It
//  started life as the POC receiver for a single portable reader; it now takes
//  its cue from the CONTROL CONSOLE, which pushes the room's overall state to
//  every beacon at once:
//
//    /unknown  -> any set on the console is in error (or a board has failed)
//    /known    -> ALL five sets are known (the room has solved it)
//    /off      -> the console is back to default
//
//  This device:
//    - Joins the venue WiFi as a CLIENT (SSID/password from secrets.h). It no
//      longer hosts an access point.
//    - Takes a STATIC IP stored in NVS so each unit can be addressed without
//      recompiling: set it over serial with `set ip 192.168.50.51`. With no IP
//      configured it falls back to DHCP and prints the address it got.
//    - Drives a single 40-px WS2812B ring/strip (no other hardware).
//    - Exposes the same three endpoints (GET or POST):
//        /known    -> LEDs pulse through shades of blue
//        /unknown  -> urgent red breath
//        /off      -> return to idle (slow green pulse)
//
//  Springtrap lesson: the async HTTP handlers NEVER touch FastLED. They just
//  enqueue the requested mode; loop() drains the queue and owns every LED op,
//  so there's no concurrent access from the AsyncTCP task.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h>
#include <Preferences.h>
#include "secrets.h"   // WIFI_SSID / WIFI_PASSWORD (git-ignored)

// -----------------------------------------------------------------------------
//  Hardware
// -----------------------------------------------------------------------------
#define PIN_RING     13
#define NUM_LEDS     40
static CRGB ring[NUM_LEDS];

// -----------------------------------------------------------------------------
//  Network — a client on the venue AP, at a static IP held in NVS so the same
//  binary can be flashed to every beacon and addressed individually afterwards.
// -----------------------------------------------------------------------------
static const IPAddress GATEWAY    (192, 168, 50,  1);
static const IPAddress SUBNET     (255, 255, 255, 0);
static const IPAddress DNS_SERVER (192, 168, 50,  1);

static Preferences prefs;
static const char* PREF_NS     = "beacon";
static const char* PREF_KEY_IP = "ip";
static String staticIp;          // "" = DHCP

AsyncWebServer server(80);

// -----------------------------------------------------------------------------
//  Modes
// -----------------------------------------------------------------------------
enum Mode { MODE_IDLE, MODE_KNOWN, MODE_UNKNOWN };
static Mode currentMode = MODE_IDLE;

// HTTP handlers (async task) enqueue a mode; loop() applies it.
static QueueHandle_t modeQueue = NULL;

// Idle glow: a slow green pulse. Hue 96 is FastLED's pure green; drop toward 85
// for a yellower green, up toward 110 for a cooler one.
static const uint8_t IDLE_HUE   = 96;
static const uint8_t IDLE_BPM   = 20;   // pulse rate
static const uint8_t IDLE_MIN_V = 25;   // dimmest point of the breath
static const uint8_t IDLE_MAX_V = 255;  // brightest point

// Unknown mode: an urgent red breath — deliberately faster than the idle pulse
// so the room reads it as agitation.
static const uint8_t  ALERT_HUE   = 0;    // red
static const uint8_t  ALERT_BPM   = 50;   // vs IDLE_BPM 20 — noticeably quicker
static const uint8_t  ALERT_MIN_V = 60;   // never fully dark; it stays a red presence
static const uint8_t  ALERT_MAX_V = 255;

// -----------------------------------------------------------------------------
//  Stored config (NVS)
// -----------------------------------------------------------------------------
static void loadConfig() {
    prefs.begin(PREF_NS, false);
    staticIp = prefs.isKey(PREF_KEY_IP) ? prefs.getString(PREF_KEY_IP, "") : String("");
    prefs.end();
}

static void saveStaticIp(const String& ip) {
    prefs.begin(PREF_NS, false);
    prefs.putString(PREF_KEY_IP, ip);
    prefs.end();
    staticIp = ip;
}

static void clearStaticIp() {
    prefs.begin(PREF_NS, false);
    prefs.remove(PREF_KEY_IP);
    prefs.end();
    staticIp = "";
}

static void printConfig() {
    Serial.println(F("---- config ----"));
    Serial.print(F("Static IP: "));
    Serial.println(staticIp.length() ? staticIp : String("(unset — using DHCP)"));
    Serial.print(F("WiFi:      "));
    Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("(not connected)"));
    Serial.print(F("Mode:      "));
    Serial.println(currentMode == MODE_KNOWN ? "known" : currentMode == MODE_UNKNOWN ? "unknown" : "idle");
    Serial.println(F("----------------"));
}

// Parse one serial command: set ip <addr> | clear ip | show config.
static void handleConfigCommand(String cmd) {
    cmd.trim();
    if (cmd.startsWith("set ip ")) {
        String ip = cmd.substring(7);
        ip.trim();
        IPAddress probe;
        if (!probe.fromString(ip)) {
            Serial.print(F("Not a valid IPv4 address: "));
            Serial.println(ip);
            return;
        }
        saveStaticIp(ip);
        Serial.print(F("Static IP set to: "));
        Serial.println(staticIp);
        Serial.println(F("Reboot (or power-cycle) for it to take effect."));
    } else if (cmd == "clear ip") {
        clearStaticIp();
        Serial.println(F("Static IP cleared — will use DHCP after a reboot."));
    } else if (cmd == "show config" || cmd == "show") {
        printConfig();
    } else if (cmd == "help" || cmd == "?") {
        Serial.println(F("Commands: set ip <addr> | clear ip | show config"));
    } else {
        Serial.print(F("Unknown command: "));
        Serial.println(cmd);
        Serial.println(F("Try: set ip <addr> | clear ip | show config"));
    }
}

// Non-blocking: accumulate a line from serial and dispatch it on newline.
static void pollSerialConfig() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (line.length()) handleConfigCommand(line);
            line = "";
        } else if (line.length() < 120) {
            line += c;
        }
    }
}

// -----------------------------------------------------------------------------
//  Mode application + animations (all in loop() context)
// -----------------------------------------------------------------------------
static void applyMode(Mode m) {
    currentMode = m;
    switch (m) {
        case MODE_IDLE:
            Serial.println("Mode: IDLE (green pulse)");
            break;   // animated continuously in updateIdle()
        case MODE_KNOWN:
            Serial.println("Mode: KNOWN (pulsing blue)");
            break;   // animated continuously in updateKnown()
        case MODE_UNKNOWN:
            Serial.println("Mode: UNKNOWN (red breath)");
            break;   // animated continuously in updateUnknown()
    }
}

static void updateIdle() {
    // Gentle idle glow: a slow green pulse — the room is calm and nominal.
    uint8_t v = beatsin8(IDLE_BPM, IDLE_MIN_V, IDLE_MAX_V);
    fill_solid(ring, NUM_LEDS, CHSV(IDLE_HUE, 255, v));
    FastLED.show();
}

static void updateKnown() {
    // Pulse through shades of blue: brightness breathes while the hue drifts
    // across the blue range.
    uint8_t v = beatsin8(30, 40, 255);    // brightness ~30 BPM
    uint8_t h = beatsin8(15, 150, 175);   // hue wanders within the blues
    fill_solid(ring, NUM_LEDS, CHSV(h, 255, v));
    FastLED.show();
}

static void updateUnknown() {
    // Urgent red breath — same shape as the idle pulse, just faster and red.
    uint8_t v = beatsin8(ALERT_BPM, ALERT_MIN_V, ALERT_MAX_V);
    fill_solid(ring, NUM_LEDS, CHSV(ALERT_HUE, 255, v));
    FastLED.show();
}

// -----------------------------------------------------------------------------
//  WiFi + web server
// -----------------------------------------------------------------------------
static void connectWifi() {
    Serial.printf("WiFi: connecting to \"%s\" ...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);

    if (staticIp.length()) {
        IPAddress ip;
        if (ip.fromString(staticIp)) {
            if (!WiFi.config(ip, GATEWAY, SUBNET, DNS_SERVER)) {
                Serial.println("WiFi: static IP config failed — falling back to DHCP.");
            }
        } else {
            Serial.printf("WiFi: stored IP \"%s\" is invalid — using DHCP.\n", staticIp.c_str());
        }
    } else {
        Serial.println("WiFi: no static IP set — using DHCP. Set one with 'set ip <addr>'.");
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi: connected, IP ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("WiFi: not connected (continuing offline — lights still run).");
    }
}

static void queueMode(Mode m) {
    if (modeQueue) xQueueSend(modeQueue, &m, 0);
}

static void setupWebServer() {
    server.on("/known", HTTP_ANY, [](AsyncWebServerRequest *req) {
        queueMode(MODE_KNOWN);
        req->send(200, "text/plain", "known");
    });
    server.on("/unknown", HTTP_ANY, [](AsyncWebServerRequest *req) {
        queueMode(MODE_UNKNOWN);
        req->send(200, "text/plain", "unknown");
    });
    server.on("/off", HTTP_ANY, [](AsyncWebServerRequest *req) {
        queueMode(MODE_IDLE);   // "off" endpoint returns to the idle glow
        req->send(200, "text/plain", "off");
    });
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain",
                  "databox beacon\nendpoints (GET or POST): /known /unknown /off\n");
    });
    server.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "not found");
    });
    server.begin();
}

// -----------------------------------------------------------------------------
//  Setup / loop
// -----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\ndatabox beacon booting...");

    FastLED.addLeds<WS2812B, PIN_RING, GRB>(ring, NUM_LEDS);
    FastLED.setBrightness(255);
    fill_solid(ring, NUM_LEDS, CRGB::Black);   // idle glow takes over in loop()
    FastLED.show();

    modeQueue = xQueueCreate(8, sizeof(Mode));

    loadConfig();
    connectWifi();
    setupWebServer();
    Serial.println("HTTP server up. Endpoints: /known /unknown /off");
    Serial.println("Config: set ip <addr> | clear ip | show config");
}

void loop() {
    pollSerialConfig();

    Mode m;
    if (modeQueue && xQueueReceive(modeQueue, &m, 0) == pdTRUE) {
        applyMode(m);
    }
    switch (currentMode) {
        case MODE_KNOWN:   updateKnown();   break;
        case MODE_UNKNOWN: updateUnknown(); break;
        case MODE_IDLE:    updateIdle();    break;
    }
    delay(5);
}
