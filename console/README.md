# databox control console

The ESP32-driven console the portable devices talk to: 388 addressable LEDs
across **5 panels**, plus 4 plain "single" LEDs. Separate PlatformIO project from
the portable reader (`../`).

## Board types

| Name | Board | LEDs |
|---|---|---|
| `bar` | 8-px strip | 8 |
| `small` | 7-px ring | 7 |
| `medium` | 16-px ring | 16 |
| `large` | 24-px ring | 24 |
| `single` | plain LED (GPIO) | 1 |

## Layout (per panel)

| Panel | bar | small | medium | large | single |
|---|--|--|--|--|--|
| 1 | 5 | 4 | – | – | 2 |
| 2 | 4 | 2 | – | 1 | – |
| 3 | 6 | 3 | 1 | – | 2 |
| 4 | 6 | 2 | 3 | – | – |
| 5 | 6 | 1 | – | – | – |
| **Total** | 27 | 12 | 4 | 1 | 4 |

**44 addressable boards / 388 pixels** (worst-case ~23.3 A →
`setMaxPowerInVoltsAndMilliamps(5, 9000)` caps it under the 10 A supply). On
panels 1 and 5 the last two bars are at the **end of the chain** for easier assembly.

## Three independent groupings

- **panel (1–5)** — physical location (metadata).
- **chain / pin** — daisy-chain wiring; one LED array + data pin per panel.
- **set (1–5)** — the logical group **one portable device** drives. Sets are
  **spread across panels** so a device lights the whole console, and every board
  belongs to exactly one set. See `BOARDS[]` in `src/main.cpp`.

Set assignment (even spread — 8 or 9 boards each):

| Set | bar | small | medium | large | boards | panels |
|---|--|--|--|--|--|--|
| 1 | 5 | 2 | – | 1 | 8 | 1,2,3,4,5 |
| 2 | 6 | 2 | 1 | – | 9 | 1,2,3,4,5 |
| 3 | 6 | 2 | 1 | – | 9 | 1,3,4,5 |
| 4 | 5 | 3 | 1 | – | 9 | 1,2,3,4,5 |
| 5 | 5 | 3 | 1 | – | 9 | 1,2,3,4,5 |

The four late-added end-of-chain bars went to sets 5 and 3 (panel 1) and sets 4
and 2 (panel 5). Change any board's set by editing its row in `BOARDS[]`.

## Behaviour

- **Default (idle):** each small ring blinks on/off as a **single unit in one
  fixed colour** (white, amber, green or blue, assigned per ring at boot); medium,
  large and bars run a random per-pixel **twinkle** in **white / red / yellow**;
  singles blink randomly.
- **Device activation (latched):** a device hits `/setN/known` or `/setN/unknown`
  and that set latches until changed (`/setN/off` → default):
  - small rings **pulse** (breathing)
  - medium/large **comet chase**; bars a **left→right comet sweep**
  - colour is **blue** (known) / **red** (unknown)
  - singles are decorative only — not part of the set response.

### Failure mode

An operator can trigger a failure from the admin page (**trigger failure**, or
`GET`/`POST /fail`):

- **~30% of the boards flash red** (300 ms on/off), picked at random **per set** —
  2–3 boards in each set — so every device has something to repair.
- A failed board **keeps flashing red until its own set receives `/known`** — i.e.
  the good cartridge in the device for that set (the admin page's manual **known**
  button counts too). Boards repair per set: set 3's cartridge only fixes set 3's
  boards, and they resume the normal known animation (blue comet) once fixed.
- `unknown` and `off` do **not** clear a failure; failed boards keep flashing while
  the rest of their set follows its normal state.
- If a set's cartridge is already sitting in its device when the failure hits, pull
  it and re-insert it (a fresh `/known` is what repairs).
- Triggering again re-rolls a fresh selection. Singles aren't part of this.
- The admin page shows how many boards are failing, overall and per set.
  Tunables: `FAIL_PERCENT`, `FAIL_FLASH_MS`.

## Networking

The console is a **WiFi station on the venue AP** (external, higher-power — the
console does **not** host it) with a **fixed IP `192.168.50.10`**, so the devices
can always reach it. Each device is configured `set url http://192.168.50.10/setN`
and POSTs `/known` `/unknown` `/off`. WiFi credentials live in git-ignored
`src/secrets.h`. The portable devices are on **DHCP** (clients), so the fleet
shares one AP without collisions.

