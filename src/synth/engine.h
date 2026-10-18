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
