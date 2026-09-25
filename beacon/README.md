# databox beacon

A small standalone **beacon** that mirrors the state of the whole room. Drop one
(or several) anywhere in the space and they all show the same thing at once:
calm, alarmed, or solved.

This started life as the POC receiver driven by a single portable reader. It is
now driven by the **control console**, which pushes the room's overall state to
every beacon whenever that state changes.

## What it does

- Joins the venue WiFi as a **client** (SSID/password from `secrets.h`). It no
  longer hosts an access point.
- Sits at a **static IP stored in NVS**, set over serial — so every beacon runs
  the *same binary* and is addressed individually after flashing. With no IP
  configured it falls back to **DHCP** and prints the address it received.
- Drives a single **16-LED WS2812B ring** on **GPIO13** (no other hardware).
- Exposes three HTTP endpoints (**GET or POST**):

  | Endpoint   | Effect                                             |
  |------------|----------------------------------------------------|
  | `/known`   | LEDs pulse through shades of blue                  |
  | `/unknown` | Flash red 6 times, then hold solid red             |
  | `/off`     | Return to idle — LEDs pulse through orange/yellow  |

- Starts up in **idle** (pulsing orange/yellow). The light patterns are
  unchanged from the POC.

## What the console sends

The console decides the room's state and POSTs it to every beacon in its
`BEACON_IPS[]` list, **only when the state changes**:

| Room state | Sent | When |
|---|---|---|
| alert | `/unknown` | **any** set is unknown, or **any** board is in failure mode |
| known | `/known` | **all five** sets are known — the room has solved it |
| default | `/off` | anything else (all idle, or a partial mix of known and idle) |

Alert wins over everything, so one bad cartridge anywhere lights every beacon
red. `/known` needs the whole room correct at once.

## Wiring

One 16-LED WS2812B ring, nothing else:

| Ring | ESP32 | Notes |
|---|---|---|
| `DIN` | **GPIO13** | `PIN_RING16` in `src/main.cpp` |
| `GND` | `GND` | must be common with whatever powers the ring |
| `5V` | `5V` / external | see below |

Data goes to the ring's **DIN** end — a WS2812B ring is directional, and wiring
into `DOUT` gives you a dark ring with no error to explain it.

**Power.** 16 pixels is ~960 mA at full white, more than a USB port or the
board's regulator should be asked for. The beacon never runs full white, though:
the idle glow is capped around 2/3 brightness on one or two channels and the
alert is pure red, so real draw stays a few hundred mA. Off USB for bench work
that is fine. For anything permanent, feed the ring from a 5 V supply and tie its
ground to the ESP32's.

If you change the pin, update `PIN_RING16` — avoid GPIO6–11 (flash), and the
input-only pins 34–39 can't drive data.

## Configuring a unit

Connect over serial (115200) and use the config console:

| Command | Effect |
|---|---|
| `set ip 192.168.50.51` | store this beacon's static IP in NVS |
| `clear ip` | forget it and use DHCP |
| `show config` | print the stored IP, the live IP, and the current mode |

The address takes effect on the **next boot** — set it, then power-cycle. The
gateway and subnet are compile-time constants (`192.168.50.1`, `/24`) matching
the console's network.

Flashing does **not** erase NVS, so a reflash keeps each unit's address.

Whatever addresses you assign must also be listed in `BEACON_IPS[]` in
`../console/src/main.cpp` — that list is how the console knows who to notify.
Keep beacons clear of the DHCP pool so nothing else claims their addresses.

## Design note (from Springtrap)

The async HTTP handlers never touch FastLED. They only enqueue the requested
mode on a FreeRTOS queue; `loop()` drains it and owns every LED operation. That
keeps all hardware access single-threaded and off the AsyncTCP task.

The console side follows the same rule in reverse: its beacon POSTs run on their
own task, so a beacon that is switched off or unreachable times out without ever
stalling the console's animations.

## Build & flash

```bash
cp src/secrets.h.example src/secrets.h   # then set WIFI_SSID / WIFI_PASSWORD
pio run -t upload
pio device monitor -b 115200
```

`secrets.h` is git-ignored. Libraries (FastLED, AsyncTCP, ESPAsyncWebServer) are
pinned in `platformio.ini`.

## Testing it

**Standalone (no console needed):** from anything on the same network, visit
`http://<beacon-ip>/known`, `/unknown`, or `/off` in a browser to drive the ring.

**End-to-end:** use the console's admin page to force a set to unknown — every
beacon should go red. Set all five to known and they turn blue. Hit **reset
everything** and they return to the idle glow. The admin page shows the room
state the beacons are being sent.
