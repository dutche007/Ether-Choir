// Renders audio voices into fixed speaker buffers (reverb bypassed).
#include "engine.h"

#include <M5Cardputer.h>
#include <esp_timer.h>
#include <math.h>
#include <atomic>
#include <stdint.h>
// (fx_reverb.h removed: reverb bypassed, dry only).

namespace {

constexpr uint32_t kSampleRate = 22050;
constexpr size_t kBlockSamples = 256;
constexpr size_t kBlockCount = 3;
constexpr size_t kSineTableSize = 1024;
constexpr size_t kVoicePoolSize = 12;
constexpr size_t kRadioVoiceCount = kVoicePoolSize - 1;
constexpr size_t kVoiceQueueCapacity = 32;
constexpr size_t kStealTailCount = 2;
constexpr uint16_t kStealFadeSamples = 128;
constexpr uint8_t kSpeakerChannel = 0;
constexpr uint32_t kEnvelopeMaximum = 0x00FFFFFF;
constexpr uint32_t kFastAttackStep = 12000;
constexpr uint32_t kPadAttackStep = 900;
constexpr uint32_t kPadReleaseStep = 160;
constexpr int32_t kLimiterKnee = 12000;
constexpr int32_t kLimiterMaximum = 16000;
constexpr uint32_t kMaximumPhaseDeviation = 1u << 30;
constexpr uint32_t kSmoothingSamples = 256;
constexpr uint32_t kPadHoldSamples = kSampleRate * 2;
constexpr uint32_t kShimmerHoldSamples = kSampleRate / 4;
constexpr uint32_t kPluckRetriggerSamples = kSampleRate / 8;
constexpr uint16_t kVoiceMixGainQ15[kVoicePoolSize + kStealTailCount + 1] = {
    0, 21299, 15060, 12297, 10649, 9529, 8695, 8052, 7525, 7100, 6735, 6421, 6149, 5907, 5693
};
constexpr int32_t kDcBlockerPoleQ15 = 32600;

enum class EnvelopeStage : uint8_t { Inactive, Attack, Sustain, Decay, Release };
enum class BufferState : uint8_t { Free, Rendering, Ready, Queued };

struct AudioBlock {
    int16_t samples[kBlockSamples]{};
    std::atomic<BufferState> state{BufferState::Free};
};

struct Voice {
    uint32_t idHash = 0;
    VoiceFamily family = VoiceFamily::Pad;
    EnvelopeStage stage = EnvelopeStage::Inactive;
    bool active = false;
    uint8_t modulatorRatio = 2;
    uint32_t phase = 0;
    uint32_t detunedPhase = 0;
    uint32_t modulatorPhase = 0;
    int32_t phaseIncrement = 0;
    int32_t targetPhaseIncrement = 0;
    int32_t detunedPhaseIncrement = 0;
    int32_t targetDetunedPhaseIncrement = 0;
    uint32_t envelopeQ24 = 0;
    int32_t gainQ15 = 0;
    int32_t targetGainQ15 = 0;
    int32_t fmIndexQ15 = 0;
    int32_t targetFmIndexQ15 = 0;
    uint32_t samplesSinceEvent = 0;
    uint32_t holdSamples = 0;
    uint32_t generation = 0;
    uint32_t noiseState = 1;
    int32_t noiseFilter = 0;
    int32_t padFilter = 0;
};

struct StealTail {
    Voice voice{};
    uint16_t samplesRemaining = 0;
};

struct VoiceQueueSlot { VoiceCommand command{}; };

int16_t g_sineTable[kSineTableSize];
AudioBlock g_blocks[kBlockCount];
Voice g_voices[kVoicePoolSize];
StealTail g_stealTails[kStealTailCount];
VoiceQueueSlot g_voiceQueue[kVoiceQueueCapacity];
std::atomic<uint32_t> g_voiceQueueHead{0};
std::atomic<uint32_t> g_voiceQueueTail{0};
std::atomic<uint32_t> g_voiceQueueDropCount{0};
std::atomic<uint32_t> g_keyboardFrequencyMilliHz{440000};
std::atomic<uint16_t> g_keyboardGainQ15{0};
std::atomic<uint16_t> g_keyboardFmIndexQ15{0};
std::atomic<bool> g_keyboardEnabled{false};
uint32_t g_voiceGeneration = 0;
bool g_started = false;

// (Reverb g_reverb removed: bypassed.)
std::atomic<uint64_t> g_renderTimeUs{0};
int32_t g_dcPrevInput = 0;
int32_t g_dcPrevOutput = 0;

uint32_t phaseIncrementFor(uint32_t frequencyMilliHz) {
    return static_cast<uint32_t>((static_cast<uint64_t>(frequencyMilliHz) << 32) / (kSampleRate * 1000ULL));
}

int16_t limitSample(int32_t sample) {
    const int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude <= kLimiterKnee) return static_cast<int16_t>(sample);
    const int32_t excess = magnitude - kLimiterKnee;
    const int32_t limited = kLimiterKnee + (excess * (kLimiterMaximum - kLimiterKnee)) / (excess + kLimiterMaximum - kLimiterKnee);
    return static_cast<int16_t>(sample < 0 ? -limited : limited);
}

