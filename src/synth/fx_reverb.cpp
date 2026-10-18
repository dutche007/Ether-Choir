// Implementation of Schroeder-style reverb.
// Static delay-line buffers only; no heap allocation.
#include "fx_reverb.h"

#include <math.h>

// Comb delay lengths (samples @ 22.05 kHz). Prime-ish to avoid echo flutter.
// 400 ~ 18ms, 500 ~ 23ms, 600 ~ 27ms, 700 ~ 32ms.
constexpr uint32_t kCombDelay1 = 400;
constexpr uint32_t kCombDelay2 = 500;
constexpr uint32_t kCombDelay3 = 600;
constexpr uint32_t kCombDelay4 = 700;

// Allpass delay lengths.
constexpr uint32_t kAllpassDelay1 = 100;  // ~4.5 ms
constexpr uint32_t kAllpassDelay2 = 200;  // ~9 ms

// Defaults.
constexpr uint16_t kDefaultWetQ15 = 8192;   // 25% wet
constexpr uint16_t kDefaultFeedbackQ15 = 19660;  // ~0.6
constexpr int32_t kDampCoefQ15 = 24576;     // light damping ~0.75

// Allpass feedback (0.5 typical).
constexpr int32_t kAllpassFeedbackQ15 = 16384;

// DC blocker pole (close to 1 = strong DC removal, minimal phase effect).
constexpr float kDcBlockerPole = 0.995f;

// Storage for static buffers -- defined here to avoid inline header issues.
int16_t Reverb::s_comb1Buf[512] = {};
int16_t Reverb::s_comb2Buf[512] = {};
int16_t Reverb::s_comb3Buf[1024] = {};
int16_t Reverb::s_comb4Buf[1024] = {};
int16_t Reverb::s_ap1Buf[128] = {};
int16_t Reverb::s_ap2Buf[256] = {};

Reverb::Reverb()
    : m_wetQ15(kDefaultWetQ15), m_dryQ15(32767 - kDefaultWetQ15) {
    m_combs[0] = {s_comb1Buf, 512, 511, 0, kCombDelay1, kDefaultFeedbackQ15, kDampCoefQ15, 0};
    m_combs[1] = {s_comb2Buf, 512, 511, 0, kCombDelay2, kDefaultFeedbackQ15, kDampCoefQ15, 0};
    m_combs[2] = {s_comb3Buf, 1024, 1023, 0, kCombDelay3, kDefaultFeedbackQ15, kDampCoefQ15, 0};
    m_combs[3] = {s_comb4Buf, 1024, 1023, 0, kCombDelay4, kDefaultFeedbackQ15, kDampCoefQ15, 0};

    m_allpasses[0] = {s_ap1Buf, 128, 127, 0, kAllpassDelay1, kAllpassFeedbackQ15};
    m_allpasses[1] = {s_ap2Buf, 256, 255, 0, kAllpassDelay2, kAllpassFeedbackQ15};
}

void Reverb::setWetQ15(uint16_t wetQ15) {
    m_wetQ15 = wetQ15;
    m_dryQ15 = static_cast<uint16_t>(32767 - wetQ15);
}

void Reverb::setFeedbackQ15(uint16_t feedbackQ15) {
    for (auto& c : m_combs) {
        c.feedbackQ15 = feedbackQ15;
    }
}

void Reverb::mute() {
    for (auto& c : m_combs) {
        for (uint32_t i = 0; i < c.size; ++i) {
            c.buffer[i] = 0;
        }
        c.writePos = 0;
        c.prevOutput = 0;
    }
    for (auto& a : m_allpasses) {
        for (uint32_t i = 0; i < a.size; ++i) {
            a.buffer[i] = 0;
        }
        a.writePos = 0;
    }
}

int16_t Reverb::combProcess(Comb& c, int16_t input) {
    // Read delayed sample.
    int32_t delayed = c.buffer[c.writePos];

    // Simple one-pole lowpass damping in the feedback path.
    int32_t damped = c.prevOutput + ((c.dampCoefQ15 * (delayed - c.prevOutput)) >> 15);
    c.prevOutput = damped;

    // Feedback: output = input + feedback * delayed_damped.
    int32_t output = input + ((c.feedbackQ15 * damped) >> 15);

    // Write into delay line at current position.
    c.buffer[c.writePos] = static_cast<int16_t>(output);

    // Advance circular position.
    c.writePos = (c.writePos + 1) & c.mask;

    return static_cast<int16_t>(output);
}

int16_t Reverb::allpassProcess(Allpass& a, int16_t input) {
    // Read delayed sample.
    int32_t delayed = a.buffer[a.writePos];

    // Allpass: output = -input + delayed; feedback = input + (-feedback * delayed)
    // Standard: y[n] = -x[n] + g*x[n-D] + g*y[n-D]; stored = x[n] + g*y[n-D]
    // Simplified form: output = delayed - feedback * input; stored = input + feedback * output
    int32_t output = delayed - ((a.feedbackQ15 * input) >> 15);
    int32_t stored = input + ((a.feedbackQ15 * output) >> 15);

    a.buffer[a.writePos] = static_cast<int16_t>(stored);
    a.writePos = (a.writePos + 1) & a.mask;

    return static_cast<int16_t>(output);
}

int16_t Reverb::process(int16_t sample) {
    // Dry path.
    int32_t dry = (static_cast<int32_t>(sample) * m_dryQ15) >> 15;

    // Parallel comb filters.
    int32_t wet = 0;
    for (auto& c : m_combs) {
        wet += combProcess(c, sample);
    }

    // Series allpasses on the summed comb output.
    int16_t ap = static_cast<int16_t>(wet);
    for (auto& a : m_allpasses) {
        ap = allpassProcess(a, ap);
    }

    int32_t wetScaled = (static_cast<int32_t>(ap) * m_wetQ15) >> 15;

    int32_t out = dry + wetScaled;

    // Soft clip to int16 range.
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;

    return static_cast<int16_t>(out);
}
