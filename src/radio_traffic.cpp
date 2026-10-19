#include "radio_traffic.h"
#include "config.h"
#include "radio.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <stdint.h>

// Defined in radio_wifi.cpp: pins the capture radio to a fixed channel and
// turns the promiscuous capture on or off.
void wifiSetChannel(uint8_t channel);
void wifiSetPromiscuous(bool enabled);

namespace {

constexpr char kApSsid[] = "EtherChoir-Local";
constexpr char kApPassphrase[] = "choir-local-1";
// Room for a couple of test devices, so a phone or a second Cardputer can
// join the local access point and be picked up as a connected choir member.
constexpr uint8_t kApMaxClients = 4;
constexpr uint32_t kLocalEventIntervalMs = 40;
constexpr int8_t kSelfRssiBase = -52;

// The access point setup is a deep IDF call chain, so it runs on its own task
// with a generous stack instead of on the radio scheduler.
constexpr uint32_t kTrafficTaskStackWords = 4096;   // 16 KB
constexpr uint8_t kTrafficTaskPriority = 1;
constexpr uint8_t kTrafficTaskCore = 0;
constexpr uint32_t kTrafficTaskPollMs = 5;

TaskHandle_t g_task = nullptr;
std::atomic<bool> g_pendingStart{false};
std::atomic<bool> g_pendingStop{false};
bool g_active = false;
uint32_t g_eventCount = 0;
uint32_t g_lastEventMs = 0;
uint32_t g_selfIdHash = 0;
uint8_t g_selfChannel = kTrafficChannel;

void startLocalAp() {
    WiFi.mode(WIFI_AP_STA);
    // softAP() takes (ssid, passphrase, channel, ssid_hidden, max_connection,
    // ftm_responder, auth_mode, cipher). The access point accepts a single
    // client; the default auth mode is WPA2-PSK with CCMP.
    if (!WiFi.softAP(kApSsid, kApPassphrase, kTrafficChannel,
                     /*ssid_hidden=*/false, /*max_connection=*/kApMaxClients,
                     /*ftm_responder=*/false)) {
        WiFi.mode(WIFI_STA);
        Serial.println("[traffic] could not start the local access point");
        return;
    }

    // Pin the capture radio to the access point's channel.
    wifiSetChannel(kTrafficChannel);

    uint8_t apMac[6] = {0};
    WiFi.softAPmacAddress(apMac);
    g_selfIdHash = hashAddress(apMac);
    g_selfChannel = kTrafficChannel;
    g_lastEventMs = millis();
    g_active = true;

    Serial.printf("[traffic] local TX active: ap=\"%s\" ch=%u clients=%u\n",
                  kApSsid, static_cast<unsigned>(kTrafficChannel),
                  static_cast<unsigned>(WiFi.softAPgetStationNum()));
}

void stopLocalAp() {
    if (!g_active) {
        return;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_active = false;
    g_eventCount = 0;
    Serial.println("[traffic] local TX stopped");
}

// The radio cannot hear its own transmission, so the local source is added to
// the event queue directly. The RSSI is a near-field estimate, not a
// measurement.
void emitLocalEvent() {
    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - g_lastEventMs) < kLocalEventIntervalMs) {
        return;
    }
    g_lastEventMs = now;
    ++g_eventCount;

    SignalEvent event{};
    event.idHash = g_selfIdHash != 0 ? g_selfIdHash : 0xE7C401u;
    event.rssi = kSelfRssiBase + static_cast<int8_t>((g_eventCount % 7) - 3);
    event.channel = g_selfChannel;
    event.type = SrcType::WifiData;
    event.tMs = now;
    event.isLocal = true;
    radioPushEvent(event);
}

void trafficTask(void*) {
    for (;;) {
        if (g_pendingStop.exchange(false, std::memory_order_relaxed)) {
            stopLocalAp();
        }
        if (g_pendingStart.exchange(false, std::memory_order_relaxed)) {
            startLocalAp();
        }
        if (g_active) {
            emitLocalEvent();
        }
        vTaskDelay(pdMS_TO_TICKS(kTrafficTaskPollMs));
    }
}

}  // namespace

bool trafficBegin() {
    if (g_task == nullptr) {
        const BaseType_t result =
            xTaskCreatePinnedToCore(trafficTask, "traffic",
                                    kTrafficTaskStackWords, nullptr,
                                    kTrafficTaskPriority, &g_task,
                                    kTrafficTaskCore);
        if (result != pdPASS || g_task == nullptr) {
            g_task = nullptr;
            Serial.println("[traffic] could not start the traffic task");
            return false;
        }
    }
    // The access point is brought up on the traffic task so the radio
    // scheduler and the UI loop never block on it.
    g_pendingStart.store(true, std::memory_order_relaxed);
    g_pendingStop.store(false, std::memory_order_relaxed);
    return true;
}

void trafficStop() {
    g_pendingStart.store(false, std::memory_order_relaxed);
    g_pendingStop.store(true, std::memory_order_relaxed);
}

uint32_t trafficPacketCount() {
    return g_eventCount;
}

bool trafficIsActive() {
    return g_active;
}

const char* trafficAccessPointSsid() {
    return kApSsid;
}

uint8_t trafficCopyClientHashes(uint32_t* destination, uint8_t maxCount) {
    if (destination == nullptr || maxCount == 0 || !g_active) {
        return 0;
    }
    // The access point keeps a list of the stations associated with it. The
    // addresses are hashed here so no raw address leaves this module.
    wifi_sta_list_t stationList{};
    if (esp_wifi_ap_get_sta_list(&stationList) != ESP_OK) {
        return 0;
    }
    uint8_t written = 0;
    for (int index = 0; index < stationList.num && written < maxCount; ++index) {
        destination[written++] = hashAddress(stationList.sta[index].mac);
    }
    return written;
}
