// Implementation of the Cardputer ADV keyboard reader.
#include "input_keys.h"

#include <algorithm>
#include <stdint.h>

namespace {

// TCA8418 keypad matrix geometry used by the ADV.
constexpr uint8_t kMatrixRows = 7;
constexpr uint8_t kMatrixColumns = 8;

// Upper bound on events drained in a single poll.
constexpr uint8_t kMaxEventsPerPoll = 16;

// TCA8418 registers used here.
constexpr uint8_t kRegCfg = 0x01;
constexpr uint8_t kRegIntStat = 0x02;
constexpr uint8_t kRegKeyEventA = 0x04;

// CFG bit 3 turns on the overflow status bit in INT_STAT, so a lost event can
// be noticed instead of silently leaving a key stuck down.
constexpr uint8_t kCfgOverflowIntEnable = 0x08;

// INT_STAT bit 0 is the KEY_EVENT_A flag, bit 3 is the FIFO overflow flag.
constexpr uint8_t kIntStatKeyEventA = 0x01;
constexpr uint8_t kIntStatOverflow = 0x08;

}  // namespace

void Tca8418AdvKeyboardReader::begin() {
    _tca8418 = std::make_unique<Adafruit_TCA8418>();
    if (!_tca8418->begin()) {
        _tca8418.reset();
        return;
    }
    _tca8418->matrix(kMatrixRows, kMatrixColumns);
    _tca8418->flush();
    _tca8418->enableInterrupts();
    // Ask the expander to report FIFO overflows, so a burst of events that
    // outruns the reader can be detected and recovered from.
    const uint8_t cfg = _tca8418->readRegister8(kRegCfg);
    _tca8418->writeRegister8(kRegCfg, cfg | kCfgOverflowIntEnable);
}

void Tca8418AdvKeyboardReader::update() {
    if (!_tca8418) {
        return;
    }
    // Drain every queued event. An empty FIFO reads back as 0, which is the
    // only value that is not a valid event code.
    bool drained = false;
    for (uint8_t i = 0; i < kMaxEventsPerPoll; ++i) {
        const uint8_t raw = _tca8418->readRegister8(kRegKeyEventA);
        if (raw == 0) {
            drained = true;
            break;
        }
        applyEvent(raw);
    }

    const uint8_t intStat = _tca8418->readRegister8(kRegIntStat);
    const bool overflowed = (intStat & kIntStatOverflow) != 0;
    if (overflowed) {
        // The FIFO overflowed, so press and release events were lost and the
        // held key list can no longer be trusted. Throw away the stale events
        // that are still queued and release every key, so no key stays stuck
        // down and no note keeps ringing after the key was let go.
        discardQueuedEvents();
        _key_list.clear();
        ++_overflowCount;
        Serial.printf("Keyboard: FIFO overflow, keys released (resync %lu)\n",
                      static_cast<unsigned long>(_overflowCount));
    }

    // INT_STAT is write-1-to-clear, and clearing a flag discards the event it
    // belongs to. Acknowledge only KEY_EVENT_A, and only once the FIFO is
    // empty, so a queued press or release can never be thrown away. Losing a
    // release leaves the key stuck down and its note ringing on.
    uint8_t acknowledge = 0;
    if (drained) {
        acknowledge |= kIntStatKeyEventA;
    }
    if (overflowed) {
        acknowledge |= kIntStatOverflow;
    }
    if (acknowledge != 0) {
        _tca8418->writeRegister8(kRegIntStat, acknowledge);
    }
}

void Tca8418AdvKeyboardReader::discardQueuedEvents() {
    for (uint8_t i = 0; i < kMaxEventsPerPoll; ++i) {
        if (_tca8418->readRegister8(kRegKeyEventA) == 0) {
            break;
        }
    }
}

bool Tca8418AdvKeyboardReader::selfTest() {
    struct Expectation {
        uint8_t event;   // raw KEY_EVENT_A byte, bit 7 clear means press
        int x;
        int y;
        const char* name;
    };
    // The ADV scans a 7x8 matrix that is remapped onto the 4x14 key grid, so
    // the music keys 'a' to 'l' sit on grid row 2 at columns 2 to 10, ';' is
    // at (11,2) and the space bar at (13,3).
    static const Expectation kExpectations[] = {
        {0x0D, 2, 2, "a"},
        {0x11, 3, 2, "s"},
        {0x17, 4, 2, "d"},
        {0x1B, 5, 2, "f"},
        {0x21, 6, 2, "g"},
        {0x25, 7, 2, "h"},
        {0x2B, 8, 2, "j"},
        {0x2F, 9, 2, "k"},
        {0x35, 10, 2, "l"},
        {0x39, 11, 2, ";"},
        {0x44, 13, 3, "space"},
    };

    bool ok = true;
    for (const Expectation& expectation : kExpectations) {
        _key_list.clear();
        applyEvent(expectation.event);
        bool pressed = false;
        for (const Point2D_t& point : _key_list) {
            if (point.x == expectation.x && point.y == expectation.y) {
                pressed = true;
                break;
            }
        }
        if (!pressed) {
            ok = false;
            Serial.printf("Keyboard self-test: press 0x%02X did not map to '%s' (%d,%d)\n",
                          expectation.event, expectation.name, expectation.x, expectation.y);
        }
        // The matching release has to take the key back out of the list,
        // otherwise the key stays down and its note rings on.
        applyEvent(static_cast<uint8_t>(expectation.event | 0x80));
        if (!_key_list.empty()) {
            ok = false;
            Serial.printf("Keyboard self-test: release 0x%02X left %u key(s) down\n",
                          static_cast<uint8_t>(expectation.event | 0x80),
                          static_cast<unsigned>(_key_list.size()));
        }
    }

    // GPIO events share the event register and must not be read as keys.
    _key_list.clear();
    applyEvent(0x5B);
    if (!_key_list.empty()) {
        ok = false;
        Serial.println("Keyboard self-test: GPIO event was read as a key");
    }

    _key_list.clear();
    return ok;
}

void Tca8418AdvKeyboardReader::applyEvent(uint8_t rawEvent) {
    // Bit 7 set means release, clear means press. Event codes are 1-based.
    const bool pressed = (rawEvent & 0x80) == 0;
    uint16_t code = static_cast<uint16_t>(rawEvent & 0x7F);
    if (code == 0) {
        return;
    }
    --code;

    const uint8_t scanRow = static_cast<uint8_t>(code / 10);
    const uint8_t scanCol = static_cast<uint8_t>(code % 10);
    if (scanRow >= kMatrixRows || scanCol >= kMatrixColumns) {
        return;
    }

    // The ADV scans the matrix in a different order to the original
    // Cardputer; remap it onto the standard 4 row x 14 column key grid.
    uint8_t x = scanRow * 2;
    if (scanCol > 3) {
        ++x;
    }
    const uint8_t y = (scanCol + 4) % 4;
    setKeyState(x, y, pressed);
}

void Tca8418AdvKeyboardReader::setKeyState(int x, int y, bool pressed) {
    Point2D_t point;
    point.x = x;
    point.y = y;

    const auto it = std::find(_key_list.begin(), _key_list.end(), point);
    if (pressed) {
        if (it == _key_list.end()) {
            _key_list.push_back(point);
        }
    } else if (it != _key_list.end()) {
        _key_list.erase(it);
    }
}