int16_t dcBlocker(int16_t sample) {
    const int32_t out = sample - g_dcPrevInput + ((g_dcPrevOutput * kDcBlockerPoleQ15) >> 15);
    g_dcPrevInput = sample;
    if (out > 32767) { g_dcPrevOutput = 32767; return 32767; }
    if (out < -32768) { g_dcPrevOutput = -32768; return -32768; }
    g_dcPrevOutput = out;
    return static_cast<int16_t>(out);
}

int16_t sineAtPhase(uint32_t phase) {
    const uint32_t index = phase >> 22;
    const uint32_t nextIndex = (index + 1) & (kSineTableSize - 1);
    const uint32_t fraction = (phase >> 6) & 0xFFFF;
    const int32_t first = g_sineTable[index];
    const int32_t difference = static_cast<int32_t>(g_sineTable[nextIndex]) - first;
    return static_cast<int16_t>(first + ((difference * static_cast<int32_t>(fraction)) >> 16));
}

int32_t smoothToward(int32_t current, int32_t target) {
    const int32_t difference = target - current;
    if (difference == 0) return current;
    int32_t step = difference / static_cast<int32_t>(kSmoothingSamples);
    if (step == 0) step = difference > 0 ? 1 : -1;
    const int32_t next = current + step;
    return difference > 0 ? (next > target ? target : next) : (next < target ? target : next);
}

uint8_t familyLimit(VoiceFamily family) {
    switch (family) {
        case VoiceFamily::Pad: return 4;
        case VoiceFamily::Pluck: return 4;
        case VoiceFamily::Shimmer: return 2;
        case VoiceFamily::Bell: return 4;
        case VoiceFamily::Accent: return 2;
        case VoiceFamily::Keyboard: return 1;
    }
    return 1;
}

uint8_t decayShift(VoiceFamily family) {
    switch (family) {
        case VoiceFamily::Pluck: return 10;
        case VoiceFamily::Bell: return 12;
        case VoiceFamily::Accent: return 8;
        default: return 10;
    }
}

uint32_t attackStep(VoiceFamily family) {
    return family == VoiceFamily::Pad ? kPadAttackStep : kFastAttackStep;
}

int32_t voiceLevel(const Voice& voice) {
    return static_cast<int32_t>((static_cast<int64_t>(voice.gainQ15) * voice.envelopeQ24) / kEnvelopeMaximum);
}

int findQuietestVoice(bool restrictFamily, VoiceFamily family) {
    int selected = -1;
    int32_t selectedLevel = INT32_MAX;
    uint32_t selectedGeneration = UINT32_MAX;
    for (size_t index = 0; index < kRadioVoiceCount; ++index) {
        const Voice& voice = g_voices[index];
        if (!voice.active || (restrictFamily && voice.family != family)) continue;
        const int32_t level = voiceLevel(voice);
        if (level < selectedLevel || (level == selectedLevel && voice.generation < selectedGeneration)) {
            selected = static_cast<int>(index);
            selectedLevel = level;
            selectedGeneration = voice.generation;
        }
    }
    return selected;
}

