// UI screens: constellation, waterfall, scope, status, settings.
// Aggregate metadata only; never shows raw addresses, names, or SSIDs.
//
// Every frame is composed on an off-screen sprite and pushed to the panel in a
// single transfer, so the display never shows a partially drawn frame. All
// geometry is derived from the live panel size and every draw call is bounds
// checked, so nothing can land outside the display.
//
// No reverb/echo and no save/load in this build.
#include "ui.h"

#include <M5Cardputer.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "choir.h"
#include "radio.h"
#include "radio_traffic.h"
#include "synth/engine.h"

namespace {

// ---------------------------------------------------------------- colours --

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kAmber = 0xFD20;
constexpr uint16_t kCyan = 0x07FF;
constexpr uint16_t kDimGrey = 0x4208;
constexpr uint16_t kGridGrey = 0x2104;
constexpr uint16_t kGreen = 0x07E0;

// ------------------------------------------------------------ panel metrics -

constexpr uint8_t kHeaderH = 12;     // title band, rows 0..11
constexpr uint8_t kPrivacyY = 13;    // privacy line
constexpr uint8_t kContentTop = 24;  // first row of screen content
constexpr uint8_t kFooterH = 9;      // key hints at the very bottom
constexpr uint8_t kStatsH = 11;      // one line above the footer
constexpr uint8_t kAxisW = 20;       // left gutter for RSSI labels
constexpr uint8_t kBleLaneW = 12;    // right hand lane for BLE sources
constexpr uint8_t kWaterfallGutter = 16;
constexpr uint8_t kScopePoints = 112;

// -------------------------------------------------------------- screen state

UiScreen g_screen = UiScreen::Constellation;

struct ConstellationDot {
    uint32_t idHash = 0;
    uint8_t channel = 0;   // Wi-Fi channel 1..13, 0 for BLE
    int8_t rssi = -80;
    bool isBle = false;
    bool isLocalTx = false;
    uint8_t rate = 0;       // packet rate proxy for dot size
    uint32_t lastSeenMs = 0;
    bool used = false;
};

ConstellationDot g_dots[config::kUiConstellationPoolSize];

// Waterfall rows: Wi-Fi channel 1..13 then BLE.
constexpr uint8_t kWaterfallRows = 14;
uint8_t g_waterfall[config::kUiWaterfallColumns][kWaterfallRows]{};
uint8_t g_waterfallHead = 0;

// ----------------------------------------------------------------- renderer

lgfx::LGFX_Sprite* g_sprite = nullptr;
lgfx::LovyanGFX* g_target = nullptr;

// Live panel geometry, refreshed at init.
uint16_t g_width = 240;
uint16_t g_height = 135;

uint8_t g_boundsReports = 0;

// Oscilloscope state.
int16_t g_waveBuffer[kScopePoints]{};
uint32_t g_scopeGain = 2048;
uint16_t g_scopePeakHold = 0;
uint32_t g_scopePeakHoldMs = 0;

// Frame timing: how long the scene took to compose and how long the push to
// the panel took. Both are reported in the serial log.
uint32_t g_lastRenderUs = 0;
uint32_t g_lastPushUs = 0;

// Runtime settings state.
uint8_t g_volume = 96;
bool g_familyEnabled[6] = {true, true, true, true, true, true}; // Pad, Pluck, Shimmer, Bell, Accent, Keyboard
uint32_t g_wifiSliceMs = config::kWifiSliceMs;
uint32_t g_bleSliceMs = config::kBleSliceMs;
uint32_t g_dwellMs = config::kChannelDwellMs;

constexpr uint32_t kMinWifiSliceMs = 260;
constexpr uint32_t kMaxWifiSliceMs = 8000;
constexpr uint32_t kWifiSliceStepMs = 120;
constexpr uint32_t kMinBleSliceMs = 100;
constexpr uint32_t kMaxBleSliceMs = 4000;
constexpr uint32_t kBleSliceStepMs = 100;
constexpr uint32_t kMinDwellMs = 40;
constexpr uint32_t kMaxDwellMs = 1000;
constexpr uint32_t kDwellStepMs = 20;

// --------------------------------------------------------------- primitives

lgfx::LovyanGFX& gfx() {
    return *g_target;
}

int contentBottom() {
    return static_cast<int>(g_height) - static_cast<int>(kFooterH) - static_cast<int>(kStatsH);
}

int footerY() {
    return static_cast<int>(g_height) - static_cast<int>(kFooterH);
}

int statsY() {
    return footerY() - static_cast<int>(kStatsH);
}

// Bottom of the constellation plot area, above the channel number row.
int plotBottom() {
    return contentBottom() - 13;
}

bool fits(int x, int y, int w, int h) {
    return x >= 0 && y >= 0 &&
           x + w <= static_cast<int>(g_width) &&
           y + h <= static_cast<int>(g_height);
}

void reportBounds(const char* what, int x, int y, int w, int h) {
    if (g_boundsReports >= config::kUiBoundsReportLimit) {
        return;
    }
    ++g_boundsReports;
    Serial.printf("UI %s clipped: x=%d y=%d w=%d h=%d (panel %dx%d)\n",
                  what, x, y, w, h, g_width, g_height);
}

void drawText(int x, int y, const char* text, uint16_t fg = kWhite, uint16_t bg = kBlack) {
    if (text == nullptr || *text == '\0') {
        return;
    }
    const int w = gfx().textWidth(text);
    if (!fits(x, y, w, 8)) {
        reportBounds("text", x, y, w, 8);
        return;
    }
    gfx().setTextColor(fg, bg);
    gfx().drawString(text, x, y);
}

void fillRect(int x, int y, int w, int h, uint16_t colour) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (!fits(x, y, w, h)) {
        reportBounds("rect", x, y, w, h);
        return;
    }
    gfx().fillRect(x, y, w, h, colour);
}

