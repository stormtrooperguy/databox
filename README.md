# RFID Cartridge Player

A portable, cartridge-driven audio device built on an ESP32. Players insert a
"tape" cartridge into a slot; an RFID reader identifies the tag and the device
responds with light animations and an audio clip.

Built with **PlatformIO**.

---

## Behaviour

When a cartridge is inserted, its RFID tag is read and classified:

| Class       | How it's decided                            | Reader LED | 16-ring colour | Audio               |
|-------------|---------------------------------------------|------------|----------------|---------------------|
| **Known**   | UID matches one of the 10 catalogued tapes  | Green      | Blue           | That tape's track   |
| **Error**   | UID matches the single "bad" tape           | Red        | Red            | Error sound         |
| **Unknown** | Any other UID (fallback)                     | Green      | Yellow/amber   | "Unknown" sound     |

Sequence on every insertion:

1. The **16-LED ring** runs a white comet **chase for 1 second**.
2. It **flashes white twice**.
3. It **holds steady** on the class colour (blue / yellow / red).
4. Once the ring animation finishes, the **matching audio track plays**.

Continuous background behaviour:

- The **five 7-LED rings** each blink white at **50% brightness** in an
  independent random pattern, emulating a 70s/80s sci-fi "thinking" computer.
  (All 7 LEDs in a ring act as one — they share colour covers.)
- The **three white LEDs** are always on.
- The device connects to **WiFi** at boot and reports each scan to a remote API
  (endpoint stubbed out for now).

### Cartridge presence & debounce

A cartridge's action fires **exactly once** when it's inserted. The firmware
tracks presence rather than re-reading blindly:

- The reader is polled every **120 ms** (`POLL_INTERVAL_MS`). The PN532
  re-detects the tag on every read, so a tag that's already been read still
  registers as *present*.
- While the **same** tag stays present, nothing repeats or interrupts — the
  audio and lights are triggered only on the initial read.
- Momentary read dropouts (the cartridge being **jostled**) are ignored: a tag
  must go unread continuously for **400 ms** (`ABSENT_DEBOUNCE_MS`) before it
  counts as removed. On removal the reader LEDs and the 16-ring go dark.
- Re-inserting a tape (or hot-swapping to a different tag) triggers its sequence
  again.

Both timings are tunable near the top of `src/main.cpp`.

---

## Hardware

| Component                          | Notes                                      |
|------------------------------------|--------------------------------------------|
| ESP32 DevKit                       | Main controller                            |
| PN532 NFC reader (e.g. Elechouse V3) | I2C, runs at **5V**                       |
| 2 status LEDs (red + green)        | Tied to the reader                         |
| 5 × WS2812B rings, 7 LEDs each      | "Thinking" rings, chained on one data line |
| 1 × WS2812B ring, 16 LEDs           | Insertion animation ring                   |
| DFRobot DFR1173 voice module        | UART @ 9600, integrated speaker + 16MB storage |
| 3 × white LEDs                     | Always on                                  |

### Pin map (`src/main.cpp`)

| Signal                    | ESP32 GPIO |
|---------------------------|-----------|
| PN532 SDA                 | 21 (I2C)  |
| PN532 SCL                 | 22 (I2C)  |
| PN532 IRQ                 | 32        |
| PN532 RSTPD_N (reset)     | 33        |
| Reader green LED          | 25        |
| Reader red LED            | 26        |
| 16-LED ring data          | 13        |
| 5 × 7-LED rings data       | 4         |
| 3 white LEDs (gate)       | 14        |
| DFR1173 RX (ESP → module) | 17        |
| DFR1173 TX (module → ESP) | 16        |

Notes:
- Set the PN532 board's **mode switches to I2C** (SEL0/SEL1 per the board's silk).
- The PN532 runs from **5V** on the common breakouts (onboard regulator), and its
  I2C lines are 5V-tolerant — no level shifter needed. It shares the 5V rail with
  the LED rings and DFR1173.
- Put a **~1kΩ resistor** in series between ESP32 TX (GPIO17) and the DFR1173 RX pin.
- The DFR1173 also exposes a **BUSY** pin (low = playing) if you ever want the ESP32
  to detect playback; it's unused here since audio is fire-and-forget so the light
  animations keep running.
- Drive the 3 white LEDs (and any LED drawing real current) through a
  **transistor/MOSFET** on GPIO14 rather than directly off the pin.
- Add the usual WS2812B protections: a **~470Ω** resistor in the data line and a
  **1000µF** cap across the strip's 5V/GND.

### Power

There are **51 WS2812B pixels** (16 + 35). At full white that's roughly 3A at 5V,
but in normal use the 7-rings run at 50% and are often off, and the 16-ring shows
a single colour — so a solid **5V / 2–3A** supply is comfortable. With the PN532
also on 5V, everything except the ESP32's own logic shares a single 5V rail —
just tie all grounds together. Power the LED rings from 5V directly rather than
through the ESP32's regulator.

---

## Audio files

The DFR1173 has **16MB of internal storage** (no SD card). Connect it to your
computer over USB and copy the audio files on. The module plays a track by its
**index number** — the order the files were copied — so copy them **one at a
time, in order**:

| Copy order (track #) | Purpose                     |
|----------------------|-----------------------------|
| 1–10                 | The 10 known tapes' tracks  |
| 11                   | Error sound                 |
| 12                   | Unknown-tape sound          |

Naming the files with a numeric prefix (e.g. `01_intro.mp3`, `02_...`) and
copying them in that order keeps the index predictable. MP3/WAV/WMA are
supported. The firmware sends the raw serial "play track N" command
(`0x7E 0x03 … 0xEF`); no library is needed.

Track numbers are configurable near the top of `src/main.cpp`
(`TRACK_ERROR`, `TRACK_UNKNOWN`, and the `track` field of each `KNOWN_TAPES` entry).

---

## Configuration

All user settings are grouped at the top of [`src/main.cpp`](src/main.cpp):

- **WiFi** — `WIFI_SSID` / `WIFI_PASSWORD` (placeholders for now).
- **API** — `API_URL`. Leave `""` to disable reporting; set it later to POST a
  JSON body `{"uid","class","track"}` on each scan.
- **Volume** — `AUDIO_VOLUME` (0–30).
- **Tape UIDs** — `KNOWN_TAPES[]` (10 entries) and `ERROR_TAPE`.

### Programming the tape UIDs

The placeholder UIDs must be replaced with your real tags. Every scan is printed
to the serial monitor:

```
UID: DE AD BE 01
```

Insert each tape, read its UID from the monitor, and paste the bytes into the
matching `KNOWN_TAPES` entry (or `ERROR_TAPE`), then re-flash. MIFARE Classic
cards use 4-byte UIDs (`len = 4`); 7-byte ISO14443A tags are also supported
(set `len = 7`).

---

## Build & flash

Requires the [PlatformIO CLI](https://platformio.org/).

```bash
pio run                       # compile
pio run -t upload             # flash over USB
pio device monitor -b 115200  # serial monitor (see UIDs, boot logs)
```

Libraries (`adafruit/Adafruit PN532`, `fastled/FastLED`) are pinned in
[`platformio.ini`](platformio.ini) and fetched automatically on first build
(Adafruit PN532 pulls in Adafruit BusIO). The DFR1173 needs no library — it's
driven with raw UART command frames.

---

## Roadmap

- [ ] Real WiFi credentials.
- [ ] Wire up the remote API endpoint (`API_URL`) and payload schema.
- [ ] Replace placeholder UIDs with the 10 known tapes + the error tape.
- [ ] Final audio assets on the SD card.
