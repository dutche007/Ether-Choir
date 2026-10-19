// Owns the bounded event queue, the shared per-boot address salt/hash helper,
// and the Wi-Fi/BLE radio time-slice scheduler.
// Passive capture only: transmitter addresses are hashed immediately in the
// callback and never leave this module unhashed.
#pragma once

#include <stdint.h>
#include "signal_event.h"

// Which radio owns the shared 2.4 GHz radio right now.
enum class RadioMode : uint8_t {
    WifiOnly,   // promiscuous Wi-Fi capture
    BleOnly,    // passive BLE scan
    Both,       // time-slice Wi-Fi and BLE
};

bool radioBegin();
void radioSetMode(RadioMode mode);
RadioMode radioGetMode();
bool radioTryReceive(SignalEvent& event);
uint32_t radioQueueDrops();
void radioPushEvent(const SignalEvent& event);

// The Wi-Fi channel the capture radio is listening on right now.
uint8_t radioGetCurrentChannel();

// Runtime radio timing, adjustable from the settings screen.
void radioSetWifiSliceMs(uint32_t milliseconds);
uint32_t radioGetWifiSliceMs();
void radioSetBleSliceMs(uint32_t milliseconds);
uint32_t radioGetBleSliceMs();
void radioSetChannelDwellMs(uint32_t milliseconds);
uint32_t radioGetChannelDwellMs();

// Local Wi-Fi traffic generator. When enabled the device runs a hidden access
// point and generates its own traffic, which adds a steady "self" voice to
// the choir. This transmits, so it is off by default and clearly labelled.
void radioSetTrafficEnabled(bool enabled);
bool radioGetTrafficEnabled();
uint32_t radioGetTrafficPacketCount();

// True while the local traffic generator is switched on. The Wi-Fi capture
// stops hopping channels in that case so the radio stays on the local
// access point's channel.
bool radioTrafficActive();

// Shared per-boot salted FNV-1a hash over a 6-byte address. Salt is drawn from
// esp_random() at boot and never persisted, so hashes cannot be correlated
// across sessions. Returns a 32-bit transmitter identifier.
uint32_t hashAddress(const uint8_t* address);