void hLine(int x, int y, int w, uint16_t colour) {
    fillRect(x, y, w, 1, colour);
}

void vLine(int x, int y, int h, uint16_t colour) {
    fillRect(x, y, 1, h, colour);
}

// ------------------------------------------------------------------- header

constexpr uint8_t kScreenCount = 6;

void drawHeader(const char* title) {
    gfx().fillScreen(kBlack);
    gfx().setTextSize(1);
    drawText(4, 2, title);

    char screenLine[16];
    snprintf(screenLine, sizeof(screenLine), "[;] %u/%u",
             static_cast<unsigned>(g_screen) + 1, static_cast<unsigned>(kScreenCount));
    const int x = static_cast<int>(g_width) - 4 - gfx().textWidth(screenLine);
    drawText(x > 0 ? x : 0, 2, screenLine);

    hLine(0, kHeaderH - 1, static_cast<int>(g_width), kGridGrey);
}

void drawPrivacyLine() {
    drawText(4, kPrivacyY, "Passive, metadata only", kDimGrey);
}

// A local transmitter is always advertised while it is running.
void drawTxBadge() {
    if (!radioGetTrafficEnabled()) {
        return;
    }
    const int w = gfx().textWidth("LOCAL TX");
    fillRect(static_cast<int>(g_width) - 5 - w, kPrivacyY - 1, w + 2, 10, kAmber);
    drawText(static_cast<int>(g_width) - 4 - w, kPrivacyY, "LOCAL TX", kBlack);
}

// ------------------------------------------------------------------ helpers

uint16_t rssiColour(int8_t rssi, uint16_t fullColour) {
    if (rssi >= -55) return fullColour;
    if (rssi >= -70) return kWhite;
    return kDimGrey;
}

// Map Wi-Fi channel 1..13 to an x position across the plot width.
uint16_t channelX(uint8_t channel) {
    if (channel < 1 || channel > 13) return 0;
    const int left = kAxisW;
    const int right = static_cast<int>(g_width) - static_cast<int>(kBleLaneW);
    if (right <= left) {
        return static_cast<uint16_t>(left);
    }
    return static_cast<uint16_t>(left + (channel - 1) * (right - left) / 12);
}

// Map RSSI -95..-30 to a y position within the plot area.
uint16_t rssiY(int8_t rssi) {
    int16_t r = rssi;
    if (r < -95) r = -95;
    if (r > -30) r = -30;
    const int top = kContentTop;
    const int bottom = plotBottom();
    const int span = bottom - top;
    if (span <= 0) {
        return static_cast<uint16_t>(top);
    }
    return static_cast<uint16_t>(bottom - (r + 95) * span / 65);
}

uint8_t dotSize(uint8_t rate) {
    if (rate >= 16) return 3;
    if (rate >= 8) return 2;
    return 1;
}

