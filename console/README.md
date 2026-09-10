# databox control console

The ESP32-driven console the portable devices talk to: ~402 addressable LEDs
across **5 panels**, plus 4 plain "single" LEDs. Separate PlatformIO project from
the portable reader (`../`).

## Board types

| Name | Board | LEDs |
|---|---|---|
| `bar` | 10-px strip | 10 |
| `small` | 7-px ring | 7 |
| `medium` | 16-px ring | 16 |
| `large` | 24-px ring | 24 |
| `single` | plain LED (GPIO) | 1 |

## Layout (per panel)

| Panel | bar | small | medium | large | single |
|---|--|--|--|--|--|
| 1 | 3 | 4 | – | – | 2 |
| 2 | 4 | 2 | – | 1 | – |
| 3 | 6 | 3 | 1 | – | 2 |
| 4 | 6 | 2 | 3 | – | – |
| 5 | 4 | 1 | – | – | – |
| **Total** | 23 | 12 | 4 | 1 | 4 |

**402 addressable pixels** (worst-case ~24 A → `setMaxPowerInVoltsAndMilliamps(5, 9000)` caps it under the 10 A supply).

## Three independent groupings

- **panel (1–5)** — physical location (metadata).
- **chain / pin** — daisy-chain wiring; one LED array + data pin per panel.
- **set (1–5)** — the logical group **one portable device** drives. Sets are
  **spread across panels** so a device lights the whole console, and every board
  belongs to exactly one set. See `BOARDS[]` in `src/main.cpp`.

Set assignment (even spread, 8 boards each):

| Set | bar | small | medium | large | panels |
|---|--|--|--|--|--|
| 1 | 5 | 2 | – | 1 | 1,2,3,4,5 |
| 2 | 5 | 2 | 1 | – | 1,2,3,4,5 |
| 3 | 5 | 2 | 1 | – | 1,3,4,5 |
| 4 | 4 | 3 | 1 | – | 1,2,3,4 |
| 5 | 4 | 3 | 1 | – | 2,3,4,5 |

## Behaviour

- **Default (idle):** small rings **blink** in random white/amber/green; medium,
  large, and bars run a colourful random **twinkle**; singles blink randomly.
- **Device activation (latched):** a device hits `/setN/known` or `/setN/unknown`
  and that set latches until changed (`/setN/off` → default):
  - small rings **pulse** (breathing)
  - medium/large **comet chase**; bars a **left→right comet sweep**
  - colour is **blue** (known) / **red** (unknown)
  - singles are decorative only — not part of the set response.

## Networking

The console is a **WiFi station on the venue AP** (external, higher-power — the
console does **not** host it) with a **fixed IP `192.168.50.10`**, so the devices
can always reach it. Each device is configured `set url http://192.168.50.10/setN`
and POSTs `/known` `/unknown` `/off`. WiFi credentials live in git-ignored
`src/secrets.h`. The portable devices are on **DHCP** (clients), so the fleet
shares one AP without collisions.

**Admin override:** browse to `http://192.168.50.10/` for an operator page showing
each set's state with manual **default / known / unknown** controls — for
forcing a set if a portable malfunctions. (Unauthenticated; local network only.)

## Wiring guide (recommended chains)

**One data chain per panel.** Daisy-chain each panel's boards **DIN→DOUT in the
order below** — position in the chain is what assigns each board to its set (from
`BOARDS[]` in `src/main.cpp`). Wire in this order and the set spread is automatic;
if you physically reorder, update `BOARDS[]` to match. `LEDs` is the pixel range
that board occupies in its panel's array.

**Panel 1 — GPIO13 (58 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 1 | 0–9 |
| 2 | bar | 2 | 10–19 |
| 3 | bar | 3 | 20–29 |
| 4 | small | 1 | 30–36 |
| 5 | small | 2 | 37–43 |
| 6 | small | 3 | 44–50 |
| 7 | small | 4 | 51–57 |

**Panel 2 — GPIO14 (78 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 4 | 0–9 |
| 2 | bar | 5 | 10–19 |
| 3 | bar | 1 | 20–29 |
| 4 | bar | 2 | 30–39 |
| 5 | large | 1 | 40–63 |
| 6 | small | 5 | 64–70 |
| 7 | small | 4 | 71–77 |

**Panel 3 — GPIO27 (97 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 3 | 0–9 |
| 2 | bar | 4 | 10–19 |
| 3 | bar | 5 | 20–29 |
| 4 | bar | 1 | 30–39 |
| 5 | bar | 2 | 40–49 |
| 6 | bar | 3 | 50–59 |
| 7 | medium | 2 | 60–75 |
| 8 | small | 5 | 76–82 |
| 9 | small | 1 | 83–89 |
| 10 | small | 2 | 90–96 |

**Panel 4 — GPIO26 (122 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 4 | 0–9 |
| 2 | bar | 5 | 10–19 |
| 3 | bar | 1 | 20–29 |
| 4 | bar | 2 | 30–39 |
| 5 | bar | 3 | 40–49 |
| 6 | bar | 4 | 50–59 |
| 7 | medium | 3 | 60–75 |
| 8 | medium | 4 | 76–91 |
| 9 | medium | 5 | 92–107 |
| 10 | small | 3 | 108–114 |
| 11 | small | 4 | 115–121 |

**Panel 5 — GPIO25 (47 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 5 | 0–9 |
| 2 | bar | 1 | 10–19 |
| 3 | bar | 2 | 20–29 |
| 4 | bar | 3 | 30–39 |
| 5 | small | 5 | 40–46 |

**Singles** — 4 plain LEDs, each on its own GPIO (not chained): **16, 17, 18, 19**.
Decorative random blink; pin↔LED mapping is arbitrary (2 belong in panel 1, 2 in
panel 3 physically).

Notes:
- Only **data + GND** come from the ESP32 pin; feed **5 V power separately** to
  each chain (inject at both ends / every ~100–150 px so the far end doesn't dim).
- At boot the console prints each panel's LED count — a quick way to catch a
  miscount or a board wired out of order.
- Use the **admin page** to drive each set and confirm boards light where expected
  before connecting the devices.

## Status

**Feature-complete (untested on hardware).** In place: the board table with set
assignment, per-panel LED arrays + power cap, WiFi (static IP) +
`/setN/{known,unknown,off}` endpoints with latched per-set state, the admin
override page, decorative singles, and the full per-type animations (idle blink/
twinkle; pulse + comet on known/unknown).

- **Data pins are provisional** (`PIN_P1..P5`) — finalise at wiring, then flash
  and bring-up test with the admin page.
- Animation tunables live at the top of the Animations section in
  `src/main.cpp` (`COMET_STEP_MS`, `COMET_FADE`, `TWINKLE_FADE`, palettes).
