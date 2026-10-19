// UI screens: constellation, waterfall, scope, status, settings.
// Aggregate metadata only; never shows raw addresses, names, or SSIDs.
// Every frame is composed on an off-screen sprite and pushed in one go, so the
// panel never shows a half-drawn frame. All geometry is derived from the live
// panel size and every draw call is bounds checked.
// No reverb/echo and no save/load in this build.
#pragma once

#include <stdint.h>
#include "signal_event.h"

enum class UiScreen : uint8_t {
    Constellation,
    Waterfall,
    Scope,
    Choir,
    Status,
    Settings,
};

void uiInit();
void uiNextScreen();
UiScreen uiCurrentScreen();
void uiNoteDevice(const SignalEvent& event, uint32_t nowMs);
void uiPushWaterfallColumn(const uint32_t* channelCounts, uint32_t bleCount);
void uiDraw(uint8_t volume, const char* noteName, bool toneEnabled, uint32_t nowMs);

// Boot screen shown once at startup.
void uiBootScreen();

// Runtime-adjustable settings (volume and source-family toggles).
void uiSetVolume(uint8_t volume);
uint8_t uiVolume();
void uiSetFamilyEnabled(uint8_t familyIndex, bool enabled);
bool uiFamilyEnabled(uint8_t familyIndex);
void uiToggleFamily(uint8_t familyIndex);

// Radio slice timing. The settings screen owns these values and pushes them
// straight into the radio scheduler.
uint32_t uiWifiSliceMs();
uint32_t uiBleSliceMs();
uint32_t uiChannelDwellMs();
void uiAdjustWifiSliceMs(int32_t delta);
void uiAdjustBleSliceMs(int32_t delta);
void uiAdjustChannelDwellMs(int32_t delta);

// Local Wi-Fi traffic generator. This transmits, so it is opt-in and the UI
// labels it clearly wherever it is active.
void uiSetTrafficEnabled(bool enabled);
bool uiTrafficEnabled();
void uiToggleTraffic();

// Frame timing of the last drawn frame: scene composition and panel push.
uint32_t uiLastRenderUs();
uint32_t uiLastPushUs();