// Blue -> amber -> white heat ramp for the waterfall.
uint16_t heatColour(uint8_t level) {
    if (level == 0) return kBlack;
    if (level < 64) {
        const uint8_t t = static_cast<uint8_t>(level >> 1);   // 0..31
        return static_cast<uint16_t>((t << 10) | (t << 5) | t);
    }
    if (level < 176) {
        const uint8_t t = static_cast<uint8_t>((level - 64) >> 3);  // 0..13
        return static_cast<uint16_t>(0xFC00 | (t << 5) | (t >> 2));
    }
    return kWhite;
}

// ------------------------------------------------------------- A: sky view

void drawConstellation(uint32_t nowMs) {
    drawHeader("A CONSTELLATION");
    drawPrivacyLine();
    drawTxBadge();

    const int top = kContentTop;
    const int bottom = plotBottom();
    const int left = kAxisW;
    const int right = static_cast<int>(g_width) - static_cast<int>(kBleLaneW);
    const int plotH = bottom - top;

    // RSSI grid lines and labels.
    static const int kRssiMarks[] = {-40, -55, -70, -85};
    for (const int mark : kRssiMarks) {
        const int y = rssiY(static_cast<int8_t>(mark));
        hLine(left, y, right - left, kGridGrey);
        char label[8];
        snprintf(label, sizeof(label), "%d", mark);
        const int w = gfx().textWidth(label);
        drawText(left - 1 - w, y - 4, label, kDimGrey);
    }

    // Channel ticks and the BLE lane separator.
    for (uint8_t channel = 1; channel <= 13; ++channel) {
        const int x = channelX(channel);
        vLine(x, bottom + 1, 2, kGridGrey);
        if (channel == 1 || channel == 13 || channel % 3 == 0) {
            char label[4];
            snprintf(label, sizeof(label), "%u", static_cast<unsigned>(channel));
            const int w = gfx().textWidth(label);
            drawText(x - w / 2, bottom + 4, label, kDimGrey);
        }
    }
    vLine(right + 1, top, plotH, kGridGrey);

    // Sources.
    uint8_t shown = 0;
    uint8_t bleShown = 0;
    uint8_t localShown = 0;
    for (const ConstellationDot& dot : g_dots) {
        if (!dot.used) continue;
        if (nowMs - dot.lastSeenMs >= config::kUiConstellationFadeMs) continue;
        ++shown;
        uint16_t colour;
        int x;
        int y;
        if (dot.isBle || dot.channel == 0) {
            x = right + 3;
            y = rssiY(dot.rssi);
            colour = rssiColour(dot.rssi, kCyan);
            ++bleShown;
        } else {
            x = channelX(dot.channel);
            y = rssiY(dot.rssi);
            colour = rssiColour(dot.rssi, kAmber);
        }
        if (dot.isLocalTx) {
            colour = kGreen;
            ++localShown;
        }
        const uint8_t size = dotSize(dot.rate);
        fillRect(x - size / 2, y - size / 2, size, size, colour);
    }

    char line[48];
    snprintf(line, sizeof(line), "Devices: %u  BLE: %u  Local: %u",
             static_cast<unsigned>(shown), static_cast<unsigned>(bleShown),
             static_cast<unsigned>(localShown));
    drawText(4, statsY(), line);

    drawText(4, footerY(), "x = ch 1-13, y = RSSI, BLE right");
}

// ---------------------------------------------------------- B: waterfall --

