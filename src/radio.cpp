// Provides a statically allocated, non-blocking radio event queue, the shared
// per-boot address salt/hash helper, and a Wi-Fi / BLE time-slice scheduler.
// The ESP32-S3 has one shared 2.4 GHz radio, so Wi-Fi promiscuous capture and
// passive BLE scanning never run at the same time: one is stopped cleanly
// before the other is started.
#include "radio.h"

#include "config.h"
#include "radio_traffic.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <esp_random.h>
#include <esp_system.h>
#include <stdint.h>

bool wifiRadioBegin();
void wifiSliceStart();
void wifiSliceStop();
void wifiNextChannel();
uint8_t wifiCurrentChannel();
bool bleRadioBegin();
void bleSliceStart();
void bleSliceStop();

namespace {

constexpr UBaseType_t kQueueCapacity = 64;

StaticQueue_t g_queueStorage;
alignas(4) uint8_t g_queueBytes[kQueueCapacity * sizeof(SignalEvent)];
QueueHandle_t g_eventQueue = nullptr;
std::atomic<uint32_t> g_queueDropCount{0};

// Per-boot salt drawn from the hardware RNG. Never persisted, so address hashes
// cannot be correlated across sessions.
uint32_t g_salt = 0;

std::atomic<RadioMode> g_mode{RadioMode::Both};

// Runtime radio timing, adjustable from the settings screen.
std::atomic<uint32_t> g_wifiSliceMs{config::kWifiSliceMs};
std::atomic<uint32_t> g_bleSliceMs{config::kBleSliceMs};
std::atomic<uint32_t> g_channelDwellMs{config::kChannelDwellMs};
std::atomic<bool> g_trafficEnabled{false};

// Scheduler task handle and state.
TaskHandle_t g_schedulerTask = nullptr;
StackType_t g_schedulerStack[config::kRadioTaskStackWords];
StaticTask_t g_schedulerControlBlock;

}  // namespace

uint32_t hashAddress(const uint8_t* address) {
    uint32_t hash = 2166136261u ^ g_salt;
    for (size_t index = 0; index < 6; ++index) {
        hash ^= address[index];
        hash *= 16777619u;
    }
    return hash;
}

