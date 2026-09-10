// =============================================================================
//  databox control console  —  ESP32 + addressable-LED panel
// =============================================================================
//  The console the portable devices talk to. It has ~402 WS2812 pixels across
//  five physical PANELS, made of reusable board types:
//     bar    = 10-px strip     small = 7-px ring
//     medium = 16-px ring      large = 24-px ring
//     single = one plain LED (GPIO, not addressable)
//
//  Three INDEPENDENT groupings (see BOARDS[]):
//     * panel (1-5)  — physical location, metadata only
//     * chain/pin    — how boards are daisy-chained for wiring (one array per panel)
//     * set   (1-5)  — the logical group ONE portable device drives via its endpoint.
//                      Sets are spread across panels (a device lights the whole
//                      console, not one panel). Every board belongs to exactly one set.
//
//  Default mode: bars flash color patterns/chases, small rings hold a solid
//  colour, medium/large rings do multi-colour comet chases, singles blink
//  randomly. When a device activates its set (/setN/known|unknown), that set's
//  boards LATCH to blue (known) / red (unknown) until changed again (/off ->
//  back to default). Singles are decorative only — not part of the set response.
//
//  SCOPE: this is the scaffold — the board table (with set assignment) plus a
//  bring-up render (each board lit by type so wiring can be verified) and the
//  decorative singles. Real per-type animations and the WiFi AP + /setN endpoints
//  come next. Physical pins below are PROVISIONAL — finalise at wiring time.
// =============================================================================

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "secrets.h"   // WIFI_SSID / WIFI_PASSWORD (git-ignored)

// -----------------------------------------------------------------------------
//  Board types
// -----------------------------------------------------------------------------
enum BoardType : uint8_t { BAR, SMALL, MEDIUM, LARGE };

static uint16_t ledsFor(BoardType t) {
    switch (t) {
        case BAR:    return 10;
        case SMALL:  return 7;
        case MEDIUM: return 16;
        case LARGE:  return 24;
    }
    return 0;
}

// One addressable board: its type, which panel it sits in (1-5), and which
// device set (1-5) drives it. Ordered by panel, then wiring order within a panel.
struct Board {
    BoardType type;
    uint8_t   panel;   // 1-5
    uint8_t   set;     // 1-5
};

static const Board BOARDS[] = {
    // ---- Panel 1: 3 bar, 4 small ----
    { BAR,   1, 1 }, { BAR,   1, 2 }, { BAR,   1, 3 },
    { SMALL, 1, 1 }, { SMALL, 1, 2 }, { SMALL, 1, 3 }, { SMALL, 1, 4 },
    // ---- Panel 2: 4 bar, 1 large, 2 small ----
    { BAR,   2, 4 }, { BAR,   2, 5 }, { BAR,   2, 1 }, { BAR,   2, 2 },
    { LARGE, 2, 1 },
    { SMALL, 2, 5 }, { SMALL, 2, 4 },
    // ---- Panel 3: 6 bar, 1 medium, 3 small ----
    { BAR,   3, 3 }, { BAR,   3, 4 }, { BAR,   3, 5 }, { BAR,   3, 1 }, { BAR, 3, 2 }, { BAR, 3, 3 },
    { MEDIUM,3, 2 },
    { SMALL, 3, 5 }, { SMALL, 3, 1 }, { SMALL, 3, 2 },
    // ---- Panel 4: 6 bar, 3 medium, 2 small ----
    { BAR,   4, 4 }, { BAR,   4, 5 }, { BAR,   4, 1 }, { BAR,   4, 2 }, { BAR, 4, 3 }, { BAR, 4, 4 },
    { MEDIUM,4, 3 }, { MEDIUM,4, 4 }, { MEDIUM,4, 5 },
    { SMALL, 4, 3 }, { SMALL, 4, 4 },
    // ---- Panel 5: 4 bar, 1 small ----
    { BAR,   5, 5 }, { BAR,   5, 1 }, { BAR,   5, 2 }, { BAR,   5, 3 },
    { SMALL, 5, 5 },
};
static const size_t NUM_BOARDS = sizeof(BOARDS) / sizeof(BOARDS[0]);

