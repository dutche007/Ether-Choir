#pragma once

#include <stdint.h>

// Schroeder-style reverb: 4 parallel comb filters + 2 series allpass filters.
// Static delay-line buffers only; no heap allocation. Safe to call from the
// audio task on core 1.
// Passive audio effect only -- no radio or metadata is processed here.
class Reverb {
public:
    Reverb();

    // Set wet/dry mix: 0 = dry only, 32767 = wet only.
    void setWetQ15(uint16_t wetQ15);

    // Set comb feedback (controls decay / room size): 0 = none, 32767 = long.
    void setFeedbackQ15(uint16_t feedbackQ15);

    // Process one mono int16 sample, return reverberated int16 sample.
    int16_t process(int16_t sample);

    // Clear all delay lines (mute the reverb tail).
    void mute();

private:
    struct Comb {
        int16_t* buffer;
        uint32_t size;
        uint32_t mask;
        uint32_t writePos;
        uint32_t delaySamples;
        int32_t feedbackQ15;
        int32_t dampCoefQ15;
        int32_t prevOutput;
    };

    struct Allpass {
        int16_t* buffer;
        uint32_t size;
        uint32_t mask;
        uint32_t writePos;
        uint32_t delaySamples;
        int32_t feedbackQ15;
    };

    // Delay-line buffers (power-of-2 sizes for efficient circular wrapping).
    // Total: 3456 int16 samples ~ 6.9 KB SRAM.
    static int16_t s_comb1Buf[512];
    static int16_t s_comb2Buf[512];
    static int16_t s_comb3Buf[1024];
    static int16_t s_comb4Buf[1024];
    static int16_t s_ap1Buf[128];
    static int16_t s_ap2Buf[256];

    Comb m_combs[4];
    Allpass m_allpasses[2];

    uint16_t m_wetQ15;
    uint16_t m_dryQ15;

    int16_t combProcess(Comb& c, int16_t input);
    int16_t allpassProcess(Allpass& a, int16_t input);
};