void drawWaterfall() {
    drawHeader("B WATERFALL");
    drawPrivacyLine();
    drawTxBadge();

    const int top = kContentTop;
    const int bottom = contentBottom();
    const int left = kWaterfallGutter;
    const int right = static_cast<int>(g_width) - 3;
    const int plotW = right - left;
    const int plotH = bottom - top;
    const int rowH = plotH / kWaterfallRows;
    const int barH = rowH > 1 ? rowH - 1 : 1;
    const int colStep = 2;
    int visible = colStep > 0 ? plotW / colStep : 0;
    if (visible > static_cast<int>(config::kUiWaterfallColumns)) {
        visible = static_cast<int>(config::kUiWaterfallColumns);
    }

    // Row labels: 1..13 for the channels, B for BLE.
    for (uint8_t row = 0; row < kWaterfallRows; ++row) {
        char label[4];
        if (row < 13) {
            snprintf(label, sizeof(label), "%u", static_cast<unsigned>(row + 1));
        } else {
            snprintf(label, sizeof(label), "B");
        }
        const int w = gfx().textWidth(label);
        drawText(left - 3 - w, top + row * rowH, label, kDimGrey);
    }

    for (int col = 0; col < visible; ++col) {
        const int ringIndex =
            (g_waterfallHead + static_cast<int>(config::kUiWaterfallColumns)
             - visible + col) % static_cast<int>(config::kUiWaterfallColumns);
        const int x = left + col * colStep;
        for (uint8_t row = 0; row < kWaterfallRows; ++row) {
            const uint8_t level = g_waterfall[ringIndex][row];
            if (level == 0) continue;
            fillRect(x, top + row * rowH, colStep, barH, heatColour(level));
        }
    }

    drawText(4, statsY(), "1s per column, newest right");
    drawText(4, footerY(), "rows: ch 1-13 then BLE");
}

// ------------------------------------------------------------- C: scope --

void drawScope(uint32_t nowMs, const char* noteName) {
    drawHeader("C SCOPE");
    drawPrivacyLine();
    drawTxBadge();

    const int top = kContentTop;
    const int bottom = contentBottom() - 12;
    const int left = 4;
    const int right = static_cast<int>(g_width) - 4;
    const int height = bottom - top;
    const int centre = top + height / 2;
    const int half = height / 2 - 1;

    // Grid.
    hLine(left, centre, right - left, kGridGrey);
    for (int x = left + 32; x < right; x += 32) {
        vLine(x, top, height, kGridGrey);
    }
    vLine(left, top, height, kGridGrey);
    vLine(right, top, height, kGridGrey);

    // Pull the newest decimated samples and scale them to the panel.
    const uint16_t points = audioEngineCopyWaveform(g_waveBuffer, kScopePoints);
    const uint16_t peak = audioEngineGetPeakLevel();
    uint32_t targetGain = peak > 512 ? peak : 512;
    g_scopeGain += (targetGain - g_scopeGain) / 8;
    if (g_scopeGain < 256) {
        g_scopeGain = 256;
    }
    const int32_t gain = static_cast<int32_t>(g_scopeGain);

    if (points > 1) {
        const int step = (right - left) / (static_cast<int>(points) - 1);
        int previousY = centre;
        for (uint16_t i = 0; i < points; ++i) {
            const int x = left + static_cast<int>(i) * step;
            int32_t value = g_waveBuffer[i];
            value = (value * half) / gain;
            if (value > half) value = half;
            if (value < -half) value = -half;
            const int y = centre - static_cast<int>(value);
            if (i > 0) {
                const int yLow = previousY < y ? previousY : y;
                const int yHigh = previousY < y ? y : previousY;
                vLine(x, yLow, yHigh - yLow + 1, kAmber);
            }
            previousY = y;
        }
    }

    // Peak hold marker.
    if (peak >= g_scopePeakHold) {
        g_scopePeakHold = peak;
        g_scopePeakHoldMs = nowMs;
    } else if (nowMs - g_scopePeakHoldMs > 1500) {
        g_scopePeakHold = static_cast<uint16_t>((g_scopePeakHold * 7) / 8);
    }
    int peakY = centre - ((static_cast<int32_t>(g_scopePeakHold) * half) / gain);
    if (peakY < top) peakY = top;
    if (peakY > bottom) peakY = bottom;
    hLine(left, peakY, right - left, kCyan);

    char line[48];
    int dbfs = -99;
    if (g_scopePeakHold > 0) {
        dbfs = static_cast<int>(20.0 * log10(static_cast<double>(g_scopePeakHold) / 32768.0));
        if (dbfs < -99) dbfs = -99;
    }
    snprintf(line, sizeof(line), "Peak %d dBFS  Note: %s  Keys: %u",
             dbfs, noteName != nullptr ? noteName : "-",
             static_cast<unsigned>(M5Cardputer.Keyboard.keyList().size()));
    drawText(4, statsY(), line);
    drawText(4, footerY(), "live output, auto gain");
}

// ------------------------------------------------------------ D: choir ----

