// =============================================================================
//  RFID Cartridge Player  —  ESP32 firmware  (project: databox)
// =============================================================================
//  A cartridge (RFID tape) is inserted into a slot, which presents its tag to
//  a PN532 NFC reader. The tag is classified as:
//     * KNOWN   - one of 10 catalogued tapes. Reader GREEN LED, 16-ring BLUE,
//                 plays the "known" track.
//     * ERROR   - the single "bad" tape. Reader RED LED, 16-ring RED, "other" track.
//     * UNKNOWN - fallback for any other tag. Reader RED LED, 16-ring RED, "other" track.
//
//  On insertion the 16-LED ring runs a 2s white chase, flashes white twice,
//  then holds its class colour (blue = good, red = not good). Once the ring
//  settles, the reader LED lights and the matching audio track plays on the
//  DFRobot DFR1173 voice module.
//
//  Cartridge presence model:
//    - A tag is read once on insertion and its action fires exactly once.
//    - While the SAME tag stays present, nothing repeats or interrupts.
//    - Brief read dropouts (the cartridge being jostled) are debounced, so a
//      tag has to be genuinely gone for ABSENT_DEBOUNCE_MS before it counts as
//      removed. Re-inserting a tape then replays its sequence.
//
//  The five 7-LED rings continuously blink white @ 50% in a random pattern to
//  emulate a "thinking" 70s/80s sci-fi computer. The three white LEDs are
//  always on. WiFi connects at boot and a stub reports each scan to a remote
//  API (endpoint added later).
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <FastLED.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"   // WIFI_SSID / WIFI_PASSWORD — git-ignored (see secrets.h.example)

// -----------------------------------------------------------------------------
//  Pin map  (ESP32 DevKit)
// -----------------------------------------------------------------------------
// PN532 NFC reader (I2C). Runs from 5V on common breakouts (Elechouse V3 etc.),
// so the whole build can share one 5V rail. SDA=21/SCL=22 are the ESP32's
// default I2C pins. Set the board's mode switches to I2C.
#define PIN_PN532_SDA    21
#define PIN_PN532_SCL    22
#define PIN_PN532_IRQ    32
#define PIN_PN532_RST    33

// Reader status LEDs (pins transposed to match the as-built wiring)
#define PIN_LED_GREEN    26
#define PIN_LED_RED      25

// WS2812B data lines
#define PIN_RING16       13    // the single 16-LED ring
#define PIN_RINGS7       4     // the five 7-LED rings, chained (5 x 7 = 35 px)

// The three always-on white LEDs (tie them to one gate/transistor on this pin)
#define PIN_WHITE_LEDS   14

// DFR1173 voice module (UART2). ESP TX(17) -> module RX, ESP RX(16) <- module TX.
#define PIN_DF_RX        16    // ESP32 receives on this pin
#define PIN_DF_TX        17    // ESP32 transmits on this pin

// -----------------------------------------------------------------------------
//  User configuration
// -----------------------------------------------------------------------------
// WiFi credentials live in src/secrets.h (git-ignored). WIFI_SSID and
// WIFI_PASSWORD are #defined there — copy secrets.h.example to secrets.h to set them.

// Static IP for this unit on the venue network (192.168.50.0/24).
static const IPAddress STATIC_IP  (192, 168, 50, 10);
static const IPAddress GATEWAY    (192, 168, 50,  1);
static const IPAddress SUBNET     (255, 255, 255, 0);
static const IPAddress DNS_SERVER (192, 168, 50,  1);

// Remote API — endpoint to be added later. Leave empty to disable reporting.
static const char* API_URL       = "";   // e.g. "https://example.com/api/scan"

// POC receiver (see poc/): the databox drives that device's 16-LED ring over
// WiFi by hitting /known, /unknown, /off. It hosts the AP at this address,
// which is also our configured gateway. Leave empty to disable.
static const char* RECEIVER_BASE_URL = "http://192.168.50.1";

// DFR1173 volume (0-30)
static const uint8_t AUDIO_VOLUME = 30;   // max

// Audio tracks. Only two: track 1 for a known tape, track 2 for anything else
// (unknown or the error tape). The DFR1173 plays by index number (the order
// files were copied onto its internal storage). See README.
static const uint16_t TRACK_KNOWN = 1;   // known tape
static const uint16_t TRACK_OTHER = 2;   // unknown or error tape