size_t familyVoiceCount(VoiceFamily family) {
    size_t count = 0;
    for (size_t index = 0; index < kRadioVoiceCount; ++index) {
        count += g_voices[index].active && g_voices[index].family == family;
    }
    return count;
}

void beginStealTail(const Voice& voice) {
    size_t selected = 0;
    for (size_t index = 0; index < kStealTailCount; ++index) {
        if (g_stealTails[index].samplesRemaining == 0) {
            g_stealTails[index].voice = voice;
            g_stealTails[index].samplesRemaining = kStealFadeSamples;
            return;
        }
        if (g_stealTails[index].samplesRemaining < g_stealTails[selected].samplesRemaining) selected = index;
    }
    g_stealTails[selected].voice = voice;
    g_stealTails[selected].samplesRemaining = kStealFadeSamples;
}

uint8_t modulatorRatio(VoiceFamily family, uint32_t idHash) {
    switch (family) {
        case VoiceFamily::Pad: return 1;
        case VoiceFamily::Pluck: return 2;
        case VoiceFamily::Shimmer: return 1;
        case VoiceFamily::Bell: return 2 + (idHash % 3);
        case VoiceFamily::Accent: return 4;
        case VoiceFamily::Keyboard: return 2;
    }
    return 2;
}

void startVoice(Voice& voice, const VoiceCommand& command) {
    voice = Voice{};
    voice.active = true;
    voice.idHash = command.idHash;
    voice.family = command.family;
    voice.stage = EnvelopeStage::Attack;
    voice.targetPhaseIncrement = static_cast<int32_t>(phaseIncrementFor(command.frequencyMilliHz));
    voice.targetDetunedPhaseIncrement = static_cast<int32_t>((static_cast<uint64_t>(voice.targetPhaseIncrement) * 1007) / 1000);
    voice.targetGainQ15 = command.gainQ15;
    voice.targetFmIndexQ15 = command.fmIndexQ15;
    voice.modulatorRatio = modulatorRatio(command.family, command.idHash);
    voice.holdSamples = command.family == VoiceFamily::Pad ? kPadHoldSamples
        : command.family == VoiceFamily::Shimmer ? kShimmerHoldSamples : 0;
    voice.generation = ++g_voiceGeneration;
    voice.noiseState = command.idHash | 1u;
}

void applyVoiceCommand(const VoiceCommand& command) {
    int selected = -1;
    for (size_t index = 0; index < kRadioVoiceCount; ++index) {
        if (g_voices[index].active && g_voices[index].idHash == command.idHash) {
            selected = static_cast<int>(index);
            break;
        }
    }

    if (selected < 0) {
        for (size_t index = 0; index < kRadioVoiceCount; ++index) {
            if (!g_voices[index].active) { selected = static_cast<int>(index); break; }
        }
        if (familyVoiceCount(command.family) >= familyLimit(command.family)) {
            selected = findQuietestVoice(true, command.family);
        } else if (selected < 0) {
            selected = findQuietestVoice(false, command.family);
        }
        if (selected < 0) return;
        if (g_voices[selected].active) beginStealTail(g_voices[selected]);
        startVoice(g_voices[selected], command);
        return;
    }

    Voice& voice = g_voices[selected];
    const bool retrigger = voice.family != command.family
        || (command.family == VoiceFamily::Pluck && voice.samplesSinceEvent >= kPluckRetriggerSamples);
    if (retrigger) { voice.stage = EnvelopeStage::Attack; voice.envelopeQ24 = 0; }
    voice.family = command.family;
    voice.targetPhaseIncrement = static_cast<int32_t>(phaseIncrementFor(command.frequencyMilliHz));
    voice.targetDetunedPhaseIncrement = static_cast<int32_t>((static_cast<uint64_t>(voice.targetPhaseIncrement) * 1007) / 1000);
    voice.targetGainQ15 = command.gainQ15;
    voice.targetFmIndexQ15 = command.fmIndexQ15;
    voice.modulatorRatio = modulatorRatio(command.family, command.idHash);
    voice.holdSamples = command.family == VoiceFamily::Pad ? kPadHoldSamples
        : command.family == VoiceFamily::Shimmer ? kShimmerHoldSamples : 0;
    voice.samplesSinceEvent = 0;
    voice.generation = ++g_voiceGeneration;
}

