// The roster of devices the choir is currently made of, and the part each one
// holds. See choir.h for what this module owns.
#include "choir.h"

#include <string.h>
#include "config.h"
#include "voice_manager.h"

namespace {

ChoirMember g_members[config::kChoirMaxMembers]{};
uint8_t g_memberCount = 0;

// A member that has been quiet for this long leaves the choir and gives its
// seat to the next device.
constexpr uint32_t kQuietTimeoutMs = 30000;

// A member that was seen sending data counts as connected for this long.
constexpr uint32_t kActiveHoldMs = 6000;

ChoirMember* findMember(uint32_t idHash) {
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        if (g_members[index].used && g_members[index].idHash == idHash) {
            return &g_members[index];
        }
    }
    return nullptr;
}

// The lowest seat that is not taken, so a device keeps its part for as long as
// it stays in the choir.
uint8_t nextFreePart() {
    for (uint8_t part = 1; part <= config::kChoirMaxMembers; ++part) {
        bool taken = false;
        for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
            if (g_members[index].used && g_members[index].part == part) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return part;
        }
    }
    return 0;
}

// Frees the seats of members that have gone quiet, so the roster follows the
// room rather than filling up with devices that left an hour ago, and drops
// the "connected" mark from members that have stopped sending data.
void refreshMembers(uint32_t nowMs) {
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        ChoirMember& member = g_members[index];
        if (!member.used) {
            continue;
        }
        if (nowMs - member.lastSeenMs >= kQuietTimeoutMs) {
            member.used = false;
            if (g_memberCount > 0) {
                --g_memberCount;
            }
            continue;
        }
        if (member.role == ChoirRole::Active
            && nowMs - member.lastDataMs >= kActiveHoldMs) {
            // The device stopped sending data, so it is in range again.
            member.role = ChoirRole::InRange;
        }
    }
}

// An empty seat, or the seat of the member that has been quiet longest, so a
// new device can always join and the roster keeps up with the room. A device
// that is only in range is given up before one that is connected or
// advertising.
ChoirMember* claimSeat() {
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        if (!g_members[index].used) {
            return &g_members[index];
        }
    }
    ChoirMember* quietest = nullptr;
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        const ChoirMember& member = g_members[index];
        if (member.role == ChoirRole::Active || member.role == ChoirRole::Client
            || member.role == ChoirRole::Local) {
            continue;
        }
        if (quietest == nullptr || member.lastSeenMs < quietest->lastSeenMs) {
            quietest = &g_members[index];
        }
    }
    if (quietest != nullptr) {
        return quietest;
    }
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        if (quietest == nullptr || g_members[index].lastSeenMs < quietest->lastSeenMs) {
            quietest = &g_members[index];
        }
    }
    return quietest;
}

ChoirRole roleForEvent(const SignalEvent& event) {
    if (event.isLocal) {
        return ChoirRole::Local;
    }
    if (event.type == SrcType::BleAdv) {
        return ChoirRole::Advertising;
    }
    if (event.type == SrcType::WifiData) {
        return ChoirRole::Active;
    }
    return ChoirRole::InRange;
}

}  // namespace

void choirInit() {
    memset(g_members, 0, sizeof(g_members));
    g_memberCount = 0;
}

uint8_t choirNoteEvent(const SignalEvent& event, uint32_t nowMs) {
    refreshMembers(nowMs);

    ChoirMember* member = findMember(event.idHash);
    if (member == nullptr) {
        member = claimSeat();
        if (member == nullptr) {
            return 0;
        }
        // A seat that was already taken was vacated by a device that left, so
        // the count of members does not change.
        const bool isNewSeat = !member->used;
        member->used = true;
        member->idHash = event.idHash;
        member->firstSeenMs = nowMs;
        member->lastDataMs = 0;
        member->packets = 0;
        member->part = 0;          // release the seat the previous device held
        member->role = ChoirRole::InRange;
        member->part = nextFreePart();
        if (isNewSeat) {
            ++g_memberCount;
        }
    }

    member->lastSeenMs = nowMs;
    member->rssi = event.rssi;
    member->channel = event.channel;
    member->type = event.type;
    ++member->packets;

    const ChoirRole role = roleForEvent(event);
    if (role == ChoirRole::Active) {
        member->lastDataMs = nowMs;
    }
    if (role != ChoirRole::InRange || member->role == ChoirRole::InRange) {
        member->role = role;
    }
    return member->packets == 1 ? member->part : 0;
}

void choirSetAccessPointClients(const uint32_t* idHashes, uint8_t count,
                                uint8_t channel, uint32_t nowMs) {
    refreshMembers(nowMs);
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        ChoirMember& member = g_members[index];
        if (!member.used || member.role != ChoirRole::Client) {
            continue;
        }
        bool stillListed = false;
        for (uint8_t listed = 0; listed < count; ++listed) {
            if (idHashes[listed] == member.idHash) {
                stillListed = true;
                break;
            }
        }
        if (!stillListed) {
            // The device left the access point, so it is in range again.
            member.role = ChoirRole::InRange;
        }
    }

    for (uint8_t listed = 0; listed < count; ++listed) {
        const uint32_t idHash = idHashes[listed];
        ChoirMember* member = findMember(idHash);
        if (member == nullptr) {
            SignalEvent event{};
            event.idHash = idHash;
            event.rssi = -45;
            event.channel = channel;
            event.type = SrcType::WifiData;
            event.tMs = nowMs;
            choirNoteEvent(event, nowMs);
            member = findMember(idHash);
        }
        if (member == nullptr) {
            continue;
        }
        member->role = ChoirRole::Client;
        member->lastSeenMs = nowMs;
        member->lastDataMs = nowMs;
    }
}
uint8_t choirMemberCount() {
    return g_memberCount;
}

uint8_t choirRoleCount(ChoirRole role) {
    uint8_t count = 0;
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        if (g_members[index].used && g_members[index].role == role) {
            ++count;
        }
    }
    return count;
}

const ChoirMember& choirMemberAt(uint8_t index) {
    static const ChoirMember kEmpty{};
    if (index >= config::kChoirMaxMembers || !g_members[index].used) {
        return kEmpty;
    }
    return g_members[index];
}

const char* choirNoteName(const ChoirMember& member) {
    return voiceManagerNoteName(member.idHash);
}

const char* choirNoteNameForPart(uint8_t part) {
    for (uint8_t index = 0; index < config::kChoirMaxMembers; ++index) {
        if (g_members[index].used && g_members[index].part == part) {
            return voiceManagerNoteName(g_members[index].idHash);
        }
    }
    return "-";
}

const char* choirRoleLabel(ChoirRole role) {
    switch (role) {
        case ChoirRole::InRange: return "RNG";
        case ChoirRole::Active: return "ACT";
        case ChoirRole::Advertising: return "ADV";
        case ChoirRole::Client: return "CLI";
        case ChoirRole::Local: return "LOC";
    }
    return "RNG";
}
