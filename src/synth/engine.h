// Owns the audio renderer and speaker controls (reverb bypassed, dry only).
#pragma once

#include <stdint.h>
#include "../voice_manager.h"

bool audioEngineStart();
bool audioEnginePushVoiceCommand(const VoiceCommand& command);
uint32_t audioEngineVoiceQueueDrops();
void audioEngineSetKeyboardVoice(uint32_t frequencyMilliHz, uint16_t gainQ15,
                                 uint16_t fmIndexQ15, bool enabled);
void audioEnginePump();
void audioEngineSetReverbWet(uint16_t wetQ15);
uint64_t audioEngineGetRenderTimeUs();
uint32_t audioEngineActiveVoiceCount();

// How many radio voices can sound at the same time. The choir can hold more
// members than this, so the quietest voice is recycled when the room gets
// busy.
uint32_t audioEngineVoiceCapacity();

// Oscilloscope tap. Copies the most recent decimated output samples into the
// caller's buffer, oldest first, and returns how many points were written.
uint16_t audioEngineCopyWaveform(int16_t* destination, uint8_t pointCount);
uint16_t audioEngineGetPeakLevel();
