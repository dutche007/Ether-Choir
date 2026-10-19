# Ether Choir

Ether Choir is an experimental soundscape instrument for the M5Stack Cardputer
ADV. It passively observes nearby 2.4 GHz Wi-Fi frame metadata and BLE
advertisements, then turns activity into sound. It does not decode radio
content.

**Passive listening, metadata only.** Wi-Fi transmitter addresses are hashed
with a random per-boot salt in the capture callback. Raw addresses, SSIDs and
frame payloads are not stored or displayed.

## Current prototype

- Time-sliced, passive Wi-Fi and BLE capture; no probes or other transmissions.
- Wi-Fi channel hopping and controls to select Wi-Fi, BLE, or both.
- A fixed-pool audio renderer with keyboard input and a limiter.
- Constellation and waterfall-style display views, plus status/settings screens.
- Reverb, microphone input, IMU controls and SD recording are not currently
  enabled in this prototype. Reverb/echo, preset save/load and the SD recorder
  are deliberately out of scope for this build.

This is a work in progress, not a finished or certified device. Radio results
vary with the environment and firmware; use it only where passive reception is
permitted.

## Hardware and build

The project targets an M5Stack Cardputer ADV and builds with PlatformIO, Arduino
for ESP32, M5Cardputer 1.1.1, M5Unified 0.2.26 and NimBLE-Arduino 2.5.1. These
library versions are pinned in `platformio.ini`.

Install PlatformIO Core or the PlatformIO IDE extension, connect the device,
then run from the project directory:

```text
pio run -e cardputer-adv
pio run -t upload -e cardputer-adv
pio device monitor -b 115200 -e cardputer-adv
```

## Controls

- `a s d f g h j k l`: play notes.
- `-` / `=`: lower / raise volume.
- `Space`: toggle the test tone.
- `w` / `b` / `e`: Wi-Fi only / BLE only / both.
- `;`: cycle display screens.
- `1`-`5`: toggle source families (pad, pluck, shimmer, bell, accent).

The keyboard is scanned through the M5Cardputer library abstraction.