// -----------------------------------------------------------------------------
//  Physical layout — one chain (data pin) per panel. PROVISIONAL pins.
//  Sizes must match the addressable LED count of each panel's boards.
// -----------------------------------------------------------------------------
#define P1_LEDS  58    // 3*10 + 4*7
#define P2_LEDS  78    // 4*10 + 24 + 2*7
#define P3_LEDS  97    // 6*10 + 16 + 3*7
#define P4_LEDS 122    // 6*10 + 3*16 + 2*7
#define P5_LEDS  47    // 4*10 + 7

static CRGB p1[P1_LEDS], p2[P2_LEDS], p3[P3_LEDS], p4[P4_LEDS], p5[P5_LEDS];
static CRGB* const   PANEL_LEDS[5]  = { p1, p2, p3, p4, p5 };
static const uint16_t PANEL_COUNT[5] = { P1_LEDS, P2_LEDS, P3_LEDS, P4_LEDS, P5_LEDS };
// Provisional data pins (finalise at wiring): P1..P5
#define PIN_P1 13
#define PIN_P2 14
#define PIN_P3 27
#define PIN_P4 26
#define PIN_P5 25

// Single (plain) LEDs — decorative random blink. 4 total: 2 in P1, 2 in P3.
static const uint8_t SINGLE_PINS[] = { 16, 17, 18, 19 };
static const size_t  NUM_SINGLES   = sizeof(SINGLE_PINS) / sizeof(SINGLE_PINS[0]);
static unsigned long singleNext[NUM_SINGLES];
static bool          singleOn[NUM_SINGLES];

// -----------------------------------------------------------------------------
//  Runtime segment map: where each board lives in its panel's LED array.
// -----------------------------------------------------------------------------
struct Seg { CRGB* base; uint16_t off; uint16_t count; BoardType type; uint8_t set; };
static Seg segs[NUM_BOARDS];


// -----------------------------------------------------------------------------
//  Networking — the console is a STATION on the (external) venue AP with a FIXED
//  IP so the devices can reach it; it does NOT host the AP. Exposes the
//  device endpoints /setN/{known,unknown,off} and an operator admin page at "/"
//  for manual override if a portable misbehaves.
// -----------------------------------------------------------------------------
static const IPAddress CONSOLE_IP (192, 168, 50, 10);   // fixed; devices POST here
static const IPAddress GATEWAY    (192, 168, 50,  1);
static const IPAddress SUBNET     (255, 255, 255, 0);
static const IPAddress DNS_SERVER (192, 168, 50,  1);

enum SetState : uint8_t { S_DEFAULT, S_KNOWN, S_UNKNOWN };
static volatile SetState setState[5] = { S_DEFAULT, S_DEFAULT, S_DEFAULT, S_DEFAULT, S_DEFAULT };

AsyncWebServer server(80);

static const char* stateName(SetState s) {
    return s == S_KNOWN ? "known" : s == S_UNKNOWN ? "unknown" : "default";
}

static void applySet(int i, SetState s) {   // latched until changed again
    if (i < 0 || i > 4) return;
    setState[i] = s;
    Serial.printf("Set %d -> %s\n", i + 1, stateName(s));
}

// Operator override page — shows each set's current state with manual controls.
static String adminPage() {
    String h = F("<!doctype html><html><head>"
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<meta http-equiv='refresh' content='3'><title>databox console</title>"
                 "<style>body{font-family:sans-serif;margin:1.2rem;background:#111;color:#eee}"
                 "h1{font-size:1.15rem}.set{margin:.6rem 0;padding:.5rem .7rem;border:1px solid #333;"
                 "border-radius:6px}.st{font-weight:bold}a{display:inline-block;margin:.3rem .3rem 0 0;"
                 "padding:.3rem .7rem;border-radius:4px;text-decoration:none;color:#fff;background:#333}"
                 "</style></head><body><h1>databox console &mdash; manual override</h1>");
    for (int i = 0; i < 5; i++) {
        String n = String(i + 1);
        h += "<div class='set'>Set " + n + " &mdash; <span class='st'>" + stateName(setState[i]) + "</span><br>";
        h += "<a href='/set" + n + "/off'>default</a>";
        h += "<a href='/set" + n + "/known'>known</a>";
        h += "<a href='/set" + n + "/unknown'>unknown</a></div>";
    }
    h += F("</body></html>");
    return h;
}

