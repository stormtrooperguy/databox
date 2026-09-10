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
