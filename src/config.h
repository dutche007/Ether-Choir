// Central constants for Ether Choir (Milestone 8: UI, reverb bypassed).
#pragma once

#include <stdint.h>

namespace config {

// Radio slice timing (ms). Wi-Fi and BLE each own the shared radio for one
// slice before the scheduler switches. One slice of each per scheduler tick.
constexpr uint32_t kWifiSliceMs = 300;
constexpr uint32_t kBleSliceMs = 300;

// Wi-Fi channel hop (within the Wi-Fi slice).
constexpr uint8_t kFirstWifiChannel = 1;
constexpr uint8_t kLastWifiChannel = 13;
constexpr uint32_t kChannelDwellMs = 150;

// Radio scheduler task (core 0).
constexpr uint32_t kRadioTaskStackWords = 2048;
constexpr uint8_t kRadioTaskPriority = 1;
constexpr uint8_t kRadioTaskCore = 0;

// Audio / engine constants.
constexpr uint32_t kSampleRate = 22050;

// UI (Milestone 8): four screens, static pools, no recorder.
constexpr uint32_t kUiRefreshIntervalMs = 100;
constexpr uint8_t kUiConstellationPoolSize = 24;
constexpr uint32_t kUiConstellationFadeMs = 20000;
constexpr uint8_t kUiWaterfallColumns = 60;

}  // namespace config
