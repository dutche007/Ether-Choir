// Carries metadata-only radio events; transmitter addresses are salted hashes.
#pragma once

#include <stdint.h>

enum class SrcType : uint8_t {
    WifiBeacon,
    WifiProbeReq,
    WifiProbeResp,
    WifiData,
    WifiOther,
    BleAdv
};

struct SignalEvent {
    uint32_t idHash;
    int8_t rssi;
    uint8_t channel;
    SrcType type;
    uint16_t companyId;
    uint32_t tMs;
};