**Admin override:** browse to `http://192.168.50.10/` for an operator page showing
each set's state with manual **default / known / unknown** controls — for
forcing a set if a portable malfunctions — plus the **trigger failure** link.
(Unauthenticated; local network only.)

## Wiring guide (recommended chains)

**One data chain per panel.** Daisy-chain each panel's boards **DIN→DOUT in the
order below** — position in the chain is what assigns each board to its set (from
`BOARDS[]` in `src/main.cpp`). Wire in this order and the set spread is automatic;
if you physically reorder, update `BOARDS[]` to match. `LEDs` is the pixel range
that board occupies in its panel's array.

**Panel 1 — GPIO13 (68 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 1 | 0–7 |
| 2 | bar | 2 | 8–15 |
| 3 | bar | 3 | 16–23 |
| 4 | small | 1 | 24–30 |
| 5 | small | 2 | 31–37 |
| 6 | small | 3 | 38–44 |
| 7 | small | 4 | 45–51 |
| 8 | bar (end of chain) | 5 | 52–59 |
| 9 | bar (end of chain) | 3 | 60–67 |

**Panel 2 — GPIO14 (70 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 4 | 0–7 |
| 2 | bar | 5 | 8–15 |
| 3 | bar | 1 | 16–23 |
| 4 | bar | 2 | 24–31 |
| 5 | large | 1 | 32–55 |
| 6 | small | 5 | 56–62 |
| 7 | small | 4 | 63–69 |

**Panel 3 — GPIO27 (85 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 3 | 0–7 |
| 2 | bar | 4 | 8–15 |
| 3 | bar | 5 | 16–23 |
| 4 | bar | 1 | 24–31 |
| 5 | bar | 2 | 32–39 |
| 6 | bar | 3 | 40–47 |
| 7 | medium | 2 | 48–63 |
| 8 | small | 5 | 64–70 |
| 9 | small | 1 | 71–77 |
| 10 | small | 2 | 78–84 |

**Panel 4 — GPIO26 (110 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 4 | 0–7 |
| 2 | bar | 5 | 8–15 |
| 3 | bar | 1 | 16–23 |
| 4 | bar | 2 | 24–31 |
| 5 | bar | 3 | 32–39 |
| 6 | bar | 4 | 40–47 |
| 7 | medium | 3 | 48–63 |
| 8 | medium | 4 | 64–79 |
| 9 | medium | 5 | 80–95 |
| 10 | small | 3 | 96–102 |
| 11 | small | 4 | 103–109 |

**Panel 5 — GPIO25 (55 px)**

| # | board | set | LEDs |
|--|--|--|--|
| 1 | bar | 5 | 0–7 |
| 2 | bar | 1 | 8–15 |
| 3 | bar | 2 | 16–23 |
| 4 | bar | 3 | 24–31 |
| 5 | small | 5 | 32–38 |
| 6 | bar (end of chain) | 4 | 39–46 |
| 7 | bar (end of chain) | 2 | 47–54 |

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

**Feature-complete.** In place: the board table with set assignment, per-panel
LED arrays + power cap, WiFi (static IP) + `/setN/{known,unknown,off}` endpoints
with latched per-set state, the admin override page, failure mode, decorative
singles, and the full per-type animations.

**Bring-up:** panel 1 wired and verified on hardware; panels 2–5 pending.

- **Bars are 8 px, not 10** — the ordered 10-px strips shipped as 8-px. `ledsFor()`
  and the panel sizes reflect the as-built hardware. If you ever swap in true 10-px
  bars, change `ledsFor(BAR)` and the `P*_LEDS` defines together.
- Pins as built: panels on `GPIO13/14/27/26/25`, singles on `16–19`.
- Animation tunables live at the top of the Animations section in
  `src/main.cpp` (`COMET_STEP_MS`, `COMET_FADE`, `TWINKLE_FADE`, `SMALL_IDLE`,
  `TWINKLE_COLORS`, `FAIL_PERCENT`, `FAIL_FLASH_MS`).