// The roster of devices the choir is made of. Each member holds a part, which
// is the note it sounds, for as long as it stays in the roster.
void drawChoir() {
    drawHeader("D CHOIR");
    drawPrivacyLine();
    drawTxBadge();

    const int top = kContentTop;
    constexpr int kRowHeight = 9;
    const int maxRows = (contentBottom() - top) / kRowHeight;

    char line[48];
    uint8_t shown = 0;
    uint8_t overflow = 0;
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        const ChoirMember& member = choirMemberAt(index);
        if (!member.used) {
            continue;
        }
        if (shown >= maxRows) {
            ++overflow;
            continue;
        }
        const uint16_t colour = member.role == ChoirRole::Active      ? kGreen
            : member.role == ChoirRole::Client                       ? kAmber
            : member.role == ChoirRole::Advertising                   ? kCyan
            : member.role == ChoirRole::Local                        ? kGreen
                                                                     : kWhite;
        if (member.channel >= 1 && member.channel <= 13) {
            snprintf(line, sizeof(line), "P%u %-3s %-3s %2uch %4ddBm %4up",
                     static_cast<unsigned>(member.part), choirNoteName(member),
                     choirRoleLabel(member.role),
                     static_cast<unsigned>(member.channel),
                     static_cast<int>(member.rssi),
                     static_cast<unsigned>(member.packets));
        } else {
            snprintf(line, sizeof(line), "P%u %-3s %-3s  --  %4ddBm %4up",
                     static_cast<unsigned>(member.part), choirNoteName(member),
                     choirRoleLabel(member.role),
                     static_cast<int>(member.rssi),
                     static_cast<unsigned>(member.packets));
        }
        drawText(4, top + shown * kRowHeight, line, colour);
        ++shown;
    }

    if (overflow > 0) {
        snprintf(line, sizeof(line), "+%u more", static_cast<unsigned>(overflow));
        drawText(4, top + shown * kRowHeight, line, kDimGrey);
    }

    snprintf(line, sizeof(line), "Mem %u  Conn %u  Adv %u  Cli %u  V %u/%u",
             static_cast<unsigned>(choirMemberCount()),
             static_cast<unsigned>(choirRoleCount(ChoirRole::Active)),
             static_cast<unsigned>(choirRoleCount(ChoirRole::Advertising)),
             static_cast<unsigned>(choirRoleCount(ChoirRole::Client)),
             static_cast<unsigned>(audioEngineActiveVoiceCount()),
             static_cast<unsigned>(audioEngineVoiceCapacity()));
    drawText(4, statsY(), line);
    drawText(4, footerY(), "part = note, held while the device stays");
}

// ---------------------------------------------------------- E: status ----

void drawStatus(uint8_t volume, const char* noteName, bool toneEnabled) {
    drawHeader("E STATUS");
    drawPrivacyLine();
    drawTxBadge();

    char line[48];
    const RadioMode mode = radioGetMode();
    const char* modeText = (mode == RadioMode::WifiOnly) ? "Wi-Fi"
        : (mode == RadioMode::BleOnly)                   ? "BLE"
                                                         : "Wi-Fi+BLE";
    snprintf(line, sizeof(line), "Radio: %s   Vol: %u", modeText, volume);
    drawText(4, kContentTop, line);

    snprintf(line, sizeof(line), "Voices: %u  Keys: %u  Note: %s",
             static_cast<unsigned>(audioEngineActiveVoiceCount()),
             static_cast<unsigned>(M5Cardputer.Keyboard.keyList().size()),
             noteName);
    drawText(4, kContentTop + 11, line);

    snprintf(line, sizeof(line), "Tone: %s   Scale: C min pent",
             toneEnabled ? "440 ON" : "OFF");
    drawText(4, kContentTop + 22, line);

    const uint64_t renderUs = audioEngineGetRenderTimeUs();
    const uint32_t cpuPct = static_cast<uint32_t>((renderUs * 100ULL) / 11610ULL);
    snprintf(line, sizeof(line), "Render: %lu us (%lu%%)",
             static_cast<unsigned long>(renderUs), static_cast<unsigned long>(cpuPct));
    drawText(4, kContentTop + 33, line);

    snprintf(line, sizeof(line), "Heap: %u free",
             static_cast<unsigned>(ESP.getFreeHeap()));
    drawText(4, kContentTop + 44, line);

    snprintf(line, sizeof(line), "Drops: r%lu a%lu",
             static_cast<unsigned long>(radioQueueDrops()),
             static_cast<unsigned long>(audioEngineVoiceQueueDrops()));
    drawText(4, kContentTop + 55, line);

    snprintf(line, sizeof(line), "Sweep: 13 ch / %lu ms dwell",
             static_cast<unsigned long>(g_dwellMs));
    drawText(4, kContentTop + 66, line);

    const int32_t battery = M5Cardputer.Power.getBatteryLevel();
    snprintf(line, sizeof(line), "Battery: %ld%%", static_cast<long>(battery));
    drawText(4, kContentTop + 77, line);

    if (radioGetTrafficEnabled()) {
        const char* warn = " LOCAL TX ON - TRANSMITTING ";
        const int w = gfx().textWidth(warn);
        fillRect(2, statsY() - 2, w, 12, kAmber);
        drawText(3, statsY(), warn, kBlack);
    } else {
        drawText(4, statsY(), "Local TX off (passive only)", kDimGrey);
    }
    drawText(4, footerY(), "[w]WiFi [b]BLE [e]both [sp]tone");
}

