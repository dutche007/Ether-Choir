// Passive BLE advertisement capture for one slice of the radio scanner.
// Uses NimBLE-Arduino 2.5.1 in passive mode (setActiveScan(false)): we only
// listen to advertising headers, never transmit. The transmitter address is
// hashed immediately in the callback via the shared per-boot salt (radio.h)
// and the raw address, name, and payload are never stored, logged, or printed.
#include "radio.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <stdint.h>

namespace {

constexpr uint16_t kBleScanIntervalMs = 100;
constexpr uint16_t kBleScanWindowMs = 100;

// Statically allocated so it outlives the scan session; owned by the BLE slice.
NimBLEScan* g_scan = nullptr;

class BleScanCallbacks : public NimBLEScanCallbacks {
  public:
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        if (advertisedDevice == nullptr) {
            return;
        }

        const NimBLEAddress& address = advertisedDevice->getAddress();
        const uint8_t* addressVal = address.getVal();
        if (addressVal == nullptr) {
            return;
        }

        SignalEvent event{};
        event.idHash = hashAddress(addressVal);
        event.rssi = advertisedDevice->getRSSI();
        event.channel = 0; // BLE uses advertising channels 37/38/39; 0 marks BLE.
        event.type = SrcType::BleAdv;
        event.tMs = millis();

        radioPushEvent(event);
    }
};

BleScanCallbacks g_bleCallbacks;

}  // namespace

bool bleRadioBegin() {
    if (!NimBLEDevice::init(std::string("EtherChoir"))) {
        return false;
    }

    g_scan = NimBLEDevice::getScan();
    if (g_scan == nullptr) {
        return false;
    }

    g_scan->setScanCallbacks(&g_bleCallbacks, false);
    g_scan->setActiveScan(false);
    g_scan->setInterval(kBleScanIntervalMs);
    g_scan->setWindow(kBleScanWindowMs);
    return true;
}

void bleSliceStart() {
    if (g_scan == nullptr) {
        return;
    }
    if (!g_scan->isScanning()) {
        g_scan->start(0, false, true); // 0 = scan until stop()
    }
}

void bleSliceStop() {
    if (g_scan == nullptr) {
        return;
    }
    if (g_scan->isScanning()) {
        g_scan->stop();
    }
}