// The good track plays once; the bad-tape "alarm" (track 2) is replayed so it
// sounds for longer. BAD_TRACK_MS must be ~the length of track 2 so the plays
// chain back-to-back — set it to your alarm clip's duration.
static const uint8_t  BAD_TRACK_PLAYS = 3;      // total plays for a bad tape
// Track 2's file is ~3s but the alarm sound is only ~2s (trailing silence). A
// new play command preempts the current one, so re-triggering at ~2.1s cuts the
// silence and restarts the sound for a near-seamless, back-to-back alarm.
static const uint32_t BAD_TRACK_MS    = 2100;

// Presence / debounce tuning.
static const uint32_t POLL_INTERVAL_MS      = 120; // how often presence is checked
static const uint32_t ABSENT_DEBOUNCE_MS    = 400; // must be gone this long = removed
static const uint16_t PN532_READ_TIMEOUT_MS = 50;  // per-poll blocking read cap

// Tape UID tables --------------------------------------------------------------
// Replace the placeholder UIDs below with real ones. Every scan is printed to
// the serial monitor as "UID: xx xx xx xx" — copy those bytes in here.
struct TapeEntry {
    uint8_t len;       // UID length in bytes (4 or 7 for ISO14443A tags)
    uint8_t uid[10];   // UID bytes
};

// The 10 catalogued "good" tapes. All play TRACK_KNOWN.
static const TapeEntry KNOWN_TAPES[] = {
    { 7, {0x04, 0x7E, 0x26, 0x5B, 0xC1, 0x2A, 0x81} },
    { 7, {0x04, 0xC4, 0xBD, 0x32, 0xB9, 0x72, 0x80} },
    { 7, {0x04, 0x28, 0xBC, 0x72, 0xB8, 0x72, 0x81} },
    { 4, {0xDE, 0xAD, 0xBE, 0x04} },
    { 4, {0xDE, 0xAD, 0xBE, 0x05} },
    { 4, {0xDE, 0xAD, 0xBE, 0x06} },
    { 4, {0xDE, 0xAD, 0xBE, 0x07} },
    { 4, {0xDE, 0xAD, 0xBE, 0x08} },
    { 4, {0xDE, 0xAD, 0xBE, 0x09} },
    { 4, {0xDE, 0xAD, 0xBE, 0x0A} },
};
static const size_t KNOWN_TAPE_COUNT = sizeof(KNOWN_TAPES) / sizeof(KNOWN_TAPES[0]);

// The single "error" tape (distinct lights, but plays TRACK_OTHER like unknowns).
static const TapeEntry ERROR_TAPE = { 4, {0xBA, 0xDB, 0xAD, 0x00} };

// -----------------------------------------------------------------------------
//  LED layout
// -----------------------------------------------------------------------------
#define RING16_COUNT     16
#define RINGS7_PER_RING  7
#define RINGS7_RINGS     5
#define RINGS7_COUNT     (RINGS7_PER_RING * RINGS7_RINGS)   // 35

static CRGB ring16[RING16_COUNT];
static CRGB rings7[RINGS7_COUNT];

// "Thinking" ring colour: white @ ~50%.
static const CRGB THINK_ON  = CRGB(128, 128, 128);
static const CRGB THINK_OFF = CRGB::Black;

// 16-ring class colours: blue for good, red for not good.
static const CRGB COLOR_KNOWN   = CRGB(0,   0,   255);   // blue  (good)
static const CRGB COLOR_ERROR   = CRGB(255, 0,   0);     // red   (not good)
static const CRGB COLOR_WHITE   = CRGB(255, 255, 255);

// -----------------------------------------------------------------------------
//  Globals
// -----------------------------------------------------------------------------
Adafruit_PN532 nfc(PIN_PN532_IRQ, PIN_PN532_RST);
HardwareSerial dfSerial(2);

enum TapeClass { CLASS_KNOWN, CLASS_UNKNOWN, CLASS_ERROR };

// Per-ring blink state for the five "thinking" rings.
static bool          thinkOn[RINGS7_RINGS];
static unsigned long thinkNextToggle[RINGS7_RINGS];

// Cartridge presence state.
static bool          cartridgePresent = false;
static uint8_t       curUid[10];
static uint8_t       curUidLen = 0;
static unsigned long lastSeen  = 0;
static unsigned long lastPoll  = 0;

