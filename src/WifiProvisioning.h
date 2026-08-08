#pragma once

#include <Arduino.h>

class WifiProvisioningClass {
public:
    void init();
    void autoConnect();
    void start();
    void stop();
    void update();
    void requestScan();

    bool isActive() const { return _active; }
    bool isStationConnected() const;
    const char *status() const { return _status.c_str(); }
    const char *ipAddress() const { return _ip.c_str(); }
    const char *qrPayload() const { return _qrPayload.c_str(); }
    static constexpr const char *PROVISIONING_NAME = "MY-SMALL-BOX";
    static constexpr const char *PROVISIONING_PASSWORD = "12345678";

private:
    void startPortal();
    void stopPortal();
    void configurePortalRoutes();
    void handlePortalRoot();
    void handlePortalScan();
    void handlePortalSave();
    void beginWifiScan();
    void pollWifiScan();
    void handleSerialCommand(const String &command);
    void saveCredentials(const String &ssid, const String &password);
    void runStationStartTest();
    void beginStation();
    void refreshStatus();

    bool _active = false;
    bool _initialized = false;
    bool _connectPending = false;
    bool _stationTestPending = false;
    bool _stationStarted = false;
    bool _portalStarted = false;
    bool _portalRoutesConfigured = false;
    bool _scanInProgress = false;
    int16_t _scanCount = 0;
    uint32_t _connectStartedMs = 0;
    String _serialLine;
    String _savedSsid;
    String _savedPassword;
    String _qrPayload;
    String _status = "WiFi setup is idle";
    String _ip = "192.168.4.1";
};

extern WifiProvisioningClass WifiProvisioning;