// -------------------------------------------------------- F: settings ----

void drawSettings() {
    drawHeader("F SETTINGS");
    drawText(4, kPrivacyY, "[-]vol [ ]WiFi ,.dwell t=TX 1-5=fam", kDimGrey);
    drawTxBadge();

    char line[48];
    snprintf(line, sizeof(line), "Wi-Fi slice: %lu ms",
             static_cast<unsigned long>(g_wifiSliceMs));
    drawText(4, 20, line);

    snprintf(line, sizeof(line), "BLE slice: %lu ms",
             static_cast<unsigned long>(g_bleSliceMs));
    drawText(4, 32, line);

    snprintf(line, sizeof(line), "Chan dwell: %lu ms",
             static_cast<unsigned long>(g_dwellMs));
    drawText(4, 44, line);

    snprintf(line, sizeof(line), "Volume: %u / 255", g_volume);
    drawText(4, 56, line);

    if (radioGetTrafficEnabled()) {
        snprintf(line, sizeof(line), "TX ON: ap %s ch %u",
                 trafficAccessPointSsid(), static_cast<unsigned>(kTrafficChannel));
        drawText(4, 68, line, kAmber);
    } else {
        drawText(4, 68, "Local TX: off (press t)", kDimGrey);
    }

    static const char* kFamilyNames[6] = {"Pad", "Pluck", "Shimmer",
                                          "Bell", "Accent", "Kbd"};
    for (uint8_t i = 0; i < 6; ++i) {
        const uint8_t row = i < 3 ? i : static_cast<uint8_t>(i - 3);
        const bool leftColumn = i < 3;
        const int x = leftColumn ? 4 : 124;
        const int y = 80 + row * 12;
        if (i < 5) {
            snprintf(line, sizeof(line), "%u %s: %s", i + 1, kFamilyNames[i],
                     g_familyEnabled[i] ? "on" : "off");
        } else {
            snprintf(line, sizeof(line), "%s: %s", kFamilyNames[i],
                     g_familyEnabled[i] ? "on" : "off");
        }
        drawText(x, y, line, g_familyEnabled[i] ? kWhite : kDimGrey);
    }

    drawText(4, footerY(), "Reverb/echo off  Save/load off", kDimGrey);
}

// ----------------------------------------------------------------- sprite --

bool createBackBuffer() {
    // Try internal RAM first, then PSRAM. The whole frame is composed off
    // screen, so a single push replaces the visible image without flicker.
    for (int attempt = 0; attempt < 2; ++attempt) {
        lgfx::LGFX_Sprite* sprite = new lgfx::LGFX_Sprite(&M5Cardputer.Display);
        if (sprite == nullptr) {
            return false;
        }
        sprite->setPsram(attempt == 1);
        sprite->setColorDepth(static_cast<uint8_t>(16));
        if (sprite->createSprite(static_cast<int32_t>(g_width),
                                 static_cast<int32_t>(g_height)) != nullptr) {
            sprite->setFont(M5Cardputer.Display.getFont());
            g_sprite = sprite;
            g_target = sprite;
            Serial.printf("UI back buffer: %ux%u %s\n", g_width, g_height,
                          attempt == 1 ? "psram" : "ram");
            return true;
        }
        delete sprite;
    }
    Serial.println("UI back buffer unavailable, drawing direct.");
    return false;
}

}  // namespace

