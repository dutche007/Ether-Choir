// Passive Wi-Fi promiscuous capture for one slice of the radio scheduler.
// Captures management/data header metadata only; transmitter address is hashed
// immediately in the callback via the shared per-boot salt (declared in radio.h).
// Nothing raw is stored, logged, or printed.
#include "radio.h"

#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/task.h>
#include <stdint.h>

namespace {

uint8_t g_currentChannel = config::kFirstWifiChannel;

void wifiSniffCallback(void* buffer, wifi_promiscuous_pkt_type_t packetType) {
    if (packetType != WIFI_PKT_MGMT && packetType != WIFI_PKT_DATA) {
        return;
    }

    const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
    if (packet->rx_ctrl.sig_len < 24) {
        return;
    }

    const uint8_t* frame = packet->payload;
    const uint8_t frameType = (frame[0] >> 2) & 0x03;
    const uint8_t subtype = (frame[0] >> 4) & 0x0F;
    if ((packetType == WIFI_PKT_MGMT && frameType != 0)
        || (packetType == WIFI_PKT_DATA && frameType != 2)) {
        return;
    }

    SignalEvent event{};
    event.idHash = hashAddress(frame + 10);
    event.rssi = packet->rx_ctrl.rssi;
    event.channel = packet->rx_ctrl.channel;
    event.tMs = millis();

    if (packetType == WIFI_PKT_DATA) {
        event.type = SrcType::WifiData;
    } else if (subtype == 8) {
        event.type = SrcType::WifiBeacon;
    } else if (subtype == 4) {
        event.type = SrcType::WifiProbeReq;
    } else if (subtype == 5) {
        event.type = SrcType::WifiProbeResp;
    } else {
        event.type = SrcType::WifiOther;
    }

    radioPushEvent(event);
}

}  // namespace

bool wifiRadioBegin() {
    if (!WiFi.mode(WIFI_STA)) {
        return false;
    }

    wifi_promiscuous_filter_t filter{};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK
        || esp_wifi_set_promiscuous_rx_cb(wifiSniffCallback) != ESP_OK
        || esp_wifi_set_channel(g_currentChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }
    // Promiscuous mode stays off until the scheduler grants Wi-Fi a slice.
    esp_wifi_set_promiscuous(false);
    return true;
}

void wifiSliceStart() {
    // While the local traffic generator runs, the radio stays on the access
    // point's channel so the channel is not changed underneath it.
    if (!radioTrafficActive()) {
        esp_wifi_set_channel(g_currentChannel, WIFI_SECOND_CHAN_NONE);
    }
    esp_wifi_set_promiscuous(true);
}

void wifiSetChannel(uint8_t channel) {
    if (channel < config::kFirstWifiChannel || channel > config::kLastWifiChannel) {
        return;
    }
    g_currentChannel = channel;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

void wifiSliceStop() {
    esp_wifi_set_promiscuous(false);
}

void wifiNextChannel() {
    g_currentChannel = g_currentChannel >= config::kLastWifiChannel
        ? config::kFirstWifiChannel
        : static_cast<uint8_t>(g_currentChannel + 1);
    esp_wifi_set_channel(g_currentChannel, WIFI_SECOND_CHAN_NONE);
}

uint8_t wifiCurrentChannel() {
    return g_currentChannel;
}

void wifiSetPromiscuous(bool enabled) {
    esp_wifi_set_promiscuous(enabled);
}