static void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->send(200, "text/html", adminPage());
    });
    // Device endpoints + admin links (GET from a browser redirects back to "/").
    for (int i = 0; i < 5; i++) {
        String b = "/set" + String(i + 1);
        server.on((b + "/known").c_str(), HTTP_ANY, [i](AsyncWebServerRequest* r) {
            applySet(i, S_KNOWN);
            if (r->method() == HTTP_GET) r->redirect("/"); else r->send(200, "text/plain", "known");
        });
        server.on((b + "/unknown").c_str(), HTTP_ANY, [i](AsyncWebServerRequest* r) {
            applySet(i, S_UNKNOWN);
            if (r->method() == HTTP_GET) r->redirect("/"); else r->send(200, "text/plain", "unknown");
        });
        server.on((b + "/off").c_str(), HTTP_ANY, [i](AsyncWebServerRequest* r) {
            applySet(i, S_DEFAULT);
            if (r->method() == HTTP_GET) r->redirect("/"); else r->send(200, "text/plain", "off");
        });
    }
    server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "not found"); });
    server.begin();
}

static void connectWifi() {
    Serial.printf("WiFi: connecting to \"%s\" ...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    if (!WiFi.config(CONSOLE_IP, GATEWAY, SUBNET, DNS_SERVER)) {
        Serial.println("WiFi: static IP config failed!");
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
        Serial.println("WiFi: not connected (continuing offline).");
    }
}

// -----------------------------------------------------------------------------
//  Animations  (per board type x per set state)
// -----------------------------------------------------------------------------
static const CRGB COLOR_KNOWN  = CRGB(0, 0, 255);      // blue
static const CRGB COLOR_BAD    = CRGB(255, 0, 0);      // red
// small idle blink palette: white / amber / green
static const CRGB SMALL_IDLE[] = { CRGB(130,130,130), CRGB(190,110,0), CRGB(0,150,0) };
static const uint16_t COMET_STEP_MS = 60;   // comet advance interval
static const uint8_t  COMET_FADE    = 64;   // comet tail fade per step
static const uint8_t  TWINKLE_FADE  = 40;   // idle-flash fade per frame

struct Anim {
    SetState lastState;
    bool     blinkOn;      // small idle
    uint32_t blinkNext;
    CRGB     blinkColor;
    uint8_t  head;         // comet head position
    uint32_t cometLast;    // comet step timestamp
};
static Anim anim[NUM_BOARDS];

static inline CRGB* segLeds(size_t i) { return segs[i].base + segs[i].off; }

// small idle: whole ring blinks white/amber/green on a random schedule
static void animSmallIdle(size_t i) {
    Anim& a = anim[i];
    uint32_t now = millis();
    if (now >= a.blinkNext) {
        a.blinkOn = !a.blinkOn;
        if (a.blinkOn) a.blinkColor = SMALL_IDLE[random(3)];
        a.blinkNext = now + (a.blinkOn ? random(90, 350) : random(150, 700));
    }
    fill_solid(segLeds(i), segs[i].count, a.blinkOn ? a.blinkColor : CRGB::Black);
}

// small active: breathing pulse in `base` (per-board phase offset so they differ)
static void animPulse(size_t i, const CRGB& base) {
    uint8_t v = beatsin8(28, 30, 255, 0, (uint8_t)(i * 24));
    CRGB c = base;
    c.nscale8_video(v);
    fill_solid(segLeds(i), segs[i].count, c);
}

// idle flash: colourful random twinkle (medium/large/bar)
static void animTwinkle(size_t i) {
    CRGB* leds = segLeds(i);
    fadeToBlackBy(leds, segs[i].count, TWINKLE_FADE);
    if (random8() < 70) leds[random(segs[i].count)] = CHSV(random8(), 255, 255);
}

// comet chase in `base`: rings wrap circularly, bars sweep left->right & repeat
static void animComet(size_t i, const CRGB& base) {
    Anim& a = anim[i];
    uint32_t now = millis();
    if (now - a.cometLast >= COMET_STEP_MS) {
        a.cometLast = now;
        CRGB* leds = segLeds(i);
        fadeToBlackBy(leds, segs[i].count, COMET_FADE);
        leds[a.head] = base;
        a.head = (a.head + 1) % segs[i].count;
    }
}

