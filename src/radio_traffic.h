// Opt-in local Wi-Fi traffic generator.
//
// This module TRANSMITS. It brings up a local WPA2 access point on channel 6
// and injects the traffic that belongs to it straight into the radio event
// queue.
//
// A single radio cannot hear its own transmissions, and the access point of
// this device is invisible to its own station interface (a directed probe
// response is transmitted by the same chip that would have to receive it), so
// the local source is injected into the event queue instead of being captured
// from the air. Everything else in the pipeline is unchanged, which makes the
// local source a known, repeatable test signal for the sonification.
//
// The passive band sweep is paused while the generator runs, because the
// access point and the promiscuous capture cannot share the radio.
//
// Nothing here is enabled by default: the user must switch it on from the
// settings screen, and the UI labels the state as a local transmitter.
#pragma once

#include <stdint.h>

// Channel the local access point runs on. Must be a valid 2.4 GHz channel.
constexpr uint8_t kTrafficChannel = 6;

bool trafficBegin();
void trafficStop();

uint32_t trafficPacketCount();
bool trafficIsActive();

// Name of the access point the generator brings up, so the UI can show what
// is on the air while the generator is running.
const char* trafficAccessPointSsid();

// Copies the hashes of the devices associated with the local access point
// into the caller's buffer and returns how many were written. The addresses
// are hashed with the shared per-boot salt, so no raw address leaves the
// module.
uint8_t trafficCopyClientHashes(uint32_t* destination, uint8_t maxCount);
