// UI screens (Milestone 8): static pools, display writes from loop() only.
// All positions derive from salted hashes; no raw metadata is shown.
#include "ui.h"

#include <M5Cardputer.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "radio.h"
#include "synth/engine.h"

namespace {

UiScreen g_screen = UiScreen::Constellation;

struct ConstellationDot {
    uint32_t idHash = 0;
    int8_t rssi = -80;
    bool isBle = false;
    uint32_t lastSeenMs = 0;
    bool used = false;
};

ConstellationDot g_dots[config::kUiConstellationPoolSize];

constexpr uint8_t kWaterfallRows = 14;
uint8_t g_waterfall[config::kUiWaterfallColumns][kWaterfallRows]{};
uint8_t g_waterfallHead = 0;

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kAmber = 0xFD20;
constexpr uint16_t kCyan = 0x07FF;
constexpr uint16_t kDimGrey = 0x4208;

void drawHeader(const char* title) {
    auto& display = M5Cardputer.Display;
    display.fillScreen(kBlack);
    display.setTextColor(kWhite, kBlack);
    display.setTextSize(1);
    display.drawString(title, 6, 6);
    char screenLine[16];
    snprintf(screenLine, sizeof(screenLine), "[;] %u/4",
             static_cast<unsigned>(g_screen) + 1);
    display.drawString(screenLine, 200, 6);
}

uint16_t rssiColour(int8_t rssi, uint16_t fullColour) {
    if (rssi >= -55) return fullColour;
    if (rssi >= -70) return kWhite;
    return kDimGrey;
}

void drawConstellation(uint32_t nowMs) {
    auto& display = M5Cardputer.Display;
    drawHeader("A CONSTELLATION");
    display.drawString("Passive listening, metadata only", 6, 20);
    uint8_t shown = 0;
    for (const ConstellationDot& dot : g_dots) {
        if (!dot.used) continue;
        if (nowMs - dot.lastSeenMs >= config::kUiConstellationFadeMs) continue;
        const uint16_t x = 8 + (dot.idHash % 224);
        const uint16_t y = 32 + ((dot.idHash >> 8) % 96);
        display.drawPixel(x, y, rssiColour(dot.rssi, dot.isBle ? kCyan : kAmber));
        ++shown;
    }
    char countLine[32];
    snprintf(countLine, sizeof(countLine), "Devices: %u (20s fade)", shown);
    display.drawString(countLine, 6, 122);
}


void drawWaterfall() {
    auto& display = M5Cardputer.Display;
    drawHeader("B WATERFALL");
    for (uint8_t col = 0; col < config::kUiWaterfallColumns; ++col) {
        const uint8_t ringIndex =
            (g_waterfallHead + col) % config::kUiWaterfallColumns;
        for (uint8_t row = 0; row < kWaterfallRows; ++row) {
            const uint8_t level = g_waterfall[ringIndex][row];
            if (level == 0) continue;
            const uint16_t x = 6 + col * 3;
            const uint16_t y = 24 + row * 7;
            const uint16_t colour = row == 13 ? kCyan : kAmber;
            display.fillRect(x, y, 2, level > 8 ? 6 : 4, colour);
        }
    }
    display.drawString("Ch 1-13 + BLE (left=old)", 6, 122);
}

void drawStatus(uint8_t volume, const char* noteName, bool toneEnabled) {
    auto& display = M5Cardputer.Display;
    drawHeader("C STATUS");
    char line[40];
    const RadioMode mode = radioGetMode();
    const char* modeText = (mode == RadioMode::WifiOnly) ? "Wi-Fi"
        : (mode == RadioMode::BleOnly)                   ? "BLE"
                                                         : "Wi-Fi+BLE";
    snprintf(line, sizeof(line), "Radio: %s  Vol: %u", modeText, volume);
    display.drawString(line, 6, 22);
    snprintf(line, sizeof(line), "Voices: %lu  Note: %s",
             (unsigned long)audioEngineActiveVoiceCount(), noteName);
    display.drawString(line, 6, 36);
    snprintf(line, sizeof(line), "Tone: %s  C min pent",
             toneEnabled ? "440 ON" : "OFF");
    display.drawString(line, 6, 50);
    const uint64_t renderUs = audioEngineGetRenderTimeUs();
    const uint32_t cpuPct = (uint32_t)((renderUs * 100ULL) / 11610ULL);
    snprintf(line, sizeof(line), "Render: %lu us (%lu%%)",
             (unsigned long)renderUs, (unsigned long)cpuPct);
    display.drawString(line, 6, 64);
    snprintf(line, sizeof(line), "Heap: %u free",
             (unsigned)ESP.getFreeHeap());
    display.drawString(line, 6, 78);
    snprintf(line, sizeof(line), "Q drops: r%lu a%lu",
             (unsigned long)radioQueueDrops(),
             (unsigned long)audioEngineVoiceQueueDrops());
    display.drawString(line, 6, 92);
    const int32_t battery = M5Cardputer.Power.getBatteryLevel();
    snprintf(line, sizeof(line), "Battery: %ld%%  REC: n/a", (long)battery);
    display.drawString(line, 6, 106);
    display.drawString("[w] Wi-Fi [b] BLE [e] both", 6, 120);
}

void drawSettings(uint8_t volume) {
    auto& display = M5Cardputer.Display;
    drawHeader("D SETTINGS");
    char line[40];
    snprintf(line, sizeof(line), "Wi-Fi slice: %lu ms",
             (unsigned long)config::kWifiSliceMs);
    display.drawString(line, 6, 24);
    snprintf(line, sizeof(line), "BLE slice: %lu ms",
             (unsigned long)config::kBleSliceMs);
    display.drawString(line, 6, 38);
    snprintf(line, sizeof(line), "Chan dwell: %lu ms",
             (unsigned long)config::kChannelDwellMs);
    display.drawString(line, 6, 52);
    snprintf(line, sizeof(line), "Volume: %u / 255", volume);
    display.drawString(line, 6, 66);
    display.drawString("Reverb: off (bypassed)", 6, 80);
    display.drawString("Brightness: default", 6, 94);
    display.drawString("Settings display-only in M8", 6, 108);
    display.drawString("[-] quieter [=] louder", 6, 122);
}

}  // namespace