// Render one board for the current state of its set.
static void renderBoard(size_t i) {
    SetState st = setState[segs[i].set - 1];
    Anim& a = anim[i];
    if (st != a.lastState) {                 // clean transition
        fill_solid(segLeds(i), segs[i].count, CRGB::Black);
        a.head = 0; a.cometLast = 0; a.blinkOn = false; a.blinkNext = 0;
        a.lastState = st;
    }
    const CRGB& active = (st == S_KNOWN) ? COLOR_KNOWN : COLOR_BAD;
    if (segs[i].type == SMALL) {
        if (st == S_DEFAULT) animSmallIdle(i); else animPulse(i, active);
    } else {                                 // bar / medium / large
        if (st == S_DEFAULT) animTwinkle(i); else animComet(i, active);
    }
}

// -----------------------------------------------------------------------------
//  Setup
// -----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\ndatabox console booting...");

    FastLED.addLeds<WS2812B, PIN_P1, GRB>(p1, P1_LEDS);
    FastLED.addLeds<WS2812B, PIN_P2, GRB>(p2, P2_LEDS);
    FastLED.addLeds<WS2812B, PIN_P3, GRB>(p3, P3_LEDS);
    FastLED.addLeds<WS2812B, PIN_P4, GRB>(p4, P4_LEDS);
    FastLED.addLeds<WS2812B, PIN_P5, GRB>(p5, P5_LEDS);
    FastLED.setBrightness(255);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 9000);  // hard cap ~9A on the 10A supply
    FastLED.clear(true);

    // Build the segment map: walk boards per panel, accumulating offsets.
    uint16_t poff[5] = { 0, 0, 0, 0, 0 };
    for (size_t i = 0; i < NUM_BOARDS; i++) {
        uint8_t p = BOARDS[i].panel - 1;
        uint16_t n = ledsFor(BOARDS[i].type);
        segs[i] = { PANEL_LEDS[p], poff[p], n, BOARDS[i].type, BOARDS[i].set };
        poff[p] += n;
    }
    // Sanity-check each panel's boards exactly fill its array.
    for (int p = 0; p < 5; p++) {
        Serial.printf("Panel %d: %u/%u LEDs used%s\n", p + 1, poff[p], PANEL_COUNT[p],
                      poff[p] == PANEL_COUNT[p] ? "" : "  <-- MISMATCH!");
    }
    Serial.printf("Boards: %u addressable + %u singles\n", (unsigned)NUM_BOARDS, (unsigned)NUM_SINGLES);

    // Singles.
    randomSeed(analogRead(A0) ^ micros());
    for (size_t i = 0; i < NUM_SINGLES; i++) {
        pinMode(SINGLE_PINS[i], OUTPUT);
        singleOn[i] = false;
        singleNext[i] = millis() + random(100, 800);
    }

    // Per-board animation state.
    for (size_t i = 0; i < NUM_BOARDS; i++) {
        anim[i].lastState  = S_DEFAULT;
        anim[i].blinkOn    = false;
        anim[i].blinkNext  = millis() + random(0, 500);
        anim[i].blinkColor = SMALL_IDLE[0];
        anim[i].head       = random(segs[i].count);   // desync comets
        anim[i].cometLast  = 0;
    }

    connectWifi();
    setupWebServer();
    Serial.println("HTTP up: /setN/{known,unknown,off} + admin at /");
    Serial.println("Ready (scaffold: default = bring-up by type; known=blue, unknown=red).");
}

// -----------------------------------------------------------------------------
//  Loop  (scaffold render: each board solid by type; singles blink)
// -----------------------------------------------------------------------------
static void updateSingles() {
    unsigned long now = millis();
    for (size_t i = 0; i < NUM_SINGLES; i++) {
        if (now < singleNext[i]) continue;
        singleOn[i] = !singleOn[i];
        digitalWrite(SINGLE_PINS[i], singleOn[i] ? HIGH : LOW);
        singleNext[i] = now + (singleOn[i] ? random(80, 300) : random(150, 900));
    }
}

void loop() {
    for (size_t i = 0; i < NUM_BOARDS; i++) renderBoard(i);
    FastLED.show();

    updateSingles();
    delay(20);
}
