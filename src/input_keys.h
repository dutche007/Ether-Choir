// Owns keyboard scanning for the Cardputer ADV.
//
// The ADV scans its 4x14 matrix with a TCA8418 I2C keypad expander on the
// internal I2C bus (SDA 8 / SCL 9). This reader drains the expander's key
// event FIFO directly on every poll instead of relying on the GPIO interrupt,
// which fixes two problems with the stock M5Cardputer reader:
//   * only one event is read per interrupt, so a release can be dropped and a
//     key stays stuck down for good;
//   * an empty FIFO (event code 0) underflows the code to 65535, which adds a
//     phantom key at row 6553 and makes the key map read out of bounds.
//
// Press and release are both handled, so the key list always mirrors the
// physical matrix. If the expander's event FIFO ever overflows, the lost
// events are detected through the overflow status bit and the reader resyncs
// by releasing every key, so a key can never stay stuck down.
#pragma once

#include <memory>
#include <stdint.h>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418.h>
#include <utility/Keyboard/KeyboardReader/KeyboardReader.h>

class Tca8418AdvKeyboardReader : public KeyboardReader {
public:
    ~Tca8418AdvKeyboardReader() override = default;

    void begin() override;
    void update() override;

    // Feeds synthetic events through the decoder and checks that they land on
    // the expected keys. Used at boot to prove the press/release handling and
    // the matrix remap without needing anyone to press a key.
    bool selfTest();

private:
    void applyEvent(uint8_t rawEvent);
    void setKeyState(int x, int y, bool pressed);
    void discardQueuedEvents();

    std::unique_ptr<Adafruit_TCA8418> _tca8418;
    uint32_t _overflowCount = 0;
};
