# RFID Cartridge Player

A portable, cartridge-driven audio device built on an ESP32. Players insert a
"tape" cartridge into a slot; an RFID reader identifies the tag and the device
responds with light animations and an audio clip.

Built with **PlatformIO**.

---

## Behaviour

When a cartridge is inserted, its RFID tag is read and classified:

| Class       | How it's decided                            | Reader LED | 16-ring          | Audio            | API      |
|-------------|---------------------------------------------|------------|------------------|------------------|----------|
| **Known**   | UID matches the registered good tape        | Green      | Blue             | Track 1 (known)  | /known   |
| **Special** | UID in the hard-coded `SPECIAL_TAPES` list  | off        | Purple chase     | Its own track    | none     |
| **Error**   | UID matches the single "bad" tape           | Red        | Red              | Track 2 (other)  | /unknown |
| **Unknown** | Any other UID (fallback)                     | Red        | Red              | Track 2 (other)  | /unknown |

Sequence on insertion of a **known / unknown / error** tape:

1. The **16-LED ring** runs a white comet **chase for 2 seconds**.
2. It **flashes white twice**.
3. It **holds steady** on the class colour: **blue** (good) or **red** (not good).
4. Once the ring settles, the **reader LED lights** (green/red) and the
   **matching audio track plays**.

A **special** tape skips that sequence: it plays its own track and runs a
**purple comet chase** on the 16-ring for as long as the track plays (synced via
the DFR1173 BUSY pin), with the reader LEDs dark and no API call. See
[Special / easter-egg tapes](#special--easter-egg-tapes).

Continuous background behaviour:

- The **five 7-LED rings** each blink white at **50% brightness** in an
  independent random pattern, emulating a 70s/80s sci-fi "thinking" computer.
  (All 7 LEDs in a ring act as one — they share colour covers.)
- The **three white LEDs** are on once boot completes.
- The device connects to **WiFi** at boot and reports each scan to a remote API
  (endpoint stubbed out for now).

Startup sequence (reader LEDs double as status indicators):

1. WiFi join is attempted (up to 15 s).
2. The reader LEDs flash **green ×3** if it connected, **red ×3** if it failed.
3. The **white LEDs turn on** once that sequence finishes — either way — signalling
   the device is ready.

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
| Reader green LED          | 26        |
| Reader red LED            | 25        |
| 16-LED ring data          | 13        |
| 5 × 7-LED rings data       | 4         |
| 3 white LEDs (gate)       | 14        |
| DFR1173 RX (ESP → module) | 17        |
| DFR1173 TX (module → ESP) | 16        |
| DFR1173 BUSY (→ ESP)      | 27        |

Notes:
- Set the PN532 board's **mode switches to I2C** (SEL0/SEL1 per the board's silk).
- The PN532 runs from **5V** on the common breakouts (onboard regulator), and its
  I2C lines are 5V-tolerant — no level shifter needed. It shares the 5V rail with
  the LED rings and DFR1173.
- Put a **~1kΩ resistor** in series between ESP32 TX (GPIO17) and the DFR1173 RX pin.
- The DFR1173 **BUSY** pin (low = playing) is wired to **GPIO27** and used to sync
  the special-tape purple chase to actual playback. Configured `INPUT_PULLUP`, so
  if a unit is left unwired it reads "not playing" and the chase falls back to the
  per-tape `durationMs` cap.
- The 3 white LEDs are **pre-wired modules with built-in resistors rated for 9V**.
  They're driven at 5V here (tested — they light fine, just a little dimmer), so no
  extra series resistor is needed. Still switch them through a **transistor/MOSFET**
  on GPIO14 rather than sourcing them off the pin directly.
- Add the usual WS2812B protections: a **~470Ω** resistor in the data line and a
  **1000µF** cap across the strip's 5V/GND.

### Power

Power comes from an **18V tool battery** (Ryobi ONE+) stepped down to 5V by an
**LM2596** buck converter. The load is modest — see the budget below — so the
LM2596 runs well within its 3A rating and stays cool without a heatsink. Its 40V
input ceiling easily covers the ~21V a fresh pack reaches off the charger. As
always with the LM2596, set the output to 5.0V before connecting any load.