// Bad-tape alarm scheduler (replays TRACK_OTHER a few times from loop()).
static uint8_t       alarmPlaysLeft = 0;
static unsigned long alarmLastPlay  = 0;

// -----------------------------------------------------------------------------
//  DFR1173 voice module — raw serial command frames
//  Frame: { 0x7E, CMD, 0x00, 0x02, dataHi, dataLo, 0xEF }
// -----------------------------------------------------------------------------
static void dfrSend(uint8_t cmd, uint8_t dataHi, uint8_t dataLo) {
    uint8_t frame[7] = {0x7E, cmd, 0x00, 0x02, dataHi, dataLo, 0xEF};
    dfSerial.write(frame, sizeof(frame));
}

static void audioPlayTrack(uint16_t track) {
    dfrSend(0x03, (uint8_t)(track >> 8), (uint8_t)(track & 0xFF));
}
static void audioSetVolume(uint8_t level) { dfrSend(0x06, 0x00, level); }
static void audioStop()                   { dfrSend(0x16, 0x00, 0x00); }

// -----------------------------------------------------------------------------
//  "Thinking" rings — random independent blink, non-blocking
// -----------------------------------------------------------------------------
// Updates the rings7[] buffer. Does NOT call FastLED.show(); the caller does,
// so this can be folded into any animation loop without fighting over the bus.
static void updateThinkingRings() {
    unsigned long now = millis();
    for (int r = 0; r < RINGS7_RINGS; r++) {
        if (now < thinkNextToggle[r]) continue;

        thinkOn[r] = !thinkOn[r];
        CRGB c = thinkOn[r] ? THINK_ON : THINK_OFF;
        int base = r * RINGS7_PER_RING;
        for (int i = 0; i < RINGS7_PER_RING; i++) rings7[base + i] = c;

        // Random dwell so the rings drift out of sync and look "busy".
        thinkNextToggle[r] = now + (thinkOn[r] ? random(90, 350)    // on time
                                               : random(120, 600)); // off time
    }
}

// -----------------------------------------------------------------------------
//  Small helpers
// -----------------------------------------------------------------------------
// Wait `ms` while keeping the thinking rings animating and the strip refreshed.
static void animateHold(uint32_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        updateThinkingRings();
        FastLED.show();
        delay(5);
    }
}

static void setReaderLeds(bool green, bool red) {
    digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
    digitalWrite(PIN_LED_RED,   red   ? HIGH : LOW);
}

// -----------------------------------------------------------------------------
//  16-ring insertion animation: 1s chase -> 2 white flashes -> steady colour
// -----------------------------------------------------------------------------
static void playInsertionAnimation(CRGB finalColor) {
    // --- 2 second white comet chase ---
    const uint32_t CHASE_MS = 2000;
    const uint32_t STEP_MS  = 45;
    uint32_t start = millis();
    int head = 0;
    while (millis() - start < CHASE_MS) {
        fadeToBlackBy(ring16, RING16_COUNT, 90);   // trailing tail
        ring16[head] = COLOR_WHITE;
        head = (head + 1) % RING16_COUNT;

        uint32_t stepStart = millis();
        while (millis() - stepStart < STEP_MS) {
            updateThinkingRings();
            FastLED.show();
            delay(3);
        }
    }

    // --- 2 white flashes ---
    for (int f = 0; f < 2; f++) {
        fill_solid(ring16, RING16_COUNT, COLOR_WHITE);
        animateHold(130);
        fill_solid(ring16, RING16_COUNT, CRGB::Black);
        animateHold(130);
    }

    // --- steady final colour ---
    fill_solid(ring16, RING16_COUNT, finalColor);
    FastLED.show();
}

// -----------------------------------------------------------------------------
//  RFID classification (operates on an explicit UID, not global reader state)
// -----------------------------------------------------------------------------
static bool uidMatches(const TapeEntry& e, const uint8_t* uid, uint8_t len) {
    if (len != e.len) return false;
    for (uint8_t i = 0; i < len; i++) {
        if (uid[i] != e.uid[i]) return false;
    }
    return true;
}

static bool sameUid(const uint8_t* a, uint8_t alen, const uint8_t* b, uint8_t blen) {
    if (alen != blen) return false;
    return memcmp(a, b, alen) == 0;
}