void uiInit() {
    g_width = M5Cardputer.Display.width();
    g_height = M5Cardputer.Display.height();
    if (g_width < 160) g_width = 240;
    if (g_height < 100) g_height = 135;

    g_target = &M5Cardputer.Display;
    createBackBuffer();

    g_screen = UiScreen::Constellation;
    for (uint8_t i = 0; i < config::kUiConstellationPoolSize; ++i) {
        g_dots[i].used = false;
        g_dots[i].rate = 0;
    }
    for (uint8_t col = 0; col < config::kUiWaterfallColumns; ++col) {
        for (uint8_t row = 0; row < kWaterfallRows; ++row) {
            g_waterfall[col][row] = 0;
        }
    }
    g_waterfallHead = 0;
    g_volume = 96;
    for (uint8_t i = 0; i < 6; ++i) g_familyEnabled[i] = true;

    g_wifiSliceMs = radioGetWifiSliceMs();
    g_bleSliceMs = radioGetBleSliceMs();
    g_dwellMs = radioGetChannelDwellMs();
}

void uiNextScreen() {
    const uint8_t next = (static_cast<uint8_t>(g_screen) + 1) % kScreenCount;
    g_screen = static_cast<UiScreen>(next);
}

UiScreen uiCurrentScreen() {
    return g_screen;
}

void uiBootScreen() {
    if (g_target == nullptr) {
        g_target = &M5Cardputer.Display;
    }
    gfx().fillScreen(kBlack);
    gfx().setTextSize(1);
    gfx().setTextColor(kWhite, kBlack);
    gfx().drawString("ETHER CHOIR", 87, 34);
    gfx().setTextColor(kCyan, kBlack);
    gfx().drawString("Passive listening, metadata only", 24, 62);
    gfx().setTextColor(kWhite, kBlack);
    gfx().drawString("Wi-Fi + BLE sonification", 48, 76);
    gfx().drawString("M5Stack Cardputer ADV", 57, 90);
    gfx().drawString("No reverb / echo / save-load", 36, 104);
    if (g_sprite != nullptr) {
        g_sprite->pushSprite(&M5Cardputer.Display, 0, 0);
    }
}

