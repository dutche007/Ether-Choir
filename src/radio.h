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

// Shared per-boot salted FNV-1a hash over a 6-byte address. Salt is drawn from
// esp_random() at boot and never persisted, so hashes cannot be correlated
// across sessions. Returns a 32-bit transmitter identifier.
uint32_t hashAddress(const uint8_t* address);
