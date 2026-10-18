#include <M5Cardputer.h>
#include <esp_heap_caps.h>
#include <stdint.h>
#include <vector>
#include "config.h"
#include "radio.h"
#include "synth/engine.h"
#include "ui.h"
#include "voice_manager.h"

// Ether Choir main loop (Milestone 8): passive radio sonification plus
// four UI screens (constellation, waterfall, status, settings).
// No recorder in M8. Privacy: aggregate metadata only, never raw addresses.

namespace {

constexpr uint8_t kInitialVolume = 96;
constexpr uint8_t kVolumeStep = 8;
constexpr uint32_t kHeapLogIntervalMs = 5000;
constexpr uint32_t kRadioStatsIntervalMs = 1000;
constexpr uint16_t kKeyboardGainQ15 = 24000;
constexpr uint16_t kKeyboardFmIndexQ15 = 1200;

uint8_t volume = kInitialVolume;
bool toneEnabled = false;
bool previousMinusDown = false;
bool previousEqualsDown = false;
bool previousSpaceDown = false;
bool previousWDown = false;
bool previousBDown = false;
bool previousEDown = false;
uint32_t lastHeapLogMs = 0;
uint32_t lastRadioStatsMs = 0;
uint16_t previousNoteMask = 0;
int8_t currentNoteIndex = -1;
uint32_t channelCounts[14]{};
int32_t channelRssiTotals[14]{};
uint32_t bleCount = 0;
uint32_t lastUiDrawMs = 0;
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

    uiPushWaterfallColumn(channelCounts, bleCount);
    uiDirty = true;
    for (uint8_t channel = 1; channel <= 13; ++channel) {
        channelCounts[channel] = 0;
        channelRssiTotals[channel] = 0;
    }
    bleCount = 0;
}

void refreshUi(const char* noteName) {
    uiDraw(volume, noteName, toneEnabled, millis());
    lastUiDrawMs = millis();
    uiDirty = false;
}
}
void setup() {
    Serial.begin(115200);
    delay(100);

    M5Cardputer.begin();
    M5Cardputer.Speaker.setVolume(volume);

    if (!audioEngineStart()) {
        toneEnabled = false;
        Serial.println("Speaker initialisation failed.");
    }
    if (!radioBegin()) {
        Serial.println("Passive Wi-Fi/BLE capture failed to start.");
    }

    uiInit();
    uiDraw(volume, "A440", toneEnabled, millis());
    lastUiDrawMs = millis();
    uiDirty = false;
    logHeap();
    lastHeapLogMs = millis();
    lastRadioStatsMs = millis();
}

void loop() {
    M5Cardputer.update();
    audioEnginePump();
    collectRadioEvents();

    const auto& keys = M5Cardputer.Keyboard.keysState();
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
    }
    previousNoteMask = noteMask;

    if (minusDown && !previousMinusDown && volume > 0) {
        volume = volume > kVolumeStep ? volume - kVolumeStep : 0;
        M5Cardputer.Speaker.setVolume(volume);
        changed = true;
    }
    if (equalsDown && !previousEqualsDown && volume < UINT8_MAX) {
        const uint16_t nextVolume = static_cast<uint16_t>(volume) + kVolumeStep;
        volume = nextVolume > UINT8_MAX ? UINT8_MAX : static_cast<uint8_t>(nextVolume);
        M5Cardputer.Speaker.setVolume(volume);
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

    previousMinusDown = minusDown;
    previousEqualsDown = equalsDown;
    previousSpaceDown = spaceDown;
    previousWDown = wDown;
    previousBDown = bDown;
    previousEDown = eDown;
    previousSemicolonDown = semicolonDown;

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
        uiDraw(volume, note, tone, millis());
    }

    if (now - lastHeapLogMs >= kHeapLogIntervalMs) {
        logHeap();
        lastHeapLogMs = now;
    }
    if (now - lastRadioStatsMs >= kRadioStatsIntervalMs) {
        logRadioStats();
        lastRadioStatsMs = now;
    }

    delay(10);
}
