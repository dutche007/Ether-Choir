# ETHER CHOIR: Cardputer ADV Wi-Fi/BLE Sonification Synth

> **How to use this file:** save as `GEMINI.md` (or `AGENTS.md`) in the repo root, or paste the whole thing as your first message. Then send the "Kick-off prompt" at the bottom.

---

## 0. Instructions for you, the coding assistant

You are the lead embedded developer on this project. Read this whole file before writing code.

**Working rules**
1. **Work in milestones (section 10).** Finish one, make sure it compiles, tell me how to test it on the device, then wait for me to confirm before the next. Don't build everything in one go.
2. **Never invent hardware facts.** Do not hard-code GPIO pins, I2C addresses or codec registers from memory. Take them from the installed `M5Cardputer` / `M5Unified` / `M5GFX` library board definitions or the `espp/m5stack-cardputer` BSP. If you can't confirm something, say so and add a `// TODO(verify)` comment.
3. **Verify library APIs against the installed version** (NimBLE-Arduino 1.x and 2.x differ a lot; M5Unified ADV support depends on version). Read the library source in `.pio/libdeps` when unsure.
4. **Audio task rules:** no `malloc`/`new`, no `Serial.print`, no mutexes, no SD or display access, no blocking calls. Communicate only via lock-free queues or atomics.
5. **Memory is tight** (about 512KB SRAM, no PSRAM). Allocate everything statically at boot, log `ESP.getFreeHeap()` and `heap_caps_get_largest_free_block()` at each milestone, and flag anything that worries you.
6. **Keep files small and modular** (layout in section 7). Header comments say what each module owns.
7. **Privacy by design** (section 9) is a hard requirement, not optional.
8. Use **British English** in comments and UI text. Keep comments useful, not chatty.
9. When something is ambiguous, make a sensible choice, state it in one line, and carry on. Only ask me if it blocks progress.
10. After each milestone give me: files changed, how to build/flash (`pio run -t upload`), what I should see/hear, and known issues.

---

## 1. Project summary

A pocket instrument on the **M5Stack Cardputer ADV** that listens passively to the 2.4GHz environment (Wi-Fi management/data frame *metadata* and BLE advertisements) and **sonifies** it into a rich, evolving soundscape. Every nearby device becomes a voice. The user can also play over the top with the keyboard, shape the sound by tilting the device, and record sessions to microSD.

It does **not** demodulate or "hear" radio content. It maps *metadata* (presence, strength, rate, type, channel) to sound.

**Name:** Ether Choir.

---

## 2. Hardware reference (Cardputer ADV)

| Item | Detail |
|---|---|
| MCU module | M5Stamp S3A, ESP32-S3FN8, dual-core LX7 @ 240MHz, 8MB flash, **no PSRAM** |
| Radios | Wi-Fi 2.4GHz (802.11 b/g/n), **BLE 5 only (no Bluetooth Classic)**. One shared radio, so Wi-Fi and BLE time-share |
| Display | ST7789V2, 1.14", 240x135, SPI |
| Keyboard | 56 keys (4x14). On ADV it is scanned via a **TCA8418** I2C expander (differs from the original) |
| Audio out | **ES8311 codec** + NS4150B amp + 8Ω 1W speaker. Mono, small, so mid-range heavy |
| Audio in | Analogue MEMS mic through the ES8311 |
| IMU | **BMI270** on the internal I2C bus (ADV only) |
| Storage | microSD slot |
| Other | IR emitter, WS2812 RGB LED, Grove port (HY2.0-4P), 14-pin EXT header |
| Power | 1750mAh Li battery |

**Known caveats to respect**
- On the ADV the mic and speaker paths may share I2S clock lines, so **full-duplex (listen while playing) may not work**. Design the mic feature as **time-multiplexed** (short listening windows) unless testing proves otherwise. Verify in the BSP/M5Unified source.
- Wi-Fi promiscuous scanning and BLE scanning compete for the one radio. Use time-slicing (section 5).

---

## 3. Toolchain (VS Code)

