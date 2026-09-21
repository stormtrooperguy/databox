// =============================================================================
//  databox control console  —  ESP32 + addressable-LED panel
// =============================================================================
//  The console the portable devices talk to. It has 388 WS2812 pixels across
//  five physical PANELS, made of reusable board types:
//     bar    = 8-px strip      small = 7-px ring
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
//  Default mode: small rings blink white/amber/green; medium, large and bars
//  run a peak-level meter in their panel's colour; medium/large rings run a
//  pressure gauge; singles blink randomly. When a device activates its
//  set (/setN/known|unknown), that set's boards LATCH to blue (known) / red
//  (unknown) until changed again (/off -> back to default). Singles are
//  decorative only — not part of the set response.
//
//  FAILURE mode (operator, GET/POST /fail): ~30% of the boards — picked per set,
//  so every device has something to fix — flash red and override their normal
//  animation. A board stays failed until its own set receives /known (the matching
//  device's good cartridge, or the admin page's manual "known"). Unknown/off do
//  not clear it.
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
        case BAR:    return 8;    // as-built bars are 8 px (ordered 10, received 8)
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
    // ---- Panel 1: 5 bar, 4 small (the last 2 bars sit at the END of the chain) ----
    { BAR,   1, 1 }, { BAR,   1, 2 }, { BAR,   1, 3 },
    { SMALL, 1, 1 }, { SMALL, 1, 2 }, { SMALL, 1, 3 }, { SMALL, 1, 4 },
    { BAR,   1, 5 }, { BAR,   1, 3 },
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
    // ---- Panel 5: 6 bar, 1 small (the last 2 bars sit at the END of the chain) ----
    { BAR,   5, 5 }, { BAR,   5, 1 }, { BAR,   5, 2 }, { BAR,   5, 3 },
    { SMALL, 5, 5 },
    { BAR,   5, 4 }, { BAR,   5, 2 },
};
static const size_t NUM_BOARDS = sizeof(BOARDS) / sizeof(BOARDS[0]);

// -----------------------------------------------------------------------------
//  Physical layout — one chain (data pin) per panel. PROVISIONAL pins.
//  Sizes must match the addressable LED count of each panel's boards.
// -----------------------------------------------------------------------------
#define P1_LEDS  68    // 5*8 + 4*7
#define P2_LEDS  70    // 4*8 + 24 + 2*7
#define P3_LEDS  85    // 6*8 + 16 + 3*7
#define P4_LEDS 110    // 6*8 + 3*16 + 2*7
#define P5_LEDS  55    // 6*8 + 7

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

// Failure mode. failed[b] = board b is flashing red until its set gets /known.
// The web handlers only raise the request flags below; loop() does the work.
static volatile bool failed[NUM_BOARDS];
static volatile bool pendingFail = false;         // operator hit /fail
static volatile bool pendingFix[5] = { false, false, false, false, false };  // set got /known

AsyncWebServer server(80);

static const char* stateName(SetState s) {
    return s == S_KNOWN ? "known" : s == S_UNKNOWN ? "unknown" : "default";
}

