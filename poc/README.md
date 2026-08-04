# databox POC receiver

A throwaway demo target for the **databox** RFID cartridge reader. It proves that
a tape insert/read on the reader can drive an external object **over WiFi** — no
wired connection between the two devices.

## What it does

- **Hosts** a WiFi access point (`CSL_aurora` / password in `secrets.h`) — the
  same network the databox reader is configured to join.
- Sits at a **static AP IP `192.168.50.1`** (subnet `/24`). The reader uses
  `192.168.50.10` with gateway `192.168.50.1`, so this unit *is* that gateway.
- Drives a single **16-LED WS2812B ring** on **GPIO13** (no other hardware).
- Exposes three HTTP endpoints (**GET or POST**):

  | Endpoint   | Effect                                        |
  |------------|-----------------------------------------------|
  | `/known`   | LEDs pulse through shades of blue             |
  | `/unknown` | Flash red 6 times, then hold solid red        |
  | `/off`     | LEDs off                                      |

- Starts up with the **LEDs off**.

## Design note (from Springtrap)

The async HTTP handlers never touch FastLED. They only enqueue the requested
mode on a FreeRTOS queue; `loop()` drains it and owns every LED operation. That
keeps all hardware access single-threaded and off the AsyncTCP task.

## Build & flash

```bash
cp src/secrets.h.example src/secrets.h   # then set AP_SSID / AP_PASSWORD
pio run -t upload
pio device monitor -b 115200
```

`secrets.h` is git-ignored. Libraries (FastLED, AsyncTCP, ESPAsyncWebServer) are
pinned in `platformio.ini`.

## Testing it

**Standalone (no reader needed):** join the `CSL_aurora` AP from a phone/laptop,
then visit `http://192.168.50.1/known`, `/unknown`, or `/off` in a browser to
drive the ring.

**End-to-end with the reader:** the databox reader must POST to these endpoints —
`/known` on a good tape, `/unknown` on anything else, `/off` on removal — at
`http://192.168.50.1`. (That reader-side wiring is a small change to the reader's
`reportScan()` / removal path.)