static TapeClass classifyTape(const uint8_t* uid, uint8_t len, uint16_t& trackOut) {
    if (uidMatches(ERROR_TAPE, uid, len)) {
        trackOut = TRACK_OTHER;
        return CLASS_ERROR;
    }
    for (size_t i = 0; i < KNOWN_TAPE_COUNT; i++) {
        if (uidMatches(KNOWN_TAPES[i], uid, len)) {
            trackOut = TRACK_KNOWN;
            return CLASS_KNOWN;
        }
    }
    trackOut = TRACK_OTHER;
    return CLASS_UNKNOWN;
}

static void printUid(const uint8_t* uid, uint8_t len) {
    Serial.print("UID:");
    for (uint8_t i = 0; i < len; i++) Serial.printf(" %02X", uid[i]);
    Serial.println();
}

// -----------------------------------------------------------------------------
//  Remote API reporting (stub — endpoint added later)
// -----------------------------------------------------------------------------
static void reportScan(TapeClass cls, uint16_t track, const uint8_t* uid, uint8_t len) {
    if (strlen(API_URL) == 0) return;             // reporting disabled
    if (WiFi.status() != WL_CONNECTED) return;

    char uidStr[21] = {0};
    for (uint8_t i = 0; i < len && i < 10; i++) {
        snprintf(uidStr + i * 2, 3, "%02X", uid[i]);
    }
    const char* clsStr = (cls == CLASS_KNOWN) ? "known"
                       : (cls == CLASS_ERROR) ? "error"
                                              : "unknown";

    String body = String("{\"uid\":\"") + uidStr +
                  "\",\"class\":\"" + clsStr +
                  "\",\"track\":" + track + "}";

    HTTPClient http;
    http.begin(API_URL);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    Serial.printf("API POST -> %d\n", code);
    http.end();
}

// Fire-and-forget hit to a POC receiver endpoint ("/known", "/unknown", "/off").
static void notifyReceiver(const char* path) {
    if (strlen(RECEIVER_BASE_URL) == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    String url = String(RECEIVER_BASE_URL) + path;
    http.begin(url);
    http.setConnectTimeout(800);   // don't stall long if the receiver is absent
    http.setTimeout(800);
    int code = http.POST("");
    Serial.printf("POC %s -> %d\n", path, code);
    http.end();
}

// -----------------------------------------------------------------------------
//  Handle a freshly inserted cartridge (fires once per insertion)
// -----------------------------------------------------------------------------
static void handleTape(const uint8_t* uid, uint8_t len) {
    printUid(uid, len);

    uint16_t track = 0;
    TapeClass cls = classifyTape(uid, len, track);

    const char* clsName = (cls == CLASS_KNOWN) ? "KNOWN"
                        : (cls == CLASS_ERROR) ? "ERROR"
                                               : "UNKNOWN";
    Serial.printf("  -> %s (track %u)\n", clsName, track);

    CRGB finalColor;
    bool ledGreen = false, ledRed = false;
    switch (cls) {
        case CLASS_KNOWN:   finalColor = COLOR_KNOWN; ledGreen = true;  ledRed = false; break;
        case CLASS_UNKNOWN: finalColor = COLOR_ERROR; ledGreen = false; ledRed = true;  break;
        case CLASS_ERROR:   finalColor = COLOR_ERROR; ledGreen = false; ledRed = true;  break;
    }

    // Reader LEDs stay dark during the ring animation, then light once the ring
    // settles on its final colour; audio follows.
    setReaderLeds(false, false);
    playInsertionAnimation(finalColor);
    setReaderLeds(ledGreen, ledRed);
    audioPlayTrack(track);
    // Good track plays once; a bad tape queues extra replays so the alarm lasts.
    alarmPlaysLeft = (cls == CLASS_KNOWN) ? 0 : (BAD_TRACK_PLAYS - 1);
    alarmLastPlay  = millis();

    // Drive the POC receiver: /known for a good tape, /unknown for anything else.
    notifyReceiver(cls == CLASS_KNOWN ? "/known" : "/unknown");
    reportScan(cls, track, uid, len);
}

static void handleRemoval() {
    setReaderLeds(false, false);
    fill_solid(ring16, RING16_COUNT, CRGB::Black);
    FastLED.show();
    alarmPlaysLeft = 0;   // cancel any pending alarm replays
    notifyReceiver("/off");
    Serial.println("Cartridge removed.");
}

// -----------------------------------------------------------------------------
//  RFID presence read
// -----------------------------------------------------------------------------
// The PN532 re-detects a card on every call, so while a cartridge sits in the
// field this keeps returning its UID — exactly what the presence model wants.
// The short timeout stops empty polls from stalling the light animations.
// Returns true and fills uid/len when a tag is read.
static bool tryReadUid(uint8_t* uid, uint8_t* len) {
    uint8_t buf[7] = {0};                 // ISO14443A UIDs are 4 or 7 bytes
    uint8_t uidLen = 0;

    if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, buf, &uidLen,
                                 PN532_READ_TIMEOUT_MS)) {
        return false;
    }
    if (uidLen == 0 || uidLen > sizeof(buf)) return false;

    memcpy(uid, buf, uidLen);
    *len = uidLen;
    return true;
}