void radioSchedulerTask(void*) {
    uint32_t lastSliceMs = millis();
    uint32_t lastHopMs = millis();
    bool wifiActive = false;
    bool bleActive = false;
    uint8_t channelsSwept = 0;
    for (;;) {
        const RadioMode mode = g_mode.load(std::memory_order_relaxed);
        const uint32_t now = millis();
        const uint32_t sliceElapsed = now - lastSliceMs;
        const uint32_t hopElapsed = now - lastHopMs;
        const uint32_t wifiSliceMs = g_wifiSliceMs.load(std::memory_order_relaxed);
        const uint32_t bleSliceMs = g_bleSliceMs.load(std::memory_order_relaxed);
        const uint32_t dwellMs = g_channelDwellMs.load(std::memory_order_relaxed);
        const bool traffic = g_trafficEnabled.load(std::memory_order_relaxed);

        const bool wantWifi = (mode == RadioMode::WifiOnly) || (mode == RadioMode::Both);
        const bool wantBle = (mode == RadioMode::BleOnly) || (mode == RadioMode::Both);

        // The local traffic generator needs the radio to stay on its own
        // channel, so the band sweep and the BLE slices are paused while it
        // runs.
        if (traffic) {
            if (bleActive) {
                bleSliceStop();
                bleActive = false;
            }
            if (!wifiActive) {
                wifiSliceStart();
                wifiActive = true;
                lastSliceMs = now;
                lastHopMs = now;
                channelsSwept = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Hop to the next channel within the Wi-Fi slice.
        if (wifiActive && hopElapsed >= dwellMs) {
            wifiNextChannel();
            lastHopMs = now;
            if (channelsSwept < config::kWifiChannelCount) {
                ++channelsSwept;
            }
        }

        // Decide which radio owns the slice. In Both mode the radios alternate;
        // in a single-radio mode only that radio runs.
        if (wantWifi && !wantBle) {
            // Wi-Fi only: hold the radio for the whole cycle, hopping channels.
            if (bleActive) {
                bleSliceStop();
                bleActive = false;
            }
            if (!wifiActive) {
                wifiSliceStart();
                wifiActive = true;
                lastSliceMs = now;
                lastHopMs = now;
                channelsSwept = 0;
            }
        } else if (wantBle && !wantWifi) {
            // BLE only: hold the radio for the whole cycle.
            if (wifiActive) {
                wifiSliceStop();
                wifiActive = false;
            }
            if (!bleActive) {
                bleSliceStart();
                bleActive = true;
                lastSliceMs = now;
            }
        } else if (wifiActive) {
            // Both mode: hold the Wi-Fi slice until it elapses or the whole
            // band has been swept, then hand the radio over to BLE.
            if (sliceElapsed >= wifiSliceMs
                || channelsSwept >= config::kWifiChannelCount) {
                wifiSliceStop();
                wifiActive = false;
                bleSliceStart();
                bleActive = true;
                lastSliceMs = now;
                lastHopMs = now;
                channelsSwept = 0;
            }
        } else if (bleActive) {
            // Both mode: hold the BLE slice until it elapses, then hand the
            // radio back to Wi-Fi.
            if (sliceElapsed >= bleSliceMs) {
                bleSliceStop();
                bleActive = false;
                wifiSliceStart();
                wifiActive = true;
                lastSliceMs = now;
                lastHopMs = now;
                channelsSwept = 0;
            }
        } else {
            // Both mode with neither radio running: start the Wi-Fi slice.
            wifiSliceStart();
            wifiActive = true;
            lastSliceMs = now;
            lastHopMs = now;
            channelsSwept = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


bool radioBegin() {
    g_salt = static_cast<uint32_t>(esp_random());

    g_eventQueue = xQueueCreateStatic(kQueueCapacity, sizeof(SignalEvent), g_queueBytes,
                                      &g_queueStorage);
    if (g_eventQueue == nullptr) {
        return false;
    }

    if (!wifiRadioBegin()) {
        return false;
    }
    if (!bleRadioBegin()) {
        return false;
    }

    g_schedulerTask = xTaskCreateStaticPinnedToCore(radioSchedulerTask, "radio_sched",
                                                    config::kRadioTaskStackWords, nullptr,
                                                    config::kRadioTaskPriority,
                                                    g_schedulerStack, &g_schedulerControlBlock,
                                                    config::kRadioTaskCore);
    return g_schedulerTask != nullptr;
}

void radioSetMode(RadioMode mode) {
    g_mode.store(mode, std::memory_order_relaxed);
}

RadioMode radioGetMode() {
    return g_mode.load(std::memory_order_relaxed);
}

bool radioTryReceive(SignalEvent& event) {
    return g_eventQueue != nullptr && xQueueReceive(g_eventQueue, &event, 0) == pdTRUE;
}

uint32_t radioQueueDrops() {
    return g_queueDropCount.load(std::memory_order_relaxed);
}

void radioPushEvent(const SignalEvent& event) {
    if (g_eventQueue == nullptr || xQueueSend(g_eventQueue, &event, 0) != pdTRUE) {
        g_queueDropCount.fetch_add(1, std::memory_order_relaxed);
    }
}

uint8_t radioGetCurrentChannel() {
    return wifiCurrentChannel();
}

void radioSetWifiSliceMs(uint32_t milliseconds) {
    g_wifiSliceMs.store(milliseconds, std::memory_order_relaxed);
}

uint32_t radioGetWifiSliceMs() {
    return g_wifiSliceMs.load(std::memory_order_relaxed);
}

void radioSetBleSliceMs(uint32_t milliseconds) {
    g_bleSliceMs.store(milliseconds, std::memory_order_relaxed);
}

uint32_t radioGetBleSliceMs() {
    return g_bleSliceMs.load(std::memory_order_relaxed);
}

void radioSetChannelDwellMs(uint32_t milliseconds) {
    g_channelDwellMs.store(milliseconds, std::memory_order_relaxed);
}

uint32_t radioGetChannelDwellMs() {
    return g_channelDwellMs.load(std::memory_order_relaxed);
}

void radioSetTrafficEnabled(bool enabled) {
    const bool wasEnabled = g_trafficEnabled.load(std::memory_order_relaxed);
    if (wasEnabled == enabled) {
        return;
    }
    g_trafficEnabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        trafficBegin();
    } else {
        trafficStop();
    }
}

bool radioGetTrafficEnabled() {
    return g_trafficEnabled.load(std::memory_order_relaxed);
}

uint32_t radioGetTrafficPacketCount() {
    return trafficPacketCount();
}

bool radioTrafficActive() {
    return g_trafficEnabled.load(std::memory_order_relaxed);
}
