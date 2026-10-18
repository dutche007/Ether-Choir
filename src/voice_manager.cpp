// Maps Wi-Fi frame metadata to a C minor-pentatonic FM voice.
#include "voice_manager.h"

#include <stdint.h>

namespace {

constexpr uint32_t kMinorPentatonicMilliHz[3][5] = {
    {130813, 155563, 174614, 196000, 233082},
    {261626, 311127, 349228, 391995, 466164},
    {523251, 622254, 698456, 783991, 932328}
};
constexpr int8_t kMinimumRssi = -95;
constexpr int8_t kMaximumRssi = -30;
constexpr uint16_t kMinimumGainQ15 = 5000;
constexpr uint16_t kMaximumGainQ15 = 27000;

uint16_t baseFmIndex(SrcType type) {
    switch (type) {
        case SrcType::WifiBeacon: return 2600;
        case SrcType::WifiProbeReq: return 7000;
        case SrcType::WifiProbeResp: return 5600;
        case SrcType::WifiData: return 3400;
        case SrcType::WifiOther: return 4800;
        case SrcType::BleAdv: return 6000;
    }
    return 4000;
}

VoiceFamily familyFor(SrcType type) {
    switch (type) {
        case SrcType::WifiBeacon: return VoiceFamily::Pad;
        case SrcType::WifiProbeReq:
        case SrcType::WifiProbeResp: return VoiceFamily::Pluck;
        case SrcType::WifiData: return VoiceFamily::Shimmer;
        case SrcType::WifiOther: return VoiceFamily::Accent;
        case SrcType::BleAdv: return VoiceFamily::Bell;
    }
    return VoiceFamily::Accent;
}

}  // namespace

bool voiceManagerMapEvent(const SignalEvent& event, VoiceCommand& command) {
    int16_t rssi = event.rssi;
    if (rssi < kMinimumRssi) {
        rssi = kMinimumRssi;
    } else if (rssi > kMaximumRssi) {
        rssi = kMaximumRssi;
    }
    const uint16_t strength = static_cast<uint16_t>(rssi - kMinimumRssi);
    const uint8_t degree = event.idHash % 5;
    const uint8_t octave = event.channel >= 1 && event.channel <= 13
        ? (event.channel - 1) / 5
        : (event.idHash >> 8) % 3;

    command.idHash = event.idHash;
    command.family = familyFor(event.type);
    command.frequencyMilliHz = kMinorPentatonicMilliHz[octave][degree];
    command.gainQ15 = kMinimumGainQ15
        + (static_cast<uint32_t>(strength) * (kMaximumGainQ15 - kMinimumGainQ15))
            / (kMaximumRssi - kMinimumRssi);
    command.fmIndexQ15 = baseFmIndex(event.type)
        + (static_cast<uint32_t>(strength) * 70) / (kMaximumRssi - kMinimumRssi);
    return true;
}