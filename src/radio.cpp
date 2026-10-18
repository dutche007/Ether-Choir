// Provides a statically allocated, non-blocking radio event queue, the shared
// per-boot address salt/hash helper, and a Wi-Fi / BLE time-slice scheduler.
// The ESP32-S3 has one shared 2.4 GHz radio, so Wi-Fi promiscuous capture and
// passive BLE scanning never run at the same time: one is stopped cleanly
// before the other is started.
#include "radio.h"

#include "config.h"

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
    for (;;) {
        const RadioMode mode = g_mode.load(std::memory_order_relaxed);
        const uint32_t now = millis();
        const uint32_t sliceElapsed = now - lastSliceMs;
        const uint32_t hopElapsed = now - lastHopMs;

        const bool wantWifi = (mode == RadioMode::WifiOnly) || (mode == RadioMode::Both);
        const bool wantBle = (mode == RadioMode::BleOnly) || (mode == RadioMode::Both);

        // Channel hop within the Wi-Fi slice (150 ms dwell), independent of the
        // Wi-Fi/BLE slice boundary.
        if (wifiActive && hopElapsed >= config::kChannelDwellMs) {
            wifiNextChannel();
            lastHopMs = now;
        }

        // Decide which radio owns the slice. In Both mode the radios alternate;
        // in a single-radio mode only that radio runs.
        const bool wifiTurn = wantWifi && (!wantBle || (sliceElapsed >= config::kWifiSliceMs && wifiActive));

        if (wifiTurn && !wifiActive) {
            if (bleActive) {
                bleSliceStop();
                bleActive = false;
            }
            wifiSliceStart();
            wifiActive = true;
            lastSliceMs = now;
            lastHopMs = now;
            continue;
        }
        if (!wifiTurn && wantBle && !bleActive) {
            if (wifiActive) {
                wifiSliceStop();
                wifiActive = false;
            }
            bleSliceStart();
            bleActive = true;
            lastSliceMs = now;
            lastHopMs = now;
            continue;
        }

        // Hold the current slice until its dwell elapses, then yield to the
        // other radio (Both mode) or keep running (single-radio mode).
        if (wifiActive && sliceElapsed >= config::kWifiSliceMs) {
            if (wantBle) {
                wifiSliceStop();
                wifiActive = false;
                lastSliceMs = now;
                continue;
            }
            lastSliceMs = now;
        }
        if (bleActive && sliceElapsed >= config::kBleSliceMs) {
            if (wantWifi) {
                bleSliceStop();
                bleActive = false;
                lastSliceMs = now;
                continue;
            }
            lastSliceMs = now;
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