void drainVoiceCommands() {
    uint32_t tail = g_voiceQueueTail.load(std::memory_order_relaxed);
    const uint32_t head = g_voiceQueueHead.load(std::memory_order_acquire);
    while (tail != head) {
        applyVoiceCommand(g_voiceQueue[tail & (kVoiceQueueCapacity - 1)].command);
        ++tail;
    }
    g_voiceQueueTail.store(tail, std::memory_order_release);
}

void updateKeyboardVoice() {
    Voice& voice = g_voices[kRadioVoiceCount];
    const bool enabled = g_keyboardEnabled.load(std::memory_order_relaxed);
    if (!enabled) {
        if (voice.active && voice.stage != EnvelopeStage::Release) {
            voice.stage = EnvelopeStage::Release;
            voice.targetGainQ15 = 0;
        }
        return;
    }
    VoiceCommand command{};
    command.family = VoiceFamily::Keyboard;
    command.frequencyMilliHz = g_keyboardFrequencyMilliHz.load(std::memory_order_relaxed);
    command.gainQ15 = g_keyboardGainQ15.load(std::memory_order_relaxed);
    command.fmIndexQ15 = g_keyboardFmIndexQ15.load(std::memory_order_relaxed);
    if (!voice.active) {
        startVoice(voice, command);
    } else {
        if (voice.stage == EnvelopeStage::Release) voice.stage = EnvelopeStage::Attack;
        voice.targetPhaseIncrement = static_cast<int32_t>(phaseIncrementFor(command.frequencyMilliHz));
        voice.targetDetunedPhaseIncrement = voice.targetPhaseIncrement;
        voice.targetGainQ15 = command.gainQ15;
        voice.targetFmIndexQ15 = command.fmIndexQ15;
        voice.samplesSinceEvent = 0;
    }
}

int32_t noiseSample(Voice& voice) {
    uint32_t value = voice.noiseState;
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    voice.noiseState = value;
    const int32_t noise = static_cast<int16_t>(value >> 16);
    voice.noiseFilter += (noise - voice.noiseFilter) / 20;
    return voice.noiseFilter;
}