// -----------------------------------------------------------------------------
//  Setup
// -----------------------------------------------------------------------------
static void connectWifi() {
    Serial.printf("WiFi: connecting to \"%s\" ...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_SERVER)) {
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

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nRFID Cartridge Player (databox) booting...");

    // Reader status LEDs + always-on white LEDs.
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED,   OUTPUT);
    pinMode(PIN_WHITE_LEDS, OUTPUT);
    setReaderLeds(false, false);
    digitalWrite(PIN_WHITE_LEDS, HIGH);   // white LEDs are always on

    // WS2812B strips.
    FastLED.addLeds<WS2812B, PIN_RING16, GRB>(ring16, RING16_COUNT);
    FastLED.addLeds<WS2812B, PIN_RINGS7, GRB>(rings7, RINGS7_COUNT);
    FastLED.setBrightness(255);            // per-pixel values already scaled
    FastLED.clear(true);

    // Seed randomness for the thinking rings from a floating ADC pin.
    randomSeed(analogRead(35) ^ micros());
    for (int r = 0; r < RINGS7_RINGS; r++) {
        thinkOn[r] = false;
        thinkNextToggle[r] = millis() + random(0, 400);
    }

    // PN532 NFC reader (I2C).
    Wire.begin(PIN_PN532_SDA, PIN_PN532_SCL);
    nfc.begin();
    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        Serial.println("PN532: NOT found (check I2C wiring / mode switch).");
    } else {
        Serial.printf("PN532: found, firmware %d.%d\n",
                      (int)((ver >> 16) & 0xFF), (int)((ver >> 8) & 0xFF));
    }
    nfc.SAMConfig();   // required before reading passive targets

    // DFR1173 voice module.
    dfSerial.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);
    delay(200);
    audioSetVolume(AUDIO_VOLUME);
    Serial.println("DFR1173: volume set.");

    connectWifi();

    Serial.println("Ready. Insert a cartridge.");
}

// -----------------------------------------------------------------------------
//  Main loop
// -----------------------------------------------------------------------------
void loop() {
    // Keep the "thinking" rings alive every pass.
    updateThinkingRings();
    FastLED.show();

    // Bad-tape alarm: replay TRACK_OTHER a few times so it sounds for longer.
    if (alarmPlaysLeft > 0 && millis() - alarmLastPlay >= BAD_TRACK_MS) {
        audioPlayTrack(TRACK_OTHER);
        alarmLastPlay = millis();
        alarmPlaysLeft--;
    }

    unsigned long now = millis();
    if (now - lastPoll < POLL_INTERVAL_MS) {
        delay(5);
        return;
    }
    lastPoll = now;

    uint8_t uid[10];
    uint8_t len = 0;
    if (tryReadUid(uid, &len)) {
        lastSeen = now;
        // A new insertion is either "nothing was present" or "a different tag
        // than the one we're tracking" (a hot swap without a clean removal).
        bool isNew = !cartridgePresent || !sameUid(uid, len, curUid, curUidLen);
        if (isNew) {
            memcpy(curUid, uid, len);
            curUidLen = len;
            cartridgePresent = true;
            handleTape(uid, len);
        }
        // Same tag still present -> do nothing (no repeat, no interrupt).
    } else if (cartridgePresent && (now - lastSeen >= ABSENT_DEBOUNCE_MS)) {
        // Missed reads long enough to count as a real removal (jostle-proof).
        cartridgePresent = false;
        curUidLen = 0;
        handleRemoval();
    }
}
