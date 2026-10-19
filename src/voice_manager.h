// Maps radio metadata to a scale-quantised, RSSI-scaled voice command.
#pragma once

#include <stdint.h>
#include "signal_event.h"

enum class VoiceFamily : uint8_t {
    Pad,
    Pluck,
    Shimmer,
    Bell,
    Accent,
    Keyboard
};

struct VoiceCommand {
    uint32_t idHash;
    VoiceFamily family;
    uint32_t frequencyMilliHz;
    uint16_t gainQ15;
    uint16_t fmIndexQ15;
};

bool voiceManagerMapEvent(const SignalEvent& event, VoiceCommand& command);

// The note a source sounds, for example "C4". Both the degree and the octave
// come from the transmitter hash, so a device keeps the same part for as long
// as it is in the choir.
const char* voiceManagerNoteName(uint32_t idHash);