- VS Code + **PlatformIO IDE** extension.
- Framework: **Arduino on ESP32 (espressif32 platform)** for speed of development. Allow a later move to ESP-IDF only if the audio engine needs it.
- Libraries: `M5Cardputer` (pulls `M5Unified` and `M5GFX`), `NimBLE-Arduino` (pin an exact version and note which API generation you use).

`platformio.ini` starting point (adjust board/flags after checking M5's current recommended config):

```ini
[env:cardputer-adv]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
board_build.flash_size = 8MB
board_build.partitions = default_8MB.csv
build_flags =
    -DCORE_DEBUG_LEVEL=0
    -DARDUINO_USB_CDC_ON_BOOT=1
    -O2
lib_deps =
    m5stack/M5Cardputer
    h2zero/NimBLE-Arduino @ ^2.0.0
```

If the stock board definition fights the Stamp-S3A, check M5Stack's docs for the recommended board/partition settings and tell me what you changed.

---

## 4. Architecture

```
 CORE 0                                   CORE 1
 ┌───────────────────────┐                ┌──────────────────────────┐
 │ Radio task            │                │ Audio task (high prio)   │
 │  Wi-Fi sniff / BLE    │  SignalEvent   │  render blocks of N      │
 │  scheduler (slices)   │ ─── queue ───▶ │  samples → I2S/codec     │
 └───────────────────────┘                │  voices + FX             │
 ┌───────────────────────┐                └───────────▲──────────────┘
 │ Control task (med)    │  ParamSet (atomics / SPSC) │
 │  keyboard, IMU, mic   │ ───────────────────────────┘
 │  mapping/voice mgr    │
 └───────────────────────┘
 ┌───────────────────────┐   ┌─────────────────────────┐
 │ UI task (low) display │   │ SD task (low) record/   │
 └───────────────────────┘   │ presets, ring buffer in │
                             └─────────────────────────┘
```

**Data flow**
1. Radio callbacks produce tiny `SignalEvent` structs and push to a queue (never block, drop on full).
2. The **voice manager** (control task) turns events into voice commands: spawn / refresh / retune / release.
3. The **audio task** renders from a static voice pool, applies the master FX chain, and outputs blocks. It also pushes finished blocks to a ring buffer for the SD recorder (non-blocking).
4. UI and SD tasks read state snapshots; they never touch audio internals directly.

**Suggested parameters (tune by ear and CPU):**
- Sample rate 22,050Hz (try 32,000Hz if CPU allows), 16-bit mono (stereo only if the codec path makes it free).
- Block size 256 samples, with 3 DMA/playback buffers.
- Voice pool 12 (compile-time constant), master limiter on the output.
- Audio task pinned to core 1; radio on core 0.

---

## 5. Radio layer

### 5.1 Event struct

```cpp
// signal_event.h
#pragma once
#include <stdint.h>

enum class SrcType : uint8_t {
  WifiBeacon, WifiProbeReq, WifiProbeResp, WifiData, WifiOther,
  BleAdv
};

struct SignalEvent {
  uint32_t idHash;      // salted hash of the transmitter address, never the raw MAC
  int8_t   rssi;        // dBm
  uint8_t  channel;     // Wi-Fi channel 1..13, 0 for BLE
  SrcType  type;
  uint16_t companyId;   // BLE manufacturer ID, 0 if n/a
  uint32_t tMs;         // millis() at capture
};
```

### 5.2 Wi-Fi promiscuous capture (skeleton, verify against current ESP-IDF/Arduino core)

```cpp
// radio_wifi.cpp
#include <WiFi.h>
#include <esp_wifi.h>
#include "signal_event.h"
#include "radio.h"

static uint32_t g_salt;   // random per boot

static inline uint32_t fnv1a(const uint8_t* d, size_t n, uint32_t salt) {
  uint32_t h = 2166136261u ^ salt;
  for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h;
}

static void wifiSniffCb(void* buf, wifi_promiscuous_pkt_type_t pktType) {
  auto* pkt = static_cast<wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* h = pkt->payload;
  if (pkt->rx_ctrl.sig_len < 24) return;

  uint8_t ftype = (h[0] >> 2) & 0x3;     // 0 mgmt, 1 ctrl, 2 data
  uint8_t sub   = (h[0] >> 4) & 0xF;
  SignalEvent ev{};
  ev.rssi    = pkt->rx_ctrl.rssi;
  ev.channel = pkt->rx_ctrl.channel;
  ev.tMs     = millis();
  ev.idHash  = fnv1a(h + 10, 6, g_salt);  // addr2 = transmitter; hash immediately

  if (ftype == 0) {
    if      (sub == 8) ev.type = SrcType::WifiBeacon;
    else if (sub == 4) ev.type = SrcType::WifiProbeReq;
    else if (sub == 5) ev.type = SrcType::WifiProbeResp;
    else               ev.type = SrcType::WifiOther;
  } else if (ftype == 2) {
    ev.type = SrcType::WifiData;           // header metadata only, payload ignored
  } else return;

  radioPushEvent(ev);                      // non-blocking, drop if full
}

void wifiSniffStart() {
  g_salt = esp_random();
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous_rx_cb(&wifiSniffCb);
  wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&f);
  esp_wifi_set_promiscuous(true);
}

void wifiSetChannel(uint8_t ch) { esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); }
void wifiSniffStop() { esp_wifi_set_promiscuous(false); }
```

Channel hopping: step 1→13 with ~120–250ms dwell while Wi-Fi is in its time slice.

### 5.3 BLE advertisement capture (NimBLE-Arduino 2.x style, verify)

```cpp
// radio_ble.cpp
#include <NimBLEDevice.h>
#include "signal_event.h"
#include "radio.h"

class AdvCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    SignalEvent ev{};
    ev.type   = SrcType::BleAdv;
    ev.rssi   = d->getRSSI();
    ev.tMs    = millis();
    ev.idHash = bleHashAddr(d->getAddress().getVal());   // 6 bytes, salted FNV, as above
    std::string m = d->getManufacturerData();
    if (m.size() >= 2) ev.companyId = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
    radioPushEvent(ev);
  }
};
static AdvCb cb;

void bleScanInit() {
  NimBLEDevice::init("");
  auto* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&cb, false);   // false = report duplicates, we want repeat adverts as rhythm
  s->setActiveScan(false);           // passive, nothing transmitted
  s->setInterval(100); s->setWindow(99);
}
void bleScanSlice(uint32_t ms) { NimBLEDevice::getScan()->start(ms, false); }
void bleScanStop()             { NimBLEDevice::getScan()->stop(); }
```

### 5.4 Radio scheduler (core 0 task)

Time-slice because the radio is shared:
- Default cycle: **Wi-Fi slice 6s** (hop all channels, about 150ms each), then **BLE slice 3s**, repeat.
- Make slice lengths adjustable from the UI.
- Add a **"Wi-Fi only" / "BLE only" / "both"** mode.
- Voices must **linger and fade** (release time of several seconds) so the sound doesn't drop out during the other radio's slice. This is a feature: it gives a natural breathing quality.
- When switching, stop one cleanly before starting the other. Don't leave promiscuous mode on during BLE scan.

If time-slicing proves unnecessary on this hardware, make true coexistence an option, but measure packet loss before making it default.

---

## 6. Sound design

### 6.1 Mapping table

| Signal property | Sound parameter |
|---|---|
| `idHash` | Pitch: `hash % scaleLength` indexes into the active scale; octave from channel/type. Same device always gets the same note |
| `rssi` (-95..-30 dBm, normalised) | Amplitude and filter cutoff / FM index (closer = louder, brighter) |
| Wi-Fi `channel` 1..13 | Register (octave) and FM modulator ratio |
| Packet rate per device (smoothed events/sec) | LFO rate, tremolo depth, grain density, pluck retrigger rate |
| `type` | Instrument family (below) |
| BLE `companyId` | Bell/FM character: choose modulator ratio from a small table of "inharmonic" ratios (e.g. 1.41, 2.76, 3.5, 5.4) by `companyId % N` |
| Total active devices | Reverb size/wet, overall density, master gain compensation |
| BLE advertising interval (derived from inter-arrival time) | Pulse / rhythm of the voice |

### 6.2 Instrument families

| Source | Voice type | Notes |
|---|---|---|
| Wi-Fi beacon (routers) | **Pad**: 3 detuned band-limited saws or wavetable, slow attack (1–2s), slow release (4–6s), lowpass | The sustained harmonic bed |
| Wi-Fi probe request (phones searching) | **Pluck**: Karplus-Strong or short FM pluck | Triggered per event, rate-limited |
| Wi-Fi data frames | **Shimmer**: filtered noise grains / high soft clicks | Density follows packet rate; keep quiet |
| BLE advertisers | **FM bell**: 2-op FM, inharmonic ratio, exponential decay | Pitch from hash, brightness from RSSI |
| Odd/rare frames | **Accent**: short dissonant (tritone/minor 2nd) stab | Optional toggle |

### 6.3 Scales and tuning
- Scale list: Minor pentatonic (default), Major pentatonic, Dorian, Lydian, Whole tone, Hirajoshi, Chromatic.
- Root note adjustable (C..B), base octave adjustable.
- Quantise all device pitches to the scale so any crowd sounds consonant.
- Pad voices should favour chord tones (root, 3rd/4th, 5th) by weighting the hash→degree map.

### 6.4 DSP building blocks to implement
- 1024-entry int16 or float sine LUT with linear interpolation.
- Phase-accumulator oscillators (use `uint32_t` phase, 32-bit increments).
- 2-operator FM voice with exponential envelope.
- Detuned saw pad (PolyBLEP or just lowpassed naive saw, since the speaker is small).
- Karplus-Strong pluck with a tiny delay line (use fixed small buffers, e.g. max 512 samples per pluck voice).
- State-variable or one-pole filters per voice.
- ADSR envelope (linear/exponential, integer-friendly).
- **Master chain:** soft limiter, then small Schroeder-style reverb (4 combs + 2 allpasses, total roughly 6–12k samples of int16 buffers), then optional DC blocker.
- Use `esp-dsp` / ESP32-S3 vector instructions only after a plain C++ version works and profiling shows it's needed.

### 6.5 Voice management
- Fixed pool (default 12). Each voice: `idHash`, `family`, `targetAmp`, `freq`, envelope state, `lastSeenMs`.
- Event for known `idHash` → refresh and retune the voice (smooth with slew limiting to avoid zipper noise).
- Event for new device when the pool is full → steal the quietest/oldest voice with a short crossfade.
- No event for N seconds → release.
- Per-source-type limits (e.g. max 5 pads, 4 bells, 3 plucks) so one noisy type doesn't take over.
- Smooth every parameter change (slew limiter, about 20–50ms) to avoid clicks.

### 6.6 Quality bar
- No clicks, pops or zipper noise on parameter changes or voice steals.
- No clipping at full voice count (limiter + per-voice gain scaling by 1/sqrt(activeVoices)).
- Quiet room → sparse, calm. Busy area → rich but not harsh. The 1W speaker will distort if pushed, so keep the master gain conservative with a user volume control.

---

## 7. Suggested repository layout

```
/platformio.ini
/GEMINI.md
/src
  main.cpp               // setup(), task creation only
  config.h               // all constants: sample rate, pool size, slice times
  signal_event.h
  radio.h / radio.cpp    // queue + scheduler
  radio_wifi.cpp
  radio_ble.cpp
  voice_manager.h/.cpp   // events → voice commands, mapping, scales
  scales.h/.cpp
  synth/
    engine.h/.cpp        // audio task, block render, voice pool
    voice_fm.h/.cpp
    voice_pad.h/.cpp
    voice_pluck.h/.cpp
    voice_noise.h/.cpp
    fx_reverb.h/.cpp
    fx_limiter.h/.cpp
    dsp_common.h         // LUT, envelopes, filters
  audio_out.h/.cpp       // codec/I2S/Speaker setup (single place for hardware details)
  input_keys.h/.cpp
  input_imu.h/.cpp
  input_mic.h/.cpp
  ui.h/.cpp
  sd_recorder.h/.cpp
  presets.h/.cpp
/data_templates          // example preset files
```

---

## 8. The extras (all required)

### 8.1 Keyboard (playable layer + controls)
Use the M5Cardputer keyboard API. Make the mapping table-driven in one file so I can change it.

| Keys | Action |
|---|---|
| Home-row letters (`a s d f g h j k l`) | Play notes of the current scale over the ambient bed (simple pluck/bell) |
| `[` `]` | Previous/next scale |
| `-` `=` | Root note down/up |
| `,` `.` (or arrow equivalents) | Octave down/up |
| `1`–`5` | Toggle source families: pads, plucks, shimmer, bells, accents |
| `Tab` | Cycle screens (waterfall, constellation, status, settings) |
| `Space` | Freeze/hold: stop spawning new voices, keep current ones |
| `m` | Mute |
| `r` | Start/stop recording to SD |
| `w` / `b` | Radio mode: Wi-Fi only / BLE only; `e` both |
| `Fn`+arrows | Volume and reverb amount |
| `Enter` | Save preset; `Del` cancels or clears |

Handle key-repeat sensibly (notes trigger on press, not on repeat). Because the ADV keyboard goes through the TCA8418, use the library's keyboard abstraction rather than raw matrix assumptions.

### 8.2 IMU (BMI270)
- Tilt left/right → filter cutoff offset (global brightness).
- Tilt forward/back → pitch bend (about ±2 semitones) or reverb wet.
- Shake gesture → "scatter": release all voices with a burst of bells.
- Lay flat → neutral. Low-pass the IMU data and apply a deadzone.
- If M5Unified lacks BMI270 support in the installed version, use the BSP or direct I2C and tell me.

### 8.3 Microphone (ambient awareness)
- **Time-multiplexed:** every N seconds (default 20) pause output for about 150ms, measure ambient RMS, resume. Check first whether concurrent mic + speaker works on ADV; if it does, make it continuous.
- Use the room level to scale master gain slightly and modulate reverb wet (noisy room → drier, brighter; quiet room → wetter, softer).
- Optional later: use a short mic capture as a convolution-ish excitation for the reverb. Keep it behind a flag.

### 8.4 Display (240x135)
- Use an off-screen sprite (RGB565 is about 64KB; consider 8-bit palette mode if heap is tight) and push at about 20–30fps from the UI task.
- **Screen A, Constellation:** x = Wi-Fi channel (BLE in a separate strip), y = RSSI, dot size = packet rate, colour = source type, brightness fades with voice envelope.
- **Screen B, Waterfall:** scrolling spectrogram-style view of event activity per channel over time.
- **Screen C, Status:** voice count, scale and root, radio mode, free heap, CPU load of the audio task, battery, recording indicator.
- **Screen D, Settings:** slice lengths, voice limits, master volume, reverb, brightness.
- Never show raw MACs. SSIDs are not stored or shown by default.

### 8.5 microSD
- **Recorder:** audio task pushes finished blocks into a lock-free ring buffer (about 16–32KB). A low-priority SD task drains it into a 16-bit mono PCM `.wav` (write a header on open, patch lengths on close). Name files `ETHER_YYYYMMDD_HHMMSS.wav` or an incrementing counter (no RTC assumed).
- **Presets:** save/load a small config file (scale, root, family toggles, slice lengths, reverb, volume, mapping curve choices) in `/ether/presets/*.json` or `.ini`. Keep parsing minimal and static (no big JSON allocations; ArduinoJson with a fixed `StaticJsonDocument` is fine).
- **Never write raw MACs, SSIDs or payloads to the SD card.** Optional session log may contain only aggregates (counts per channel per minute).
- Handle missing/ejected cards gracefully (recorder disables, UI shows it).

### 8.6 Stretch ideas (only after core is solid)
- RGB LED pulses with the strongest voice.
- IR emitter as a metronome/blinker (visual only).
- Second Cardputer over ESP-NOW to share "scenes" (careful with radio contention).
- External I2S DAC via EXT header for stereo/headphones.
- Time-of-day or "walk mode" that biases scale/tempo.
- Serial MIDI-out or BLE-MIDI of the voice events (note: don't run BLE-MIDI at the same time as BLE scanning without testing).

---

## 9. Privacy and legality (hard rules)

- Passive reception only. **Never transmit** probes, deauths or anything else on Wi-Fi or BLE.
- Capture header metadata only (frame type/subtype, transmitter address for hashing, RSSI, channel). **Do not read, store or display frame payloads.**
- Hash transmitter addresses with a **random per-boot salt** immediately in the callback. Raw addresses must not leave the callback or hit logs, SD or the display.
- No SSID or device-name logging by default.
- Add a short "Passive listening, metadata only" line in the README and on the boot screen.
- Do not add features that track specific individuals or devices across sessions.

---

## 10. Milestones (do one at a time)

1. **Hello hardware:** project builds and flashes. Display text, read keys, play a 440Hz test tone through the ES8311 speaker with a volume control. Log heap. *(Prove the audio path before anything else.)*
2. **Audio engine core:** audio task on core 1, block renderer, one sine voice, sine LUT, limiter. Keyboard plays notes. No clicks.
3. **Wi-Fi capture:** promiscuous mode, channel hopping, `SignalEvent` queue, serial dump of aggregated stats (count per channel, avg RSSI). Hashed IDs only.
4. **First sonification:** voice manager + FM voice. Wi-Fi events become pitched, RSSI-scaled tones. Scale quantisation working.
5. **Full voice set:** pad, pluck, shimmer, bell voices; per-family limits; voice stealing; smooth parameters.
6. **BLE + scheduler:** NimBLE passive scanning, time-sliced with Wi-Fi, radio modes, lingering release.
7. **FX:** reverb and master chain tuned. Profile CPU; add `esp-dsp` optimisations only if needed.
8. **UI:** constellation, waterfall, status, settings screens.
9. **IMU + mic:** tilt control, shake scatter, time-multiplexed ambient sensing.
10. **SD:** WAV recorder via ring buffer, presets save/load.
11. **Polish:** battery behaviour, boot screen, README, defaults, stability soak test (run 30+ minutes, watch heap and audio underruns).

**Acceptance criteria for "done":**
- Runs for 30+ minutes with no crash, no heap leak, no audible dropouts.
- Audio task CPU on core 1 stays under about 70% with the full voice pool.
- Free heap stays above about 40KB during normal operation.
- Recording produces a valid WAV that plays on a PC.
- Walking from a quiet room to a busy area audibly changes the texture.

---

## 11. Estimated memory budget (rough, to be measured)

| Item | Approx |
|---|---|
| Wi-Fi stack (promiscuous) | 40–60KB |
| NimBLE | 30–50KB |
| Display sprite (RGB565 240x135) | about 65KB (or about 32KB at 8-bit) |
| Audio buffers (3 x 256 samples) | about 2KB |
| Reverb buffers | 12–24KB |
| Pluck delay lines (6 x 512 x 2B) | about 6KB |
| Recorder ring buffer | 16–32KB |
| Voice pool + tables | under 10KB |

These are guesses. Measure and report real numbers at milestones 3, 6, 8 and 10. If heap gets tight, reduce the sprite depth first, then the recorder buffer, then reverb size.

---

## 12. Testing and debugging aids

- Add a `DEBUG_STATS` build flag that prints once per second: events/sec per source type, active voices by family, audio task CPU %, buffer underruns, free heap, queue drops.
- Add a **"simulator" mode** (key toggle) that generates fake `SignalEvent`s so the synth can be tuned indoors without radio traffic.
- Keep a `test_tones` function for isolating audio-path problems.
- Count and report I2S/audio underruns; any underrun is a bug to fix, not to ignore.

---

## 13. Kick-off prompt (send this after the file is loaded)

> Read GEMINI.md fully. Start with **Milestone 1** only. Check the installed M5Cardputer/M5Unified libraries for ADV (ES8311 / TCA8418 / BMI270) support and tell me what you find before writing code. Then give me a `platformio.ini`, a `main.cpp` that shows text on the display, reads the keyboard, and plays a 440Hz test tone with volume control, plus build/flash/test instructions. List any assumptions or `TODO(verify)` items. Don't proceed to Milestone 2 until I confirm.
