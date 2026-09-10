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

// Per-type bring-up colours (so each board type is identifiable while wiring).
static CRGB typeColor(BoardType t) {
    switch (t) {
        case BAR:    return CRGB(0,   80,  0);    // green
        case SMALL:  return CRGB(0,   0,   80);   // blue
        case MEDIUM: return CRGB(80,  40,  0);    // amber
        case LARGE:  return CRGB(60,  0,   80);   // purple
    }
    return CRGB::Black;
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

    Serial.println("Ready (scaffold: bring-up render by board type).");
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
    // TODO: replace this bring-up render with real per-type animations +
    // latched per-set known/unknown override, once wiring + endpoints land.
    for (size_t i = 0; i < NUM_BOARDS; i++) {
        fill_solid(segs[i].base + segs[i].off, segs[i].count, typeColor(segs[i].type));
    }
    FastLED.show();

    updateSingles();
    delay(20);
}