void uiNoteDevice(const SignalEvent& event, uint32_t nowMs) {
    const bool isBle = event.type == SrcType::BleAdv;
    const bool isLocalTx = event.isLocal;
    for (uint8_t i = 0; i < config::kUiConstellationPoolSize; ++i) {
        if (g_dots[i].used && g_dots[i].idHash == event.idHash) {
            g_dots[i].channel = event.channel;
            g_dots[i].rssi = event.rssi;
            g_dots[i].lastSeenMs = nowMs;
            g_dots[i].isBle = isBle;
            g_dots[i].isLocalTx = isLocalTx;
            if (g_dots[i].rate < 255) ++g_dots[i].rate;
            return;
        }
    }
    for (uint8_t i = 0; i < config::kUiConstellationPoolSize; ++i) {
        if (!g_dots[i].used) {
            g_dots[i].used = true;
            g_dots[i].idHash = event.idHash;
            g_dots[i].channel = event.channel;
            g_dots[i].rssi = event.rssi;
            g_dots[i].isBle = isBle;
            g_dots[i].isLocalTx = isLocalTx;
            g_dots[i].lastSeenMs = nowMs;
            g_dots[i].rate = 1;
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
    g_dots[oldest].channel = event.channel;
    g_dots[oldest].rssi = event.rssi;
    g_dots[oldest].isBle = isBle;
    g_dots[oldest].isLocalTx = isLocalTx;
    g_dots[oldest].lastSeenMs = nowMs;
    g_dots[oldest].rate = 1;
    g_dots[oldest].used = true;
}

void uiPushWaterfallColumn(const uint32_t* channelCounts, uint32_t bleCount) {
    for (uint8_t row = 0; row < 13; ++row) {
        const uint32_t count = channelCounts[row + 1];
        g_waterfall[g_waterfallHead][row] = count > 255 ? 255 : static_cast<uint8_t>(count);
    }
    g_waterfall[g_waterfallHead][13] = bleCount > 255 ? 255 : static_cast<uint8_t>(bleCount);
    g_waterfallHead = (g_waterfallHead + 1) % config::kUiWaterfallColumns;
}

void uiDraw(uint8_t volume, const char* noteName, bool toneEnabled,
            uint32_t nowMs) {
    if (g_target == nullptr) {
        g_target = &M5Cardputer.Display;
    }
    const uint32_t renderStartUs = micros();
    switch (g_screen) {
        case UiScreen::Constellation:
            drawConstellation(nowMs);
            break;
        case UiScreen::Waterfall:
            drawWaterfall();
            break;
        case UiScreen::Scope:
            drawScope(nowMs, noteName);
            break;
        case UiScreen::Choir:
            drawChoir();
            break;
        case UiScreen::Status:
            drawStatus(volume, noteName, toneEnabled);
            break;
        case UiScreen::Settings:
            drawSettings();
            break;
    }
    const uint32_t renderEndUs = micros();
    if (g_sprite != nullptr) {
        g_sprite->pushSprite(&M5Cardputer.Display, 0, 0);
    }
    g_lastRenderUs = renderEndUs - renderStartUs;
    g_lastPushUs = micros() - renderEndUs;
}

void uiSetVolume(uint8_t volume) {
    g_volume = volume;
}

uint8_t uiVolume() {
    return g_volume;
}

void uiSetFamilyEnabled(uint8_t familyIndex, bool enabled) {
    if (familyIndex < 6) g_familyEnabled[familyIndex] = enabled;
}

bool uiFamilyEnabled(uint8_t familyIndex) {
    return familyIndex < 6 && g_familyEnabled[familyIndex];
}

void uiToggleFamily(uint8_t familyIndex) {
    if (familyIndex < 6) g_familyEnabled[familyIndex] = !g_familyEnabled[familyIndex];
}

uint32_t uiWifiSliceMs() {
    return g_wifiSliceMs;
}

uint32_t uiBleSliceMs() {
    return g_bleSliceMs;
}

uint32_t uiChannelDwellMs() {
    return g_dwellMs;
}

void uiAdjustWifiSliceMs(int32_t delta) {
    int64_t next = static_cast<int64_t>(g_wifiSliceMs) + delta;
    if (next < static_cast<int64_t>(kMinWifiSliceMs)) next = kMinWifiSliceMs;
    if (next > static_cast<int64_t>(kMaxWifiSliceMs)) next = kMaxWifiSliceMs;
    g_wifiSliceMs = static_cast<uint32_t>(next);
    radioSetWifiSliceMs(g_wifiSliceMs);
}

void uiAdjustBleSliceMs(int32_t delta) {
    int64_t next = static_cast<int64_t>(g_bleSliceMs) + delta;
    if (next < static_cast<int64_t>(kMinBleSliceMs)) next = kMinBleSliceMs;
    if (next > static_cast<int64_t>(kMaxBleSliceMs)) next = kMaxBleSliceMs;
    g_bleSliceMs = static_cast<uint32_t>(next);
    radioSetBleSliceMs(g_bleSliceMs);
}

void uiAdjustChannelDwellMs(int32_t delta) {
    int64_t next = static_cast<int64_t>(g_dwellMs) + delta;
    if (next < static_cast<int64_t>(kMinDwellMs)) next = kMinDwellMs;
    if (next > static_cast<int64_t>(kMaxDwellMs)) next = kMaxDwellMs;
    g_dwellMs = static_cast<uint32_t>(next);
    radioSetChannelDwellMs(g_dwellMs);

    // Keep the Wi-Fi slice long enough to cover the whole band once.
    const uint32_t minimum = g_dwellMs * 13U;
    if (g_wifiSliceMs < minimum) {
        g_wifiSliceMs = minimum > kMaxWifiSliceMs ? kMaxWifiSliceMs : minimum;
        radioSetWifiSliceMs(g_wifiSliceMs);
    }
}

void uiSetTrafficEnabled(bool enabled) {
    radioSetTrafficEnabled(enabled);
}

bool uiTrafficEnabled() {
    return radioGetTrafficEnabled();
}

void uiToggleTraffic() {
    uiSetTrafficEnabled(!radioGetTrafficEnabled());
}

uint32_t uiLastRenderUs() {
    return g_lastRenderUs;
}

uint32_t uiLastPushUs() {
    return g_lastPushUs;
}