void uiInit() {
    g_screen = UiScreen::Constellation;
    for (uint8_t i = 0; i < config::kUiConstellationPoolSize; ++i) {
        g_dots[i].used = false;
    }
    for (uint8_t col = 0; col < config::kUiWaterfallColumns; ++col) {
        for (uint8_t row = 0; row < kWaterfallRows; ++row) {
            g_waterfall[col][row] = 0;
        }
    }
    g_waterfallHead = 0;
}

void uiNextScreen() {
    const uint8_t next = (static_cast<uint8_t>(g_screen) + 1) % 4;
    g_screen = static_cast<UiScreen>(next);
}

UiScreen uiCurrentScreen() {
    return g_screen;
}

void uiNoteDevice(const SignalEvent& event, uint32_t nowMs) {
    const bool isBle = event.type == SrcType::BleAdv;
    for (uint8_t i = 0; i < config::kUiConstellationPoolSize; ++i) {
        if (g_dots[i].used && g_dots[i].idHash == event.idHash) {
            g_dots[i].rssi = event.rssi;
            g_dots[i].lastSeenMs = nowMs;
            g_dots[i].isBle = isBle;
            return;
        }
    }
    for (uint8_t i = 0; i < config::kUiConstellationPoolSize; ++i) {
        if (!g_dots[i].used) {
            g_dots[i].used = true;
            g_dots[i].idHash = event.idHash;
            g_dots[i].rssi = event.rssi;
            g_dots[i].isBle = isBle;
            g_dots[i].lastSeenMs = nowMs;
            return;
        }
    }
    uint8_t oldest = 0;
    uint32_t oldestAge = 0;
    for (uint8_t i = 0; i < config::kUiConstellationPoolSize; ++i) {
        const uint32_t age = nowMs - g_dots[i].lastSeenMs;
        if (age > oldestAge) {
            oldestAge = age;
            oldest = i;
        }
    }
    g_dots[oldest].idHash = event.idHash;
    g_dots[oldest].rssi = event.rssi;
    g_dots[oldest].isBle = isBle;
    g_dots[oldest].lastSeenMs = nowMs;
    g_dots[oldest].used = true;
}

void uiPushWaterfallColumn(const uint32_t* channelCounts, uint32_t bleCount) {
    for (uint8_t row = 0; row < 13; ++row) {
        const uint32_t c = channelCounts[row + 1];
        g_waterfall[g_waterfallHead][row] = c > 255 ? 255 : (uint8_t)c;
    }
    g_waterfall[g_waterfallHead][13] = bleCount > 255 ? 255 : (uint8_t)bleCount;
    g_waterfallHead = (g_waterfallHead + 1) % config::kUiWaterfallColumns;
}

void uiDraw(uint8_t volume, const char* noteName, bool toneEnabled,
            uint32_t nowMs) {
    switch (g_screen) {
        case UiScreen::Constellation:
            drawConstellation(nowMs);
            break;
        case UiScreen::Waterfall:
            drawWaterfall();
            break;
        case UiScreen::Status:
            drawStatus(volume, noteName, toneEnabled);
            break;
        case UiScreen::Settings:
            drawSettings(volume);
            break;
    }
}
