// UI screens (Milestone 8): constellation, waterfall, status, settings.
// Aggregate metadata only; never shows raw addresses, names, or SSIDs.
#pragma once

#include <stdint.h>
#include "signal_event.h"

enum class UiScreen : uint8_t {
    Constellation,
    Waterfall,
    Status,
    Settings,
};

void uiInit();
void uiNextScreen();
UiScreen uiCurrentScreen();
void uiNoteDevice(const SignalEvent& event, uint32_t nowMs);
void uiPushWaterfallColumn(const uint32_t* channelCounts, uint32_t bleCount);
void uiDraw(uint8_t volume, const char* noteName, bool toneEnabled, uint32_t nowMs);
