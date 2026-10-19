// The roster of devices the choir is currently made of, and the part each one
// holds.
//
// Metadata only: a member is identified by the same per-boot salted hash the
// radio produces, so no raw address is stored and nothing here can be linked
// to a device across sessions. Nothing is persisted.
//
// A member is "connected" when it is seen sending data, when it advertises
// over BLE, or when it is associated with the local access point. Devices that
// are only heard beaconing are in range but not connected.
#pragma once

#include <stdint.h>
#include "signal_event.h"

// How a member takes part in the choir.
enum class ChoirRole : uint8_t {
    InRange,      // only beacons seen so far
    Active,       // sending data, so it is connected to a network
    Advertising,  // sending BLE advertisements
    Client,       // associated with the local access point
    Local,        // this device's own transmissions
};

struct ChoirMember {
    uint32_t idHash = 0;
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0;
    uint32_t lastDataMs = 0;
    uint32_t packets = 0;
    int8_t rssi = -90;
    uint8_t channel = 0;
    uint8_t part = 0;        // seat in the choir, 1..config::kChoirMaxMembers
    ChoirRole role = ChoirRole::InRange;
    SrcType type = SrcType::WifiOther;
    bool used = false;
};

void choirInit();

// Folds an event into the roster. Returns the part of a device that has just
// joined, or 0 when the event came from a member that was already in the
// choir.
uint8_t choirNoteEvent(const SignalEvent& event, uint32_t nowMs);

// Replaces the list of devices associated with the local access point. A
// member that is no longer listed has left the access point.
void choirSetAccessPointClients(const uint32_t* idHashes, uint8_t count,
                                uint8_t channel, uint32_t nowMs);

uint8_t choirMemberCount();
uint8_t choirRoleCount(ChoirRole role);
const ChoirMember& choirMemberAt(uint8_t index);

// The note the member's part sounds, for example "C4".
const char* choirNoteName(const ChoirMember& member);

// The note a part sounds, for example "C4".
const char* choirNoteNameForPart(uint8_t part);

// Short label for a role, for example "ACT".
const char* choirRoleLabel(ChoirRole role);