At 20V→5V the LM2596 (non-synchronous) runs ~75–80% efficient; a synchronous
module would do ~90%, but with this device's short run times the efficiency
difference is immaterial.

With the PN532 also on 5V, everything except the ESP32's own logic shares the
single 5V rail off the buck converter — just tie all grounds together. Power the
LED rings from that 5V directly rather than through the ESP32's regulator.

#### Power budget

Estimated average draw on the 5V rail with the LEDs animating constantly and a
tape insert / audio clip every 3–5 minutes:

| Load                              | Avg @ 5V |
|-----------------------------------|----------|
| ESP32 + WiFi (connected)          | ~120 mA  |
| 5× 7-LED "thinking" rings (50%)   | ~430 mA  |
| 16-LED ring                       | ~100 mA  |
| 3 white LEDs                      | ~25 mA   |
| PN532 (polling)                   | ~50 mA   |
| DFR1173 (mostly idle)             | ~30 mA   |
| **Total**                         | **~0.75 A** (~3.75 W) |

At the LM2596's ~78% efficiency the battery sees ~4.8 W. A **Ryobi PBP006 (18V,
2.0Ah ≈ 36 Wh)** therefore runs the system for **~6.5 hours** continuously — the
LED rings are ~70% of the load, so brightness/blink duty is the main lever if you
ever want more. These are calculated figures; confirm with a meter in series on
the 5V rail.

In practice this device runs **≤45 minutes per round** and batteries are swapped
between rounds, so runtime is not a constraint and there's no need to dim anything
for battery's sake.

---

## Audio files

The DFR1173 has **16MB of internal storage** (no SD card). Connect it to your
computer over USB and copy the audio files on. The module plays a track by its
**index number** — the order the files were copied — so copy them **in order**:

| Copy order (track #) | Purpose                                       |
|----------------------|-----------------------------------------------|
| 1                    | Known tape                                    |
| 2                    | Anything else (unknown *and* error tape)      |
| 3, 4, …              | One per special / easter-egg tape (see below) |

Naming the files with a numeric prefix (e.g. `01_known.mp3`, `02_other.mp3`,
`03_special.mp3`) and copying them in that order keeps the index predictable.
MP3/WAV/WMA are supported. The firmware sends the raw serial "play track N"
command (`0x7E 0x03 … 0xEF`); no library is needed. Volume is max (30) at boot.

Track numbers are configurable in `src/main.cpp` (`TRACK_KNOWN`, `TRACK_OTHER`,
and the `track` field of each `SPECIAL_TAPES` entry).

---

## Configuration

Most user settings are grouped at the top of [`src/main.cpp`](src/main.cpp):

- **API endpoint** — the per-unit remote **base URL**, stored in flash and set
  over the serial console (see below); `API_URL` in `src/main.cpp` is only the
  compiled default when none is stored. The reader POSTs the mode to it the same
  way it drives the POC — `<base>/known` or `<base>/unknown` on insert, and
  `<base>/off` on removal (skipped if unset). The error tape reports as
  `/unknown`, so the API sees three modes: **known / unknown / off**.
- **POC receiver** — `RECEIVER_BASE_URL` (default `http://192.168.50.1`). On each
  scan the reader hits `/known` (good tape) or `/unknown` (anything else), and
  `/off` on removal. Set `""` to disable. See [`poc/`](poc/).
- **Volume** — `AUDIO_VOLUME` (0–30; currently 30 = max).
- **Good tape** — self-registered at boot and saved to flash; `KNOWN_TAPES[]`
  is only the fallback when none was ever registered. See below.
- **Network** — this unit uses a **static IP** on the `192.168.50.0/24` venue
  network: `STATIC_IP` = `192.168.50.10`, `GATEWAY`/`DNS_SERVER` = `192.168.50.1`,
  `SUBNET` = `255.255.255.0`. Adjust if the router isn't at `.1`.

### WiFi credentials (`secrets.h`)

WiFi credentials are **not** committed. They live in `src/secrets.h`, which is
git-ignored. Before the first build, copy the template and fill in real values:

```bash
cp src/secrets.h.example src/secrets.h
```

Then edit `src/secrets.h`:

```c
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"
```

Use your real values here — never commit them. `src/secrets.h` is git-ignored
so they stay out of the repo.

`src/main.cpp` includes `secrets.h`, so the build will fail if the file is
missing — that's the reminder to create it on a fresh checkout.

### The good tape (self-registration)

There is a single **good tape**, set by self-registration:

- **A tape present at boot** becomes the good tape. Its UID is saved to the
  ESP32's **NVS flash** (via `Preferences`), so it persists across power cycles
  **and firmware reflashes** (only a full chip-erase clears it). To re-assign the
  good tape, just power on with the new tape in the slot.
- **No tape at boot** → the previously saved good tape is loaded from flash.
- **Never registered** (blank flash) → the device falls back to the built-in
  `KNOWN_TAPES` list, so a fresh unit still works out of the box.

The boot scan window is `BOOT_REGISTER_MS` (1.5 s). If a tape is registered while
sitting in the slot, it then runs the normal known sequence (blue ring + audio),
which doubles as "registration succeeded" feedback.

Every scan is still printed to the serial monitor (`UID: 04 7E 26 ...`), and the
built-in `KNOWN_TAPES` / `ERROR_TAPE` fallback UIDs can be edited in
`src/main.cpp` (4-byte `len = 4`, 7-byte ISO14443A `len = 7`).

### Special / easter-egg tapes

A hard-coded `SPECIAL_TAPES[]` table in `src/main.cpp` holds any number of
easter-egg tapes. Each entry is `{ len, {uid}, track, durationMs }`:

```c
static const SpecialTape SPECIAL_TAPES[] = {
    { 7, {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, 3, 8000 },  // track 3, ~8s
    { 7, {0x04, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC}, 4, 5000 },  // track 4, ~5s
};
```

When a matching tape is inserted, it plays its `track` and runs a **purple comet
chase** on the 16-ring for `durationMs`, then the ring goes dark. Removing the
tape stops the chase and the audio. Special tapes make **no API call** — they
act only on the unit itself. Use track numbers **3+** (1 = known, 2 = other).

The chase is **synced to actual playback** via the DFR1173's BUSY pin (GPIO27):
it ends when the track finishes. `durationMs` is a **safety cap / fallback** —
used only if BUSY never asserts (pin unwired on that unit), so set it a bit
**longer** than the clip (e.g. the rickroll is ~3.5 min, so ~215000).

### Per-unit endpoint (serial console)

The five units share one firmware but each reports to its own API endpoint. The
endpoint is stored in flash (NVS) and set over the **serial console** — no
per-unit build. With the unit on USB and the monitor open (115200):

```
set url https://api.example.com/unit3/scan   # store this unit's endpoint
show config                                  # print endpoint, good tape, WiFi/IP
clear url                                     # revert to the compiled default
```

Set the **base URL** per unit (e.g. `https://api.example.com/unit3`); the reader
POSTs to `<base>/known`, `<base>/unknown`, `<base>/off`. The stored value
**persists across reboots and reflashes** (only a full chip-erase clears it), so
you set each unit once. Nothing is sent if none is set. The boot log prints the
active `API URL:` so you can confirm at a glance.

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

## POC receiver (`poc/`)

[`poc/`](poc/) is a separate ESP32 sketch used during testing to demonstrate the
reader driving an external object **wirelessly**. It hosts the reader's WiFi AP
at `192.168.50.1` and drives a 16-LED ring: pulsing blue on `/known`, red flashes
then solid red on `/unknown`, idle orange/yellow glow on `/off`. The reader reaches it because it
joins that AP (the AP is the reader's configured gateway). It's a sample/demo,
not part of the shipping device — see [`poc/README.md`](poc/README.md).

---

## Roadmap

- [x] WiFi credentials (`secrets.h`) + static IP.
- [x] Two-track audio, max volume.
- [x] POC receiver demo (`poc/`) driven over WiFi.
- [ ] Wire up the real remote API endpoint (`API_URL`) and payload schema.
- [ ] Catalog the remaining known tapes (only the one good tape is entered;
      everything else is treated as "bad").
