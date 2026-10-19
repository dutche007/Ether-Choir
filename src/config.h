// Central constants for Ether Choir (Milestone 8: UI, reverb bypassed).
#pragma once

#include <stdint.h>

namespace config {

// Radio slice timing (ms). Wi-Fi and BLE each own the shared radio for one
// slice before the scheduler switches. One slice of each per scheduler tick.
constexpr uint32_t kWifiSliceMs = 1560;
constexpr uint32_t kBleSliceMs = 600;

// Wi-Fi channel hop (within the Wi-Fi slice). The default slice length is
// sized to sweep the whole 2.4 GHz band once before handing the radio to BLE.
constexpr uint8_t kFirstWifiChannel = 1;
constexpr uint8_t kLastWifiChannel = 13;
constexpr uint8_t kWifiChannelCount = 13;
constexpr uint32_t kChannelDwellMs = 120;

// Radio scheduler task (core 0).
constexpr uint32_t kRadioTaskStackWords = 2048;
constexpr uint8_t kRadioTaskPriority = 1;
constexpr uint8_t kRadioTaskCore = 0;

// Audio / engine constants.
constexpr uint32_t kSampleRate = 22050;

// UI (Milestone 8): five screens, static pools, no recorder.
constexpr uint32_t kUiRefreshIntervalMs = 100;
constexpr uint32_t kUiScopeRefreshIntervalMs = 40;   // 25 fps for the scope
constexpr uint8_t kUiConstellationPoolSize = 24;
constexpr uint32_t kUiConstellationFadeMs = 20000;
constexpr uint8_t kUiWaterfallColumns = 120;

// The roster of devices the choir is made of. A device keeps its part while it
// stays in the roster, and leaves it after a period of silence.
constexpr uint8_t kChoirMaxMembers = 24;

// Development aid. When enabled the device brings the local traffic
// generator up at boot and cycles through every screen, so the whole UI can be
// checked on a device with no keyboard attached. Always false for normal
// builds.
constexpr bool kUiSelfTest = false;
constexpr uint32_t kUiSelfTestIntervalMs = 2000;

// How many out-of-bounds draw attempts are reported before the UI goes quiet.
constexpr uint8_t kUiBoundsReportLimit = 8;

}  // namespace config
