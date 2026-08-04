// =============================================================================
//  databox POC receiver  —  ESP32
// =============================================================================
//  A throwaway demo target for the databox RFID reader. It shows that a tape
//  insert/read on the reader can drive an external object over WiFi.
//
//  This device:
//    - Hosts a WiFi access point (SSID/password from secrets.h — the same
//      credentials the databox reader is configured to join).
//    - Sits at a static AP IP (192.168.50.1) so the reader can reach it. The
//      reader uses 192.168.50.10 with gateway 192.168.50.1, so this unit IS
//      that gateway.
//    - Drives a single 16-LED WS2812B ring (no other hardware).
//    - Exposes three HTTP endpoints (GET or POST):
//        /known    -> LEDs pulse through shades of blue
//        /unknown  -> flash red 6 times, then hold solid red
//        /off      -> return to idle (LEDs pulse through orange/yellow)
//    - Starts up in idle (pulsing orange/yellow).
//
//  Springtrap lesson: the async HTTP handlers NEVER touch FastLED. They just
//  enqueue the requested mode; loop() drains the queue and owns every LED op,
//  so there's no concurrent access from the AsyncTCP task.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h>
#include "secrets.h"   // AP_SSID / AP_PASSWORD (git-ignored)

// -----------------------------------------------------------------------------
//  Hardware
// -----------------------------------------------------------------------------
#define PIN_RING16   13
#define NUM_LEDS     16
static CRGB ring[NUM_LEDS];

// -----------------------------------------------------------------------------
//  Network — static AP so the reader always finds us at a known address.
// -----------------------------------------------------------------------------
static const IPAddress AP_IP      (192, 168, 50, 1);
static const IPAddress AP_GATEWAY (192, 168, 50, 1);
static const IPAddress AP_SUBNET  (255, 255, 255, 0);
static const uint8_t   WIFI_CHANNEL = 1;

AsyncWebServer server(80);

// -----------------------------------------------------------------------------
//  Modes
// -----------------------------------------------------------------------------
enum Mode { MODE_IDLE, MODE_KNOWN, MODE_UNKNOWN };
static Mode currentMode = MODE_IDLE;

// HTTP handlers (async task) enqueue a mode; loop() applies it.
static QueueHandle_t modeQueue = NULL;

// Unknown-mode flash state (owned by loop()).
static const uint8_t  UNKNOWN_FLASHES  = 6;
static const uint32_t UNKNOWN_FLASH_MS  = 120;
static bool     unkFlashing = false;
static bool     unkOn       = false;
static uint8_t  unkCount    = 0;
static uint32_t unkLast     = 0;

// -----------------------------------------------------------------------------
//  Mode application + animations (all in loop() context)
// -----------------------------------------------------------------------------
static void applyMode(Mode m) {
    currentMode = m;
    switch (m) {
        case MODE_IDLE:
            Serial.println("Mode: IDLE (pulsing orange/yellow)");
            break;   // animated continuously in updateIdle()
        case MODE_KNOWN:
            Serial.println("Mode: KNOWN (pulsing blue)");
            break;   // animated continuously in updateKnown()
        case MODE_UNKNOWN:
            unkFlashing = true;
            unkOn       = true;
            unkCount    = 0;
            unkLast     = millis();
            fill_solid(ring, NUM_LEDS, CRGB::Red);
            FastLED.show();
            Serial.println("Mode: UNKNOWN (flash x6 then solid red)");
            break;
    }
}

static void updateIdle() {
    // Gentle idle lantern glow: hue drifts orange -> yellow while brightness
    // breathes slowly.
    uint8_t v = beatsin8(20, 25, 170);    // slow, moderate brightness
    uint8_t h = beatsin8(10, 24, 64);     // orange (24) -> yellow (64)
    fill_solid(ring, NUM_LEDS, CHSV(h, 255, v));
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
    if (!unkFlashing) return;                       // holding solid red
    if (millis() - unkLast < UNKNOWN_FLASH_MS) return;
    unkLast = millis();
    unkOn = !unkOn;
    if (unkOn) {
        fill_solid(ring, NUM_LEDS, CRGB::Red);
    } else {
        fill_solid(ring, NUM_LEDS, CRGB::Black);
        if (++unkCount >= UNKNOWN_FLASHES) {
            unkFlashing = false;
            fill_solid(ring, NUM_LEDS, CRGB::Red);  // settle on solid red
        }
    }
    FastLED.show();
}

// -----------------------------------------------------------------------------
//  Web server
// -----------------------------------------------------------------------------
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
                  "databox POC receiver\nendpoints (GET or POST): /known /unknown /off\n");
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
    Serial.println("\ndatabox POC receiver booting...");

    FastLED.addLeds<WS2812B, PIN_RING16, GRB>(ring, NUM_LEDS);
    FastLED.setBrightness(255);
    fill_solid(ring, NUM_LEDS, CRGB::Black);   // idle glow takes over in loop()
    FastLED.show();

    modeQueue = xQueueCreate(8, sizeof(Mode));

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASSWORD, WIFI_CHANNEL);
    Serial.printf("AP \"%s\" up at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    setupWebServer();
    Serial.println("HTTP server up. Endpoints: /known /unknown /off");
}

void loop() {
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
