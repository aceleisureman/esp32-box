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

    // ---- 多 WiFi 网络管理 ----
    // 保存的网络上限（NVS 单键容量约束）
    static constexpr uint8_t kMaxNetworks = 12;

    // 保存一个新网络（SSID 已存在则更新密码）；供配网页/串口调用
    bool addNetwork(const String &ssid, const String &password);
    // 删除一个网络，返回是否成功
    bool removeNetwork(uint8_t index);
    // 当前保存的网络数量
    uint8_t networkCount() const { return _netCount; }
    // 取第 index 个网络的 SSID / 密码，越界返回 false
    bool networkAt(uint8_t index, String &ssid, String &password) const;
    // 当前选中的 SSID（连接使用）；无则空串
    String currentSsid() const { return _currentSsid; }
    // 切换当前网络：返回 true 表示已开始连接（结果异步）
    bool selectNetwork(uint8_t index);

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
    void scheduleReconnect(const char *reason);
    void serviceReconnect();
    void resetReconnectState();
    // ---- 多网络内部实现 ----
    void loadNetworks();                  // 从 NVS 载入网络列表
    void saveNetworks();                  // 写回 NVS
    String ssidByIndex(uint8_t index) const;
    String passwordByIndex(uint8_t index) const;

    bool _active = false;
    bool _initialized = false;
    bool _connectPending = false;
    bool _stationTestPending = false;
    bool _stationStarted = false;
    bool _reconnectScheduled = false;
    bool _wasStationConnected = false;
    bool _portalStarted = false;
    bool _portalRoutesConfigured = false;
    bool _scanInProgress = false;
    int16_t _scanCount = 0;
    uint32_t _scanStartedMs = 0;      // 扫描启动时刻，用于超时兜底
    uint32_t _scanRetryMs = 0;        // 扫描失败后的重试时刻
    bool _scanFailed = false;
    bool _staPausedForScan = false;   // 已为扫描暂停 STA 连接
    String _scanResults;              // 已缓存的扫描结果 JSON 片段
    uint32_t _connectStartedMs = 0;
    uint32_t _nextReconnectMs = 0;
    uint8_t _reconnectAttempts = 0;
    String _serialLine;
    String _savedSsid;
    String _savedPassword;
    String _qrPayload;
    String _status = "WiFi setup is idle";
    String _ip = "192.168.4.1";
    // ---- 多网络状态 ----
    String _networks;                 // NVS 持久化的 JSON：{"networks":[{s,p},...],"cur":"ssid"}
    uint8_t _netCount = 0;
    String _currentSsid;              // 当前选中网络
    bool _manualSwitchRequested = false;
};

extern WifiProvisioningClass WifiProvisioning;
