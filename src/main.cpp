#include <Arduino.h>
#include "BluetoothA2DP.h"
#include "Display.h"
#include "Spectrum.h"
#include "Input.h"
#include "WifiProvisioning.h"
#include <esp_system.h>
#include <esp_sntp.h>
#include <time.h>

namespace {

void updateNetworkTime(bool wifiConnected) {
    static bool previousWifiConnected = false;
    static bool syncPending = false;
    static bool syncApplied = false;
    static uint32_t syncStartedMs = 0;
    static uint32_t lastPollMs = 0;

    if (wifiConnected && !previousWifiConnected) {
        // China Standard Time is UTC+8 with no daylight-saving adjustment.
        sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org",
                     "time.windows.com");
        syncPending = true;
        syncApplied = false;
        syncStartedMs = millis();
        lastPollMs = 0;
        Serial.printf("[TIME][%lu] WiFi connected; NTP sync requested\n",
                      (unsigned long)syncStartedMs);
    }

    if (!wifiConnected) {
        syncPending = false;
        syncApplied = false;
    }

    previousWifiConnected = wifiConnected;
    if (!syncPending || syncApplied || millis() - lastPollMs < 500) return;
    lastPollMs = millis();

    const time_t now = time(nullptr);
    if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        if (millis() - syncStartedMs >= 30000) {
            Serial.printf("[TIME][%lu] NTP sync timeout status=%d epoch=%ld\n",
                          (unsigned long)millis(),
                          (int)sntp_get_sync_status(),
                          (long)now);
            syncPending = false;
        }
        return;
    }
    // A valid NTP timestamp is well beyond the default pre-sync epoch.
    if (now < 1700000000) {
        if (millis() - syncStartedMs >= 30000) {
            Serial.printf("[TIME][%lu] NTP sync timeout epoch=%ld\n",
                          (unsigned long)millis(), (long)now);
            syncPending = false;
        }
        return;
    }

    struct tm localTime = {};
    if (!localtime_r(&now, &localTime)) return;
    if (localTime.tm_year + 1900 < 2024 || localTime.tm_year + 1900 > 2100) return;

    Display.setClock((uint8_t)localTime.tm_hour,
                     (uint8_t)localTime.tm_min,
                     (uint8_t)localTime.tm_sec);
    Display.setDate((uint16_t)(localTime.tm_year + 1900),
                    (uint8_t)(localTime.tm_mon + 1),
                    (uint8_t)localTime.tm_mday);
    syncApplied = true;
    syncPending = false;
    Serial.printf("[TIME][%lu] NTP sync applied %04d-%02d-%02d %02d:%02d:%02d\n",
                  (unsigned long)millis(),
                  localTime.tm_year + 1900,
                  localTime.tm_mon + 1,
                  localTime.tm_mday,
                  localTime.tm_hour,
                  localTime.tm_min,
                  localTime.tm_sec);
}

}

static const char *resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "OTHER";
    }
}

void setup() {
    Serial.begin(115200);
    delay(800);

    Serial.println();
    Serial.println("=================================");
    Serial.println("[BOOT] ESP32-S3-WROOM-1-N16R8");
    Serial.println("[BOOT] Welcome page when BT off");
    Serial.println("=================================");
    const esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.printf("[BOOT] reset reason=%d (%s) free heap=%u min heap=%u\n",
                  (int)resetReason,
                  resetReasonName(resetReason),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap());

    BluetoothA2DP.init();
    Display.init();
    Input.init();
    Spectrum.init();
    WifiProvisioning.init();
    WifiProvisioning.autoConnect();

    Serial.println("[BOOT] setup done");
}

void loop() {
    BluetoothA2DP.update();
    Input.update();

    static bool networkPage = false;
    const bool networkPageNow = Display.isNetworkSettings();
    if (networkPageNow && !networkPage) {
        Serial.printf("[LOOP][%lu][heap=%u] network page entered, starting AP provisioning\n",
                      (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
        WifiProvisioning.start();
        Serial.printf("[LOOP][%lu][heap=%u] provisioning.start returned active=%s\n",
                      (unsigned long)millis(), (unsigned)ESP.getFreeHeap(),
                      WifiProvisioning.isActive() ? "true" : "false");
    }
    if (!networkPageNow && networkPage) {
        Serial.printf("[LOOP][%lu][heap=%u] network page exited, stopping provisioning\n",
                      (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
        WifiProvisioning.stop();
    }
    networkPage = networkPageNow;
    if (Display.takeNetworkRescanRequest()) WifiProvisioning.requestScan();
    WifiProvisioning.update();
    const bool wifiConnected = WifiProvisioning.isStationConnected();
    updateNetworkTime(wifiConnected);
    Display.setWifiConnected(wifiConnected);
    Display.setNetworkServiceState(WifiProvisioning.isActive(),
                                    WifiProvisioning.status(),
                                    WifiProvisioning.ipAddress(),
                                    WifiProvisioningClass::PROVISIONING_NAME,
                                    WifiProvisioning.qrPayload());

    // 仅蓝牙已连接时跑频谱，待机页不算
    if (BluetoothA2DP.isConnected()) {
        Spectrum.update();
    }

    Display.render();
    delay(20);
}