int32_t renderVoiceSample(Voice& voice) {
    if (!voice.active) return 0;
    voice.phaseIncrement = smoothToward(voice.phaseIncrement, voice.targetPhaseIncrement);
    voice.detunedPhaseIncrement = smoothToward(voice.detunedPhaseIncrement, voice.targetDetunedPhaseIncrement);
    voice.gainQ15 = smoothToward(voice.gainQ15, voice.targetGainQ15);
    voice.fmIndexQ15 = smoothToward(voice.fmIndexQ15, voice.targetFmIndexQ15);
    voice.phase += static_cast<uint32_t>(voice.phaseIncrement);
    voice.detunedPhase += static_cast<uint32_t>(voice.detunedPhaseIncrement);
    voice.modulatorPhase += static_cast<uint32_t>(voice.phaseIncrement) * voice.modulatorRatio;
    if (voice.samplesSinceEvent < UINT32_MAX) ++voice.samplesSinceEvent;

    if ((voice.family == VoiceFamily::Pad || voice.family == VoiceFamily::Shimmer)
        && voice.samplesSinceEvent > voice.holdSamples
        && voice.stage != EnvelopeStage::Release) {
        voice.stage = EnvelopeStage::Release;
    }

    switch (voice.stage) {
        case EnvelopeStage::Attack:
            voice.envelopeQ24 = voice.envelopeQ24 + attackStep(voice.family) >= kEnvelopeMaximum
                ? kEnvelopeMaximum : voice.envelopeQ24 + attackStep(voice.family);
            if (voice.envelopeQ24 == kEnvelopeMaximum) {
                voice.stage = (voice.family == VoiceFamily::Pad || voice.family == VoiceFamily::Shimmer
                    || voice.family == VoiceFamily::Keyboard) ? EnvelopeStage::Sustain : EnvelopeStage::Decay;
            }
            break;
        case EnvelopeStage::Sustain: break;
        case EnvelopeStage::Decay: {
            uint32_t decay = voice.envelopeQ24 >> decayShift(voice.family);
            if (decay == 0 && voice.envelopeQ24 != 0) decay = 1;
            voice.envelopeQ24 = decay >= voice.envelopeQ24 ? 0 : voice.envelopeQ24 - decay;
            break;
        }
        case EnvelopeStage::Release: {
            const uint32_t releaseStep = voice.family == VoiceFamily::Pad ? kPadReleaseStep
                : voice.family == VoiceFamily::Keyboard ? 2048 : 1024;
            voice.envelopeQ24 = voice.envelopeQ24 > releaseStep ? voice.envelopeQ24 - releaseStep : 0;
            break;
        }
        case EnvelopeStage::Inactive: voice.envelopeQ24 = 0; break;
    }

    if (voice.envelopeQ24 == 0 && voice.stage != EnvelopeStage::Attack) {
        voice.active = false;
        voice.stage = EnvelopeStage::Inactive;
        return 0;
    }

    int32_t signal = 0;
    if (voice.family == VoiceFamily::Shimmer) {
        signal = noiseSample(voice);
    } else if (voice.family == VoiceFamily::Pad) {
        const int32_t fundamental = sineAtPhase(voice.phase);
        const int32_t detuned = sineAtPhase(voice.detunedPhase);
        const int32_t pad = (fundamental * 3 + detuned * 2) / 5;
        voice.padFilter += (pad - voice.padFilter) / 4;
        signal = voice.padFilter;
    } else {
        const int64_t phaseOffset = (static_cast<int64_t>(sineAtPhase(voice.modulatorPhase))
                                     * voice.fmIndexQ15 * kMaximumPhaseDeviation) / (32767LL * 32767LL);
        signal = sineAtPhase(voice.phase + static_cast<uint32_t>(phaseOffset));
    }

    const int64_t enveloped = (static_cast<int64_t>(signal) * voice.envelopeQ24) / kEnvelopeMaximum;
    const int32_t familyGain = voice.family == VoiceFamily::Shimmer ? voice.gainQ15 / 12
        : voice.family == VoiceFamily::Pad ? voice.gainQ15 * 3 / 4 : voice.gainQ15;
    return static_cast<int32_t>((enveloped * familyGain) >> 15);
}

void renderBlock(int16_t* samples) {
    drainVoiceCommands();
    updateKeyboardVoice();
    size_t activeVoiceCount = 0;
    for (const Voice& voice : g_voices) activeVoiceCount += voice.active;
    for (const StealTail& tail : g_stealTails) activeVoiceCount += tail.samplesRemaining != 0 && tail.voice.active;
    if (activeVoiceCount >= sizeof(kVoiceMixGainQ15) / sizeof(kVoiceMixGainQ15[0])) {
        activeVoiceCount = sizeof(kVoiceMixGainQ15) / sizeof(kVoiceMixGainQ15[0]) - 1;
    }
    const uint16_t mixGainQ15 = kVoiceMixGainQ15[activeVoiceCount];

    for (size_t index = 0; index < kBlockSamples; ++index) {
        int32_t mix = 0;
        for (Voice& voice : g_voices) mix += renderVoiceSample(voice);
        for (StealTail& tail : g_stealTails) {
            if (tail.samplesRemaining == 0) continue;
            const int32_t tailSample = renderVoiceSample(tail.voice);
            mix += tailSample * tail.samplesRemaining / kStealFadeSamples;
            --tail.samplesRemaining;
        }
        const int32_t scaledMix = static_cast<int32_t>((static_cast<int64_t>(mix) * mixGainQ15) >> 15);

        // Master chain: limiter -> reverb -> DC blocker.
        const int16_t limited = limitSample(scaledMix);
        // (reverb bypassed: dry only).
        samples[index] = dcBlocker(limited);  // Reverb bypassed.
    }
}

AudioBlock* blockForData(const void* data) {
    for (AudioBlock& block : g_blocks) {
        if (block.samples == data) return &block;
    }
    return nullptr;
}

