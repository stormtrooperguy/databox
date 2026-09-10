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

- **Default:** bars flash color patterns/chases, small rings hold a solid colour,
  medium/large rings do multi-colour comet chases, singles blink randomly.
- **Device activation (latched):** a device hits `/setN/known` or `/setN/unknown`;
  that set's boards latch to **blue** (known) / **red** (unknown) until changed
  again (`/setN/off` → back to default). Singles are decorative only.

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

**In place:** the board table with set assignment, per-panel LED arrays + power
cap, WiFi (static IP) + `/setN/{known,unknown,off}` endpoints with latched
per-set state, the admin override page, and decorative singles. The render honors
set state (known=blue, unknown=red); the **default** state is still the bring-up
render (each board lit by type colour to verify wiring).

**Next:** real per-type default animations (bar patterns/chases, solid smalls,
medium/large comet chases).

- **Data pins are provisional** (`PIN_P1..P5`) — finalise at wiring.
