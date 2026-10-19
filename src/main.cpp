#include <M5Cardputer.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <memory>
#include <stdint.h>
#include <utility>
#include <vector>
#include "config.h"
#include "choir.h"
#include "input_keys.h"
#include "radio.h"
#include "radio_traffic.h"
#include "synth/engine.h"
#include "ui.h"
#include "voice_manager.h"

// Ether Choir main loop (Milestone 8): passive radio sonification plus
// four UI screens (constellation, waterfall, status, settings).
// No recorder in M8. Privacy: aggregate metadata only, never raw addresses.

namespace {

constexpr uint8_t kVolumeStep = 8;
constexpr uint32_t kHeapLogIntervalMs = 5000;
constexpr uint32_t kRadioStatsIntervalMs = 1000;
constexpr uint16_t kKeyboardGainQ15 = 24000;
constexpr uint16_t kKeyboardFmIndexQ15 = 1200;

bool toneEnabled = false;
bool previousMinusDown = false;
bool previousEqualsDown = false;
bool previousSpaceDown = false;
bool previousWDown = false;
bool previousBDown = false;
bool previousEDown = false;
bool previousSemicolonDown = false;
bool previousBracketLeftDown = false;
bool previousBracketRightDown = false;
bool previousCommaDown = false;
bool previousPeriodDown = false;
bool previousTDown = false;
uint32_t lastHeapLogMs = 0;
uint32_t lastRadioStatsMs = 0;
uint16_t previousNoteMask = 0;
uint16_t previousDigitMask = 0;
uint8_t previousKeySignature = 0xFF;
int8_t currentNoteIndex = -1;
uint32_t channelCounts[14]{};
int32_t channelRssiTotals[14]{};
uint32_t bleCount = 0;
uint32_t lastUiDrawMs = 0;
uint32_t lastUiFrameUs = 0;
uint32_t lastSelfTestMs = 0;
bool uiDirty = true;

constexpr char kNoteKeys[] = {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l'};
constexpr uint32_t kNoteFrequenciesMilliHz[] = {
    261626, 311127, 349228, 391995, 466164, 523251, 622254, 698456, 783991
};
constexpr const char* kNoteNames[] = {
    "C4", "Eb4", "F4", "G4", "Bb4", "C5", "Eb5", "F5", "G5"
};

bool isKeyDown(const std::vector<char>& keys, char key) {
    for (char currentKey : keys) {
        if (currentKey == key) {
            return true;
        }
    }
    return false;
}

// Prints the keys currently held, with their mapped characters, so key
// presses and releases can be seen in the serial log.
void logKeyState() {
    const auto& keyList = M5Cardputer.Keyboard.keyList();
    char buffer[64];
    size_t used = 0;
    for (const auto& point : keyList) {
        const char c = static_cast<char>(M5Cardputer.Keyboard.getKey(point));
        if (c > 31 && c < 127) {
            if (used + 2 >= sizeof(buffer)) break;
            buffer[used++] = c;
        } else {
            const int written = snprintf(buffer + used, sizeof(buffer) - used,
                                         "[%d,%d]", point.x, point.y);
            if (written < 0 || used + static_cast<size_t>(written) >= sizeof(buffer)) break;
            used += static_cast<size_t>(written);
        }
    }
    buffer[used] = '\0';
    Serial.printf("Keys: %u '%s'\n", static_cast<unsigned>(keyList.size()), buffer);
}

void logHeap() {
    Serial.printf("Free heap: %u bytes; largest 8-bit block: %u bytes\n",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

    const uint64_t renderUs = audioEngineGetRenderTimeUs();
    const uint32_t blockPeriodUs = 256ULL * 1000000ULL / 22050ULL;  // ~11610 us
    const uint32_t cpuPct = static_cast<uint32_t>((renderUs * 100ULL) / blockPeriodUs);
    Serial.printf("Audio render: %llu us (%lu%% CPU)\n",
                  static_cast<unsigned long long>(renderUs),
                  static_cast<unsigned long>(cpuPct));

    const auto& keyList = M5Cardputer.Keyboard.keyList();
    Serial.printf("Keys held: %u\n", static_cast<unsigned>(keyList.size()));
}

// A new device has joined the choir. Only its part and note are printed, so
// nothing can be tied back to the device itself.
void logChoirJoin(uint8_t part) {
    Serial.printf("Choir: part %u (%s) joined, %u members\n",
                  static_cast<unsigned>(part), choirNoteNameForPart(part),
                  static_cast<unsigned>(choirMemberCount()));
}

// Refreshes the roster once a second: the devices associated with the local
// access point are picked up as connected members, and the state of the choir
// is printed.
void refreshChoir() {
    uint32_t clientHashes[4]{};
    const uint8_t clientCount = trafficCopyClientHashes(clientHashes, 4);
    choirSetAccessPointClients(clientHashes, clientCount, kTrafficChannel, millis());

    Serial.printf("Choir: %u members, %u connected, %u advertising, %u clients, %u local\n",
                  static_cast<unsigned>(choirMemberCount()),
                  static_cast<unsigned>(choirRoleCount(ChoirRole::Active)),
                  static_cast<unsigned>(choirRoleCount(ChoirRole::Advertising)),
                  static_cast<unsigned>(choirRoleCount(ChoirRole::Client)),
                  static_cast<unsigned>(choirRoleCount(ChoirRole::Local)));
}

void logBoard() {
    // M5GFX board ids: 0 = unknown, 14 = Cardputer, 24 = Cardputer ADV.
    Serial.printf("Board id: %d (0=unknown 14=Cardputer 24=CardputerADV)\n",
                  static_cast<int>(M5.getBoard()));
    Serial.printf("Display: %dx%d\n", static_cast<int>(M5Cardputer.Display.width()),
                  static_cast<int>(M5Cardputer.Display.height()));
}

void collectRadioEvents() {
    SignalEvent event{};
    while (radioTryReceive(event)) {
        if (event.type == SrcType::BleAdv) {
            ++bleCount;
        } else if (event.channel >= 1 && event.channel <= 13) {
            ++channelCounts[event.channel];
            channelRssiTotals[event.channel] += event.rssi;
        }

        VoiceCommand command{};
        if (voiceManagerMapEvent(event, command)) {
            audioEnginePushVoiceCommand(command);
        }
        const uint8_t joinedPart = choirNoteEvent(event, millis());
        if (joinedPart != 0) {
            logChoirJoin(joinedPart);
        }
        uiNoteDevice(event, millis());
    }
}

void logRadioStats() {
    bool anyEvents = false;
    for (uint8_t channel = 1; channel <= 13; ++channel) {
        const uint32_t count = channelCounts[channel];
        if (count == 0) {
            continue;
        }

        anyEvents = true;
        const int32_t averageRssi = channelRssiTotals[channel] / static_cast<int32_t>(count);
        Serial.printf("Wi-Fi channel %u: packets=%lu avg RSSI=%ld dBm\n",
                      channel, static_cast<unsigned long>(count), static_cast<long>(averageRssi));
    }
    if (bleCount != 0) {
        anyEvents = true;
        Serial.printf("BLE advertisements: packets=%lu\n",
                      static_cast<unsigned long>(bleCount));
    }
    if (!anyEvents) {
        Serial.println("Radio: no metadata events this second.");
    }
    Serial.printf("Radio mode: %s\n",
                  radioGetMode() == RadioMode::WifiOnly   ? "Wi-Fi"
                : radioGetMode() == RadioMode::BleOnly    ? "BLE"
                                                          : "Wi-Fi+BLE");
    Serial.printf("Wi-Fi queue drops: %lu\n", static_cast<unsigned long>(radioQueueDrops()));
    Serial.printf("Audio command drops: %lu\n",
                  static_cast<unsigned long>(audioEngineVoiceQueueDrops()));
    Serial.printf("Audio peak: %u  UI frame: %lu us  ch: %u  tx: %lu\n",
                  static_cast<unsigned>(audioEngineGetPeakLevel()),
                  static_cast<unsigned long>(lastUiFrameUs),
                  static_cast<unsigned>(radioGetCurrentChannel()),
                  static_cast<unsigned long>(radioGetTrafficPacketCount()));
    Serial.printf("UI: scene %lu us, push %lu us\n",
                  static_cast<unsigned long>(uiLastRenderUs()),
                  static_cast<unsigned long>(uiLastPushUs()));

    refreshChoir();

    uiPushWaterfallColumn(channelCounts, bleCount);
    uiDirty = true;
    for (uint8_t channel = 1; channel <= 13; ++channel) {
        channelCounts[channel] = 0;
        channelRssiTotals[channel] = 0;
    }
    bleCount = 0;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(100);

    esp_log_level_set("*", ESP_LOG_INFO);

    // Start without the stock keyboard reader: it attaches a GPIO interrupt
    // to its own object, which would dangle once the reader is replaced.
    M5Cardputer.begin(false);
    // Install our own ADV reader, which drains the expander's event FIFO on
    // every poll, so no press or release event is ever lost.
    auto reader = std::make_unique<Tca8418AdvKeyboardReader>();
    Tca8418AdvKeyboardReader* readerPtr = reader.get();
    M5Cardputer.Keyboard.begin(std::move(reader));
    if (readerPtr->selfTest()) {
        Serial.println("Keyboard self-test: PASS");
    } else {
        Serial.println("Keyboard self-test: FAIL");
    }
    logBoard();
    M5Cardputer.Speaker.setVolume(uiVolume());

    // The back buffer is allocated first so the boot screen is drawn the same
    // way as every other frame.
    uiInit();
    uiBootScreen();
    delay(1200);

    if (!audioEngineStart()) {
        toneEnabled = false;
        Serial.println("Speaker initialisation failed.");
    }
    // The radio starts after the boot screen, so the backlog of packets that
    // arrives while the splash is on screen cannot overflow the event queue.
    if (!radioBegin()) {
        Serial.println("Passive Wi-Fi/BLE capture failed to start.");
    }

    uiDraw(uiVolume(), "A440", toneEnabled, millis());
    lastUiDrawMs = millis();
    uiDirty = false;
    logHeap();
    lastHeapLogMs = millis();
    lastRadioStatsMs = millis();

    if (config::kUiSelfTest) {
        uiSetTrafficEnabled(true);
        Serial.println("[selftest] local traffic generator enabled");
    }
}

void loop() {
    M5Cardputer.update();
    // The keyboard is driven by our own reader, so poll it explicitly.
    M5Cardputer.Keyboard.updateKeyList();
    M5Cardputer.Keyboard.updateKeysState();
    audioEnginePump();
    collectRadioEvents();

    const auto& keys = M5Cardputer.Keyboard.keysState();
    uint8_t keySignature = static_cast<uint8_t>(keys.word.size());
    for (char c : keys.word) {
        keySignature = static_cast<uint8_t>((keySignature * 31u) + static_cast<uint8_t>(c));
    }
    if (keySignature != previousKeySignature) {
        previousKeySignature = keySignature;
        logKeyState();
    }

    const bool minusDown = isKeyDown(keys.word, '-');
    const bool equalsDown = isKeyDown(keys.word, '=');
    const bool spaceDown = keys.space;
    bool changed = false;
    const uint32_t now = millis();
    uint16_t noteMask = 0;

    for (size_t index = 0; index < sizeof(kNoteKeys); ++index) {
        if (isKeyDown(keys.word, kNoteKeys[index])) {
            noteMask |= static_cast<uint16_t>(1U << index);
        }
    }

    const uint16_t newlyPressed = noteMask & static_cast<uint16_t>(~previousNoteMask);
    if (newlyPressed != 0) {
        for (size_t index = 0; index < sizeof(kNoteKeys); ++index) {
            if (newlyPressed & (1U << index)) {
                currentNoteIndex = static_cast<int8_t>(index);
            }
        }
    } else if (currentNoteIndex >= 0 && (noteMask & (1U << currentNoteIndex)) == 0) {
        currentNoteIndex = -1;
        for (size_t index = 0; index < sizeof(kNoteKeys); ++index) {
            if (noteMask & (1U << index)) {
                currentNoteIndex = static_cast<int8_t>(index);
            }
        }
    }
    if (noteMask != previousNoteMask) {
        changed = true;
        // Log every note edge, so a note that retriggers or hangs on can be
        // seen in the serial log next to the key state log.
        for (size_t index = 0; index < sizeof(kNoteKeys); ++index) {
            const uint16_t bit = static_cast<uint16_t>(1U << index);
            const bool nowDown = (noteMask & bit) != 0;
            const bool wasDown = (previousNoteMask & bit) != 0;
            if (nowDown != wasDown) {
                Serial.printf("Note %s %s\n", kNoteNames[index],
                              nowDown ? "on" : "off");
            }
        }
    }
    previousNoteMask = noteMask;

    if (minusDown && !previousMinusDown && uiVolume() > 0) {
        const uint8_t next = uiVolume() > kVolumeStep ? uiVolume() - kVolumeStep : 0;
        uiSetVolume(next);
        M5Cardputer.Speaker.setVolume(uiVolume());
        changed = true;
    }
    if (equalsDown && !previousEqualsDown && uiVolume() < UINT8_MAX) {
        const uint16_t nextVolume = static_cast<uint16_t>(uiVolume()) + kVolumeStep;
        uiSetVolume(nextVolume > UINT8_MAX ? UINT8_MAX : static_cast<uint8_t>(nextVolume));
        M5Cardputer.Speaker.setVolume(uiVolume());
        changed = true;
    }
    if (spaceDown && !previousSpaceDown) {
        if (toneEnabled) {
            toneEnabled = false;
        } else {
            toneEnabled = true;
        }
        changed = true;
    }

    const bool wDown = isKeyDown(keys.word, 'w');
    const bool bDown = isKeyDown(keys.word, 'b');
    const bool eDown = isKeyDown(keys.word, 'e');
    if (wDown && !previousWDown) {
        radioSetMode(RadioMode::WifiOnly);
        changed = true;
    }
    if (bDown && !previousBDown) {
        radioSetMode(RadioMode::BleOnly);
        changed = true;
    }
    if (eDown && !previousEDown) {
        radioSetMode(RadioMode::Both);
        changed = true;
    }
    const bool semicolonDown = isKeyDown(keys.word, ';');
    if (semicolonDown && !previousSemicolonDown) {
        uiNextScreen();
        uiDirty = true;
    }

    // Radio slice timing. The Wi-Fi slice is kept long enough for one full
    // sweep of the 2.4 GHz band, so a shorter slice would starve the sweep.
    const bool bracketLeftDown = isKeyDown(keys.word, '[');
    const bool bracketRightDown = isKeyDown(keys.word, ']');
    if (bracketLeftDown && !previousBracketLeftDown) {
        uiAdjustWifiSliceMs(-120);
        changed = true;
        uiDirty = true;
    }
    if (bracketRightDown && !previousBracketRightDown) {
        uiAdjustWifiSliceMs(120);
        changed = true;
        uiDirty = true;
    }
    const bool commaDown = isKeyDown(keys.word, ',');
    const bool periodDown = isKeyDown(keys.word, '.');
    if (commaDown && !previousCommaDown) {
        uiAdjustChannelDwellMs(-20);
        changed = true;
        uiDirty = true;
    }
    if (periodDown && !previousPeriodDown) {
        uiAdjustChannelDwellMs(20);
        changed = true;
        uiDirty = true;
    }

    // The local traffic generator transmits, so it is only reachable from the
    // settings screen and is always labelled while it runs.
    const bool tDown = isKeyDown(keys.word, 't');
    if (tDown && !previousTDown && uiCurrentScreen() == UiScreen::Settings) {
        uiToggleTraffic();
        Serial.printf("Local Wi-Fi traffic generator: %s\n",
                      uiTrafficEnabled() ? "ON (transmitting)" : "OFF");
        changed = true;
        uiDirty = true;
    }

    uint16_t digitMask = 0;
    for (int digit = 1; digit <= 5; ++digit) {
        if (isKeyDown(keys.word, static_cast<char>('0' + digit))) {
            digitMask |= static_cast<uint16_t>(1U << (digit - 1));
        }
    }
    const uint16_t newlyPressedDigits = digitMask & static_cast<uint16_t>(~previousDigitMask);
    for (int digit = 1; digit <= 5; ++digit) {
        if (newlyPressedDigits & (1U << (digit - 1))) {
            uiToggleFamily(static_cast<uint8_t>(digit - 1));
            changed = true;
            uiDirty = true;
        }
    }
    previousDigitMask = digitMask;

    previousMinusDown = minusDown;
    previousEqualsDown = equalsDown;
    previousSpaceDown = spaceDown;
    previousWDown = wDown;
    previousBDown = bDown;
    previousEDown = eDown;
    previousSemicolonDown = semicolonDown;
    previousBracketLeftDown = bracketLeftDown;
    previousBracketRightDown = bracketRightDown;
    previousCommaDown = commaDown;
    previousPeriodDown = periodDown;
    previousTDown = tDown;

    const bool keyboardVoiceEnabled = currentNoteIndex >= 0 || toneEnabled;
    const uint32_t keyboardFrequencyMilliHz = currentNoteIndex >= 0
        ? kNoteFrequenciesMilliHz[currentNoteIndex]
        : 440000;
    const uint16_t keyboardFmIndexQ15 = currentNoteIndex >= 0 ? kKeyboardFmIndexQ15 : 0;
    audioEngineSetKeyboardVoice(keyboardFrequencyMilliHz,
                                keyboardVoiceEnabled ? kKeyboardGainQ15 : 0,
                                keyboardFmIndexQ15, keyboardVoiceEnabled);

    if (uiDirty || changed) {
        const char* note = currentNoteIndex >= 0 ? kNoteNames[currentNoteIndex] : "A440";
        const bool tone = toneEnabled;
        uiDirty = false;
        const uint32_t startUs = micros();
        uiDraw(uiVolume(), note, tone, millis());
        lastUiFrameUs = micros() - startUs;
        lastUiDrawMs = millis();
    } else if (uiCurrentScreen() == UiScreen::Scope
               && now - lastUiDrawMs >= config::kUiScopeRefreshIntervalMs) {
        // The scope keeps refreshing so the trace stays live.
        const char* note = currentNoteIndex >= 0 ? kNoteNames[currentNoteIndex] : "A440";
        const uint32_t startUs = micros();
        uiDraw(uiVolume(), note, toneEnabled, now);
        lastUiFrameUs = micros() - startUs;
        lastUiDrawMs = now;
    }

    if (now - lastHeapLogMs >= kHeapLogIntervalMs) {
        logHeap();
        lastHeapLogMs = now;
    }
    if (now - lastRadioStatsMs >= kRadioStatsIntervalMs) {
        logRadioStats();
        lastRadioStatsMs = now;
    }

    if (config::kUiSelfTest) {
        if (now - lastSelfTestMs >= config::kUiSelfTestIntervalMs) {
            lastSelfTestMs = now;
            uiNextScreen();
            uiDirty = true;
        }
    }

    delay(10);
}