void onBufferReleased(void*, const void* data, uint8_t channel) {
    if (channel != kSpeakerChannel) return;
    AudioBlock* block = blockForData(data);
    if (block == nullptr) return;
    block->state.store(BufferState::Rendering, std::memory_order_relaxed);
    const uint64_t startUs = esp_timer_get_time();
    renderBlock(block->samples);
    const uint64_t endUs = esp_timer_get_time();
    g_renderTimeUs.store(endUs - startUs, std::memory_order_relaxed);
    if (M5Cardputer.Speaker.playRaw(block->samples, kBlockSamples, kSampleRate, false, 1, kSpeakerChannel, false)) {
        block->state.store(BufferState::Queued, std::memory_order_release);
    } else {
        block->state.store(BufferState::Ready, std::memory_order_release);
    }
}

bool queueReadyBlock(AudioBlock& block) {
    BufferState expected = BufferState::Ready;
    if (!block.state.compare_exchange_strong(expected, BufferState::Rendering,
                                             std::memory_order_acquire, std::memory_order_relaxed)) {
        return false;
    }
    const bool queued = M5Cardputer.Speaker.playRaw(block.samples, kBlockSamples, kSampleRate,
                                                     false, 1, kSpeakerChannel, false);
    block.state.store(queued ? BufferState::Queued : BufferState::Ready, std::memory_order_release);
    return queued;
}

}  // namespace

bool audioEngineStart() {
    if (g_started) return true;
    for (size_t index = 0; index < kSineTableSize; ++index) {
        const double angle = (2.0 * M_PI * index) / kSineTableSize;
        g_sineTable[index] = static_cast<int16_t>(sin(angle) * 32767.0);
    }
    auto speakerConfig = M5Cardputer.Speaker.config();
    speakerConfig.sample_rate = kSampleRate;
    speakerConfig.task_priority = 3;
    speakerConfig.task_pinned_core = 1;
    M5Cardputer.Speaker.config(speakerConfig);
    M5Cardputer.Speaker.setBufferReleaseCallback(nullptr, onBufferReleased);
    if (!M5Cardputer.Speaker.begin()) return false;
    g_started = true;
    for (size_t index = 0; index < 2; ++index) {
        AudioBlock& block = g_blocks[index];
        block.state.store(BufferState::Rendering, std::memory_order_relaxed);
        const bool queued = M5Cardputer.Speaker.playRaw(block.samples, kBlockSamples, kSampleRate, false, 1, kSpeakerChannel, false);
        block.state.store(queued ? BufferState::Queued : BufferState::Ready, std::memory_order_release);
    }
    return true;
}

bool audioEnginePushVoiceCommand(const VoiceCommand& command) {
    const uint32_t head = g_voiceQueueHead.load(std::memory_order_relaxed);
    const uint32_t tail = g_voiceQueueTail.load(std::memory_order_acquire);
    if (head - tail >= kVoiceQueueCapacity) {
        g_voiceQueueDropCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_voiceQueue[head & (kVoiceQueueCapacity - 1)].command = command;
    g_voiceQueueHead.store(head + 1, std::memory_order_release);
    return true;
}

uint32_t audioEngineVoiceQueueDrops() {
    return g_voiceQueueDropCount.load(std::memory_order_relaxed);
}

void audioEngineSetKeyboardVoice(uint32_t frequencyMilliHz, uint16_t gainQ15,
                                 uint16_t fmIndexQ15, bool enabled) {
    g_keyboardFrequencyMilliHz.store(frequencyMilliHz, std::memory_order_relaxed);
    g_keyboardGainQ15.store(gainQ15, std::memory_order_relaxed);
    g_keyboardFmIndexQ15.store(fmIndexQ15, std::memory_order_relaxed);
    g_keyboardEnabled.store(enabled, std::memory_order_relaxed);
}

void audioEnginePump() {
    if (!g_started || M5Cardputer.Speaker.isPlaying(kSpeakerChannel) >= 2) return;
    for (AudioBlock& block : g_blocks) {
        if (queueReadyBlock(block)) return;
    }
}

void audioEngineSetReverbWet(uint16_t wetQ15) {
    (void)wetQ15;  // Reverb bypassed: no-op.
}

uint64_t audioEngineGetRenderTimeUs() {
    return g_renderTimeUs.load(std::memory_order_relaxed);

uint32_t audioEngineActiveVoiceCount() {
    uint32_t count = 0;
    for (const Voice& voice : g_voices) count += voice.active ? 1 : 0;
    return count;
}
}