static void applySet(int i, SetState s) {   // latched until changed again
    if (i < 0 || i > 4) return;
    setState[i] = s;
    if (s == S_KNOWN) pendingFix[i] = true;  // a known cartridge repairs this set's failures
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
                 "a.fail{background:#b00020}a.reset{background:#1e6f3c}.bad{color:#ff5252}"
                 "</style></head><body><h1>databox console &mdash; manual override</h1>");

    // Failure status: how many boards are flashing red, and per set.
    int failBySet[5] = { 0, 0, 0, 0, 0 };
    int failTotal = 0;
    for (size_t b = 0; b < NUM_BOARDS; b++) {
        if (failed[b]) { failTotal++; failBySet[segs[b].set - 1]++; }
    }
    h += "<div class='set'>Failure &mdash; ";
    if (failTotal) h += "<span class='st bad'>" + String(failTotal) + " of " + String((int)NUM_BOARDS) +
                        " boards flashing red</span> (each set's known cartridge fixes its own)";
    else           h += "<span class='st'>none</span>";
    h += "<br><a class='fail' href='/fail'>trigger failure</a>"
         "<a class='reset' href='/reset'>reset all sets</a></div>";

    for (int i = 0; i < 5; i++) {
        String n = String(i + 1);
        h += "<div class='set'>Set " + n + " &mdash; <span class='st'>" + stateName(setState[i]) + "</span>";
        if (failBySet[i]) h += " &mdash; <span class='st bad'>" + String(failBySet[i]) + " failing</span>";
        h += "<br>";
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
    // Operator: trigger a failure (GET from the admin page redirects back).
    server.on("/fail", HTTP_ANY, [](AsyncWebServerRequest* r) {
        pendingFail = true;
        if (r->method() == HTTP_GET) r->redirect("/"); else r->send(200, "text/plain", "failure triggered");
    });
    // Operator: put every set back to default in one click. Set state only — any
    // failed boards keep flashing, since those are repaired per set by a known
    // cartridge (a set's manual "known" button still clears its own).
    server.on("/reset", HTTP_ANY, [](AsyncWebServerRequest* r) {
        for (int i = 0; i < 5; i++) setState[i] = S_DEFAULT;
        Serial.println("All sets -> default");
        if (r->method() == HTTP_GET) r->redirect("/"); else r->send(200, "text/plain", "reset");
    });
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
// small-ring idle palette (independent of the per-panel bar colours below)
static const CRGB SMALL_IDLE[] = { CRGB(130,130,130), CRGB(190,110,0),
                                   CRGB(0,150,0), CRGB(0,70,190) };  // white/amber/green/blue
static const uint8_t NUM_SMALL_IDLE = sizeof(SMALL_IDLE) / sizeof(SMALL_IDLE[0]);
// Bar idle = peak-level meter. ONE colour per panel (index 0-4 = panels 1-5);
// every bar on a panel uses its panel's colour. Edit freely — green/white/yellow
// (red is deliberately unused here: a red bar at idle reads as a fault).
static const CRGB PANEL_BAR_COLOR[5] = {
    CRGB(  0, 150, 0),    // panel 1 — green
    CRGB(255, 255, 255),  // panel 2 — white
    CRGB(255, 190, 0),    // panel 3 — yellow
    CRGB(  0, 150, 0),    // panel 4 — green
    CRGB(255, 190, 0),    // panel 5 — yellow
};
static const uint16_t COMET_STEP_MS = 60;   // comet advance interval
static const uint8_t  COMET_FADE    = 64;   // comet tail fade per step
// peak-level meter tuning (bar idle)
static const uint16_t VU_STEP_MS      = 45;   // meter update rate
static const uint8_t  VU_HIT_CHANCE   = 70;   // 0-255 chance of an attack per step
static const uint16_t VU_PEAK_HOLD_MS = 400;  // how long the peak pixel hangs at the top
static const uint16_t VU_PEAK_FALL_MS = 60;   // peak fall rate once it starts dropping
static const uint8_t  VU_BODY_SCALE   = 110;  // meter body brightness vs the peak pixel

// medium/large "pressure gauge" idle: green ring with a fluctuating yellow arc
static const CRGB     GAUGE_OK       = CRGB(0, 150, 0);     // green  — nominal
static const CRGB     GAUGE_WARN     = CRGB(255, 190, 0);   // yellow — slight warning
static const CRGB     GAUGE_CRIT     = CRGB(255,   0, 0);   // red    — the extreme end
static const uint16_t GAUGE_STEP_MS  = 110;  // how fast the level sweeps (ms per pixel)
// Cap on the yellow arc: 3/4 of the ring (a hard stop at 1/2 read oddly in person).
// Past that the needle runs red, all the way to the last pixel of the ring.
static inline uint8_t gaugeMax(uint16_t n) { return (uint8_t)(n * 3 / 4); }
static inline uint8_t gaugeTop(uint16_t n) { return (uint8_t)n; }   // full travel
static const uint16_t ERROR_FLASH_MS = 250;  // medium/large unknown-state flash half-period

static const uint8_t  FAIL_PERCENT  = 30;    // ~% of each set's boards that fail
static const uint16_t FAIL_FLASH_MS = 300;   // failure flash half-period

struct Anim {
    SetState lastState;
    bool     wasFailed;    // was flashing red last frame (forces a clean restart on repair)
    bool     blinkOn;      // small idle
    uint32_t blinkNext;
    uint8_t  gaugeLvl;         // medium/large gauge: yellow pixels shown now
    uint8_t  gaugeTarget;      // ...level it is easing toward
    uint32_t gaugeNextTarget;  // when to pick a new target
    uint32_t gaugeLastStep;    // last one-pixel move
    uint8_t  vuLevel;          // bar meter: lit pixels now
    uint8_t  vuPeak;           // ...peak-hold pixel
    uint32_t vuPeakHold;       // when the peak may start falling
    uint32_t vuLastStep;       // last meter update
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
        a.blinkOn = !a.blinkOn;   // colour is fixed per ring (assigned at boot)
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

// Bar idle: peak-level meter (see below); medium/large idle: pressure gauge.
// Bar idle: a stereo peak-level meter. Fills from the first pixel in the panel's
// colour with a fast attack and steady decay, plus a brighter peak-hold pixel.
static void animLevelMeter(size_t i, const CRGB& c) {
    Anim& a = anim[i];
    const uint16_t n = segs[i].count;
    uint32_t now = millis();

    if (now - a.vuLastStep >= VU_STEP_MS) {
        a.vuLastStep = now;
        if (random8() < VU_HIT_CHANCE) {                  // transient: jump up
            uint8_t target = (uint8_t)random(1, n + 1);
            if (target > a.vuLevel) a.vuLevel = target;
        } else if (a.vuLevel > 0) {
            a.vuLevel--;                                  // otherwise fall off
        }
        if (a.vuLevel >= a.vuPeak) {                      // peak follows up, holds
            a.vuPeak = a.vuLevel;
            a.vuPeakHold = now + VU_PEAK_HOLD_MS;
        } else if (now >= a.vuPeakHold && a.vuPeak > 0) { // ...then drifts down
            a.vuPeak--;
            a.vuPeakHold = now + VU_PEAK_FALL_MS;
        }
    }

    CRGB body = c;
    body.nscale8_video(VU_BODY_SCALE);
    CRGB* leds = segLeds(i);
    for (uint16_t p = 0; p < n; p++) leds[p] = (p < a.vuLevel) ? body : CRGB::Black;
    if (a.vuPeak > 0) leds[a.vuPeak - 1] = c;             // peak pixel, full brightness
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

// Whole board flashing one colour (used by failure mode and medium/large errors).
static void animFlash(size_t i, const CRGB& c, uint16_t halfPeriodMs) {
    bool on = ((millis() / halfPeriodMs) & 1) == 0;
    fill_solid(segLeds(i), segs[i].count, on ? c : CRGB::Black);
}

// Medium/large idle: a pressure gauge. The ring sits green; a contiguous arc of
// yellow grows and shrinks sequentially around it, capped at 3/4 of the ring, and
// once that fills the needle runs red through to the last pixel —
// "nominal, drifting toward warning, occasionally pegged in the red".
static void animGauge(size_t i) {
    Anim& a = anim[i];
    const uint16_t n = segs[i].count;
    const uint8_t  maxWarn = gaugeMax(n);      // yellow never passes 3/4 of the ring
    const uint8_t  top     = gaugeTop(n);      // ...beyond which it runs red, to the end
    uint32_t now = millis();

    if (now >= a.gaugeNextTarget) {            // drift toward a new level
        a.gaugeTarget = (uint8_t)random(0, top + 1);
        a.gaugeNextTarget = now + random(900, 2600);
    }
    if (now - a.gaugeLastStep >= GAUGE_STEP_MS) {   // ease one pixel at a time
        a.gaugeLastStep = now;
        if      (a.gaugeLvl < a.gaugeTarget) a.gaugeLvl++;
        else if (a.gaugeLvl > a.gaugeTarget) a.gaugeLvl--;
    }

    CRGB* leds = segLeds(i);
    for (uint16_t p = 0; p < n; p++) {
        if      (p >= a.gaugeLvl) leds[p] = GAUGE_OK;      // not reached yet
        else if (p < maxWarn)     leds[p] = GAUGE_WARN;    // the yellow arc
        else                      leds[p] = GAUGE_CRIT;    // the red tip
    }
}

// Fail ~FAIL_PERCENT% of the boards in EACH set (min 1), chosen at random, so every
// device has something to repair. Replaces any earlier failure selection.
static void triggerFailure() {
    for (size_t b = 0; b < NUM_BOARDS; b++) failed[b] = false;
    size_t total = 0;
    for (int s = 1; s <= 5; s++) {
        size_t idx[NUM_BOARDS];
        size_t n = 0;
        for (size_t b = 0; b < NUM_BOARDS; b++) if (segs[b].set == s) idx[n++] = b;
        if (n == 0) continue;
        size_t k = (n * FAIL_PERCENT + 50) / 100;     // rounded
        if (k < 1) k = 1;
        for (size_t j = 0; j < k; j++) {              // partial Fisher-Yates shuffle
            size_t r = j + (size_t)random((long)(n - j));
            size_t tmp = idx[j]; idx[j] = idx[r]; idx[r] = tmp;
            failed[idx[j]] = true;
            total++;
        }
    }
    Serial.printf("FAILURE triggered: %u of %u boards flashing red\n",
                  (unsigned)total, (unsigned)NUM_BOARDS);
}

// A known cartridge on set `s` (0-4) repairs that set's failed boards.
static void repairSet(int s) {
    unsigned fixed = 0;
    for (size_t b = 0; b < NUM_BOARDS; b++) {
        if (segs[b].set == s + 1 && failed[b]) { failed[b] = false; fixed++; }
    }
    if (fixed) Serial.printf("Set %d known: repaired %u board(s)\n", s + 1, fixed);
}

// Render one board for the current state of its set.
static void renderBoard(size_t i) {
    Anim& a = anim[i];
    if (failed[i]) {                         // failure overrides the set's animation
        animFlash(i, COLOR_BAD, FAIL_FLASH_MS);
        a.wasFailed = true;
        return;
    }
    if (a.wasFailed) {                       // just repaired: restart cleanly
        a.wasFailed = false;
        a.lastState = (SetState)0xFF;        // != any real state -> forces the reset below
    }
    SetState st = setState[segs[i].set - 1];
    if (st != a.lastState) {                 // clean transition
        fill_solid(segLeds(i), segs[i].count, CRGB::Black);
        a.head = 0; a.cometLast = 0; a.blinkOn = false; a.blinkNext = 0;
        a.lastState = st;
    }
    const CRGB& active = (st == S_KNOWN) ? COLOR_KNOWN : COLOR_BAD;
    if (segs[i].type == SMALL) {
        if (st == S_DEFAULT) animSmallIdle(i); else animPulse(i, active);
    } else if (segs[i].type == MEDIUM || segs[i].type == LARGE) {
        if      (st == S_DEFAULT) animGauge(i);                       // pressure gauge
        else if (st == S_UNKNOWN) animFlash(i, COLOR_BAD, ERROR_FLASH_MS);  // error
        else                      animComet(i, COLOR_KNOWN);          // known
    } else {                                 // bar
        if (st == S_DEFAULT) animLevelMeter(i, PANEL_BAR_COLOR[BOARDS[i].panel - 1]);
        else                 animComet(i, active);
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
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 11000);  // hard cap ~11A on the 15A supply
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
        anim[i].wasFailed  = false;
        failed[i]          = false;
        anim[i].blinkOn    = false;
        anim[i].blinkNext  = millis() + random(0, 500);
        anim[i].blinkColor = SMALL_IDLE[random(NUM_SMALL_IDLE)];  // one fixed colour per ring
        anim[i].gaugeLvl        = (uint8_t)random(gaugeTop(segs[i].count) + 1);  // desync gauges
        anim[i].gaugeTarget     = anim[i].gaugeLvl;
        anim[i].gaugeNextTarget = millis() + random(300, 1800);
        anim[i].gaugeLastStep   = 0;
        anim[i].vuLevel    = (uint8_t)random(segs[i].count + 1);   // desync meters
        anim[i].vuPeak     = anim[i].vuLevel;
        anim[i].vuPeakHold = 0;
        anim[i].vuLastStep = 0;
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
    // Apply requests raised by the HTTP handlers (all LED/failure state is owned here).
    if (pendingFail) { pendingFail = false; triggerFailure(); }
    for (int s = 0; s < 5; s++) {
        if (pendingFix[s]) { pendingFix[s] = false; repairSet(s); }
    }

    for (size_t i = 0; i < NUM_BOARDS; i++) renderBoard(i);
    FastLED.show();

    updateSingles();
    delay(20);
}
