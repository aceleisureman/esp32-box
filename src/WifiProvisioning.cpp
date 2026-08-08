#include "WifiProvisioning.h"

#include <Preferences.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

namespace {
Preferences prefs;
DNSServer dnsServer;
WebServer portalServer(80);

constexpr size_t kSerialLineCapacity = 128;
constexpr uint8_t kDnsPort = 53;
constexpr uint8_t kMaxPortalNetworks = 20;
const IPAddress kPortalIp(192, 168, 4, 1);
const IPAddress kPortalGateway(192, 168, 4, 1);
const IPAddress kPortalSubnet(255, 255, 255, 0);

const char kPortalHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi &#37197;&#32593;</title><style>
*{box-sizing:border-box}body{margin:0;background:#101114;color:#f4f5f7;font-family:system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:460px;margin:0 auto;padding:28px 18px}h1{font-size:26px;margin:0 0 7px}p{color:#a8adb7;margin:0 0 24px;line-height:1.55}
.panel{border:1px solid #30343b;background:#181a1f;padding:18px;border-radius:8px}label{display:block;font-size:14px;color:#c7cad1;margin:0 0 7px}
select,input{width:100%;height:46px;border:1px solid #3a3e47;border-radius:6px;background:#101114;color:#fff;padding:0 12px;font-size:16px;margin-bottom:16px}
.row{display:flex;gap:10px}.row select{flex:1}.row button{width:92px}button{height:46px;border:0;border-radius:6px;background:#f6a800;color:#17130a;font-size:15px;font-weight:700;padding:0 14px}
button.secondary{background:#2b2e35;color:#e5e7eb}.submit{width:100%}.status{min-height:22px;margin-top:15px;color:#f6a800;font-size:14px}
.meta{margin-top:18px;padding-top:16px;border-top:1px solid #30343b;color:#858b96;font-size:13px}
</style></head><body><main><h1>WiFi &#37197;&#32593;</h1>
<p>&#36873;&#25321;&#38468;&#36817;&#30340; 2.4GHz WiFi&#65292;&#36755;&#20837;&#23494;&#30721;&#21518;&#36830;&#25509;&#12290;</p>
<section class="panel"><form method="post" action="/save">
<label for="ssid">WiFi &#32593;&#32476;</label><div class="row"><select id="ssid" name="ssid" required><option value="">&#27491;&#22312;&#25628;&#32034;...</option></select>
<button class="secondary" type="button" id="rescan">&#37325;&#26032;&#25628;&#32034;</button></div>
<label for="password">WiFi &#23494;&#30721;</label><input id="password" name="password" type="password" maxlength="63" autocomplete="current-password" placeholder="&#35831;&#36755;&#20837;&#23494;&#30721;">
<button class="submit" type="submit">&#20445;&#23384;&#24182;&#36830;&#25509;</button></form><div id="status" class="status"></div>
<div class="meta">&#37197;&#32593;&#28909;&#28857;&#65306;MY-SMALL-BOX<br>&#37197;&#32593;&#22320;&#22336;&#65306;192.168.4.1</div></section></main>
<script>
const list=document.getElementById('ssid'),statusEl=document.getElementById('status');
async function loadNetworks(){try{const r=await fetch('/scan',{cache:'no-store'}),d=await r.json();
if(d.scanning){statusEl.textContent='\u6b63\u5728\u641c\u7d22\u9644\u8fd1 WiFi...';setTimeout(loadNetworks,900);return;}
list.innerHTML='';if(!d.networks.length){list.innerHTML='<option value="">\u672a\u627e\u5230 WiFi</option>';statusEl.textContent='\u672a\u627e\u5230\u7f51\u7edc\uff0c\u8bf7\u91cd\u65b0\u641c\u7d22';return;}
d.networks.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+'  '+n.rssi+' dBm'+(n.secure?'  \ud83d\udd12':'');list.appendChild(o)});
statusEl.textContent='\u5df2\u627e\u5230 '+d.networks.length+' \u4e2a WiFi';}catch(e){statusEl.textContent='\u52a0\u8f7d\u5931\u8d25\uff0c\u8bf7\u91cd\u8bd5';}}
document.getElementById('rescan').onclick=async()=>{statusEl.textContent='\u6b63\u5728\u91cd\u65b0\u641c\u7d22...';await fetch('/rescan',{method:'POST'});setTimeout(loadNetworks,500)};
loadNetworks();</script></body></html>
)HTML";

const char kSavedHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>&#27491;&#22312;&#36830;&#25509;</title><style>body{margin:0;background:#101114;color:#f4f5f7;font-family:system-ui;text-align:center;padding:52px 22px}h1{font-size:25px}p{color:#a8adb7;line-height:1.6}.dot{width:13px;height:13px;background:#f6a800;border-radius:50%;margin:0 auto 22px}</style></head>
<body><div class="dot"></div><h1>&#24050;&#20445;&#23384; WiFi</h1><p>&#35774;&#22791;&#27491;&#22312;&#36830;&#25509;&#65292;&#35831;&#26597;&#30475;&#23631;&#24149;&#29366;&#24577;&#12290;</p></body></html>
)HTML";

String jsonEscape(const String &value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); i++) {
        const uint8_t ch = (uint8_t)value[i];
        if (ch == '"' || ch == '\\') {
            escaped += '\\';
            escaped += (char)ch;
        } else if (ch < 0x20) {
            char encoded[7];
            snprintf(encoded, sizeof(encoded), "\\u%04x", ch);
            escaped += encoded;
        } else {
            escaped += (char)ch;
        }
    }
    return escaped;
}

void wifiDebug(const char *phase) {
    Serial.printf("[WIFI][%lu][heap=%u] %s\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(), phase);
}
}

WifiProvisioningClass WifiProvisioning;
constexpr const char *WifiProvisioningClass::PROVISIONING_NAME;
constexpr const char *WifiProvisioningClass::PROVISIONING_PASSWORD;

void WifiProvisioningClass::init() {
    wifiDebug("init: enter");
    if (_initialized) {
        wifiDebug("init: already initialized");
        return;
    }

    // Boot initialization only restores credentials. The radio starts when the
    // user opens the setup page.
    if (prefs.begin("wifi", true)) {
        _savedSsid = prefs.getString("ssid", "");
        _savedPassword = prefs.getString("password", "");
        prefs.end();
    } else {
        _savedSsid = "";
        _savedPassword = "";
        Serial.printf("[WIFI][%lu][heap=%u] init: preferences unavailable\n",
                      (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    }
    _serialLine.reserve(kSerialLineCapacity);
    _qrPayload.reserve(96);
    configurePortalRoutes();
    _initialized = true;
    Serial.printf("[WIFI][%lu][heap=%u] init: saved ssid length=%u password length=%u\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)_savedSsid.length(),
                  (unsigned)_savedPassword.length());
}

void WifiProvisioningClass::start() {
    wifiDebug("start: enter (AP portal provisioning)");
    init();
    if (_active) {
        wifiDebug("start: already active");
        return;
    }

    _active = true;
    _connectPending = false;
    _stationTestPending = false;
    _serialLine = "";
    _qrPayload = "WIFI:T:WPA;S:";
    _qrPayload += PROVISIONING_NAME;
    _qrPayload += ";P:";
    _qrPayload += PROVISIONING_PASSWORD;
    _qrPayload += ";;";
    _status = "Starting setup hotspot";
    _ip = kPortalIp.toString();
    startPortal();
    if (!_portalStarted) _qrPayload = "";
    Serial.printf("[WIFI] portal: ssid=%s ip=%s\n",
                  PROVISIONING_NAME, _ip.c_str());
    Serial.println("[WIFI] commands: WIFI:<ssid>|<password>  RESCAN  STA_TEST  CONNECT  STATUS");
}

void WifiProvisioningClass::autoConnect() {
    init();
    if (_stationStarted) {
        wifiDebug("autoConnect: station already started");
        return;
    }
    if (!_savedSsid.length()) {
        Serial.println("[WIFI] autoConnect: no saved SSID; waiting for provisioning");
        return;
    }

    Serial.printf("[WIFI][%lu][heap=%u] autoConnect: using saved credentials ssid length=%u\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)_savedSsid.length());
    beginStation();
}

void WifiProvisioningClass::stop() {
    wifiDebug("stop: enter");
    if (!_active) {
        wifiDebug("stop: already inactive");
        return;
    }
    _active = false;
    _connectPending = false;
    _stationTestPending = false;
    _serialLine = "";
    stopPortal();
    _qrPayload = "";
    _status = _stationStarted ? "WiFi connection active" : "WiFi setup stopped";
    _ip = isStationConnected() ? WiFi.localIP().toString() : "0.0.0.0";
}

void WifiProvisioningClass::configurePortalRoutes() {
    if (_portalRoutesConfigured) return;

    portalServer.on("/", HTTP_GET, [this]() { handlePortalRoot(); });
    portalServer.on("/scan", HTTP_GET, [this]() { handlePortalScan(); });
    portalServer.on("/rescan", HTTP_POST, [this]() {
        requestScan();
        portalServer.send(202, "application/json", "{\"scanning\":true}");
    });
    portalServer.on("/save", HTTP_POST, [this]() { handlePortalSave(); });
    portalServer.onNotFound([this]() { handlePortalRoot(); });
    _portalRoutesConfigured = true;
}

void WifiProvisioningClass::startPortal() {
    wifiDebug("portal: before WiFi.mode(WIFI_AP_STA)");
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP_STA)) {
        _status = "WiFi AP+STA start failed";
        wifiDebug("portal: WiFi.mode(WIFI_AP_STA) failed");
        return;
    }
    wifiDebug("portal: after WiFi.mode(WIFI_AP_STA)");

    if (!WiFi.softAPConfig(kPortalIp, kPortalGateway, kPortalSubnet)) {
        _status = "Setup hotspot IP failed";
        wifiDebug("portal: softAPConfig failed");
        return;
    }
    if (!WiFi.softAP(PROVISIONING_NAME, PROVISIONING_PASSWORD)) {
        _status = "Setup hotspot start failed";
        wifiDebug("portal: softAP failed");
        return;
    }

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(kDnsPort, "*", kPortalIp);
    portalServer.begin();
    _portalStarted = true;
    _ip = WiFi.softAPIP().toString();
    _status = "Scanning nearby WiFi";
    beginWifiScan();
    wifiDebug("portal: AP, DNS and web server ready");
}

void WifiProvisioningClass::stopPortal() {
    if (_portalStarted) {
        dnsServer.stop();
        portalServer.stop();
    }
    WiFi.scanDelete();
    _scanInProgress = false;
    _scanCount = 0;
    WiFi.softAPdisconnect(true);
    WiFi.mode(_stationStarted ? WIFI_STA : WIFI_OFF);
    _portalStarted = false;
    wifiDebug("portal: stopped");
}

void WifiProvisioningClass::handlePortalRoot() {
    portalServer.send_P(200, "text/html; charset=utf-8", kPortalHtml);
}

void WifiProvisioningClass::handlePortalScan() {
    const int16_t scanState = WiFi.scanComplete();
    const bool scanning = _scanInProgress || scanState == WIFI_SCAN_RUNNING;
    String response;
    response.reserve(1024);
    response = "{\"scanning\":";
    response += scanning ? "true" : "false";
    response += ",\"networks\":[";

    if (!scanning && scanState >= 0) {
        uint8_t emitted = 0;
        for (int16_t i = 0; i < scanState && emitted < kMaxPortalNetworks; i++) {
            const String ssid = WiFi.SSID(i);
            if (!ssid.length()) continue;

            bool duplicate = false;
            for (int16_t previous = 0; previous < i; previous++) {
                if (WiFi.SSID(previous) == ssid) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            if (emitted++) response += ',';
            response += "{\"ssid\":\"";
            response += jsonEscape(ssid);
            response += "\",\"rssi\":";
            response += String(WiFi.RSSI(i));
            response += ",\"secure\":";
            response += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true";
            response += '}';
        }
    }
    response += "]}";
    portalServer.send(200, "application/json; charset=utf-8", response);
}

void WifiProvisioningClass::handlePortalSave() {
    const String ssid = portalServer.arg("ssid");
    const String password = portalServer.arg("password");
    if (!ssid.length() || ssid.length() > 32 || password.length() > 63) {
        portalServer.send(400, "text/plain; charset=utf-8", "Invalid WiFi credentials");
        return;
    }

    saveCredentials(ssid, password);
    _connectPending = true;
    portalServer.send_P(200, "text/html; charset=utf-8", kSavedHtml);
    Serial.printf("[WIFI][%lu][heap=%u] portal credentials accepted ssid length=%u password length=%u\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ssid.length(),
                  (unsigned)password.length());
}

void WifiProvisioningClass::beginWifiScan() {
    if (!_active || !_portalStarted) return;
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        _status = "Scanning nearby WiFi";
        _scanInProgress = true;
        return;
    }

    WiFi.scanDelete();
    _scanCount = 0;
    _scanInProgress = true;
    _status = "Scanning nearby WiFi";
    const int16_t result = WiFi.scanNetworks(true, true);
    Serial.printf("[WIFI][%lu][heap=%u] scan started result=%d\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (int)result);
    if (result == WIFI_SCAN_FAILED) {
        _scanInProgress = false;
        _status = "WiFi scan failed; select RESCAN";
    }
}

void WifiProvisioningClass::requestScan() {
    if (!_active || !_portalStarted) {
        Serial.println("[WIFI] RESCAN rejected: setup portal is inactive");
        return;
    }
    beginWifiScan();
}

void WifiProvisioningClass::pollWifiScan() {
    if (!_scanInProgress) return;
    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;

    _scanInProgress = false;
    if (result < 0) {
        _scanCount = 0;
        _status = "WiFi scan failed; select RESCAN";
        Serial.printf("[WIFI][%lu][heap=%u] scan failed result=%d\n",
                      (unsigned long)millis(),
                      (unsigned)ESP.getFreeHeap(),
                      (int)result);
        return;
    }

    _scanCount = result;
    _status = "Found ";
    _status += String(_scanCount);
    _status += " nearby WiFi";
    Serial.printf("[WIFI][%lu][heap=%u] scan complete networks=%d\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (int)_scanCount);
}

bool WifiProvisioningClass::isStationConnected() const {
    return _stationStarted && WiFi.status() == WL_CONNECTED;
}

void WifiProvisioningClass::saveCredentials(const String &ssid, const String &password) {
    _savedSsid = ssid;
    _savedPassword = password;
    if (prefs.begin("wifi", false)) {
        prefs.putString("ssid", _savedSsid);
        prefs.putString("password", _savedPassword);
        prefs.end();
    }
    _status = "WiFi credentials saved";
    _ip = _portalStarted ? kPortalIp.toString() : "SERIAL";
    Serial.printf("[WIFI][%lu][heap=%u] serial WIFI saved ssid length=%u password length=%u\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)_savedSsid.length(),
                  (unsigned)_savedPassword.length());
}

void WifiProvisioningClass::beginStation() {
    if (!_savedSsid.length()) {
        _status = "SSID required: send WIFI first";
        Serial.println("[WIFI] CONNECT rejected: no SSID");
        return;
    }

    _status = "Starting WiFi STA";
    _ip = _portalStarted ? kPortalIp.toString() : "SERIAL";
    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: before WiFi STA init\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap());

    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: before WiFi.persistent(false)\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    WiFi.persistent(false);
    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: after WiFi.persistent(false)\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    const wifi_mode_t requestedMode = _portalStarted ? WIFI_AP_STA : WIFI_STA;
    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: before WiFi.mode(%d)\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap(),
                  (int)requestedMode);
    const bool modeOk = WiFi.mode(requestedMode);
    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: after WiFi.mode(%d)\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap(),
                  (int)requestedMode);
    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: WIFI_STA returned=%s mode=%d\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  modeOk ? "true" : "false",
                  (int)WiFi.getMode());
    if (!modeOk) {
        _status = "WiFi STA start failed";
        return;
    }

    WiFi.begin(_savedSsid.c_str(), _savedPassword.c_str());
    _stationStarted = true;
    _connectStartedMs = millis();
    _status = "Connecting to selected WiFi";
    wifiDebug("CONNECT: WiFi.begin issued");
}

void WifiProvisioningClass::runStationStartTest() {
    _status = "Testing WiFi STA start";
    _ip = "SERIAL";
    Serial.printf("[WIFI][%lu][heap=%u] STA_TEST: before WiFi.persistent(false)\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    WiFi.persistent(false);
    Serial.printf("[WIFI][%lu][heap=%u] STA_TEST: after WiFi.persistent(false)\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    Serial.printf("[WIFI][%lu][heap=%u] STA_TEST: before WiFi.mode(WIFI_STA)\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    const bool modeOk = WiFi.mode(WIFI_STA);
    Serial.printf("[WIFI][%lu][heap=%u] STA_TEST: after WiFi.mode(WIFI_STA) returned=%s mode=%d\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  modeOk ? "true" : "false",
                  (int)WiFi.getMode());
    _status = modeOk ? "WiFi STA test passed" : "WiFi STA test failed";
}

void WifiProvisioningClass::handleSerialCommand(const String &command) {
    if (!command.length()) return;

    if (command.startsWith("WIFI:")) {
        if (!_active) {
            Serial.println("[WIFI] WIFI rejected: open the WiFi settings page first");
            return;
        }
        const String payload = command.substring(5);
        const int separator = payload.indexOf('|');
        if (separator <= 0) {
            _status = "Format: WIFI:<ssid>|<password>";
            Serial.println("[WIFI] command rejected: missing SSID or '|'");
            return;
        }
        const String ssid = payload.substring(0, separator);
        const String password = payload.substring(separator + 1);
        if (ssid.length() > 32 || password.length() > 63) {
            _status = "SSID/password too long";
            Serial.println("[WIFI] command rejected: field length limit exceeded");
            return;
        }
        saveCredentials(ssid, password);
        Serial.println("[WIFI] OK WIFI");
    } else if (command == "CONNECT") {
        if (!_active) {
            Serial.println("[WIFI] CONNECT rejected: open the WiFi settings page first");
            return;
        }
        _connectPending = true;
        Serial.println("[WIFI] OK CONNECT queued");
    } else if (command == "STA_TEST") {
        _stationTestPending = true;
        Serial.println("[WIFI] OK STA_TEST queued (no WiFi.begin)");
    } else if (command == "RESCAN") {
        requestScan();
    } else if (command == "STATUS") {
        Serial.printf("[WIFI] STATUS active=%s station=%s status=%s ip=%s\n",
                      _active ? "true" : "false",
                      _stationStarted ? "started" : "stopped",
                      _status.c_str(), _ip.c_str());
    } else {
        _status = "Unknown command";
        Serial.printf("[WIFI] unknown command: %s\n", command.c_str());
    }
}

void WifiProvisioningClass::refreshStatus() {
    if (!_stationStarted) return;
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
        _status = "WiFi connected";
        _ip = _portalStarted ? WiFi.softAPIP().toString()
                             : WiFi.localIP().toString();
    } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
               (_connectStartedMs && millis() - _connectStartedMs >= 20000)) {
        _status = "Connection failed; check password";
        _ip = _portalStarted ? WiFi.softAPIP().toString() : "SERIAL";
    } else {
        _status = "Connecting to selected WiFi";
        _ip = _portalStarted ? WiFi.softAPIP().toString() : "SERIAL";
    }
    static int lastLoggedStatus = -100;
    if ((int)status != lastLoggedStatus) {
        lastLoggedStatus = (int)status;
        Serial.printf("[WIFI][%lu][heap=%u] status: wl=%d active=%s ip=%s message=%s\n",
                      (unsigned long)millis(),
                      (unsigned)ESP.getFreeHeap(),
                      (int)status,
                      _active ? "true" : "false",
                      _ip.c_str(), _status.c_str());
    }
}

void WifiProvisioningClass::update() {
    // Keep diagnostic commands available on every page. Credential changes and
    // connection attempts are still gated by _active in handleSerialCommand().
    while (Serial.available() > 0) {
        const char ch = (char)Serial.read();
        if (ch == '\r') continue;
        if (ch == '\n') {
            handleSerialCommand(_serialLine);
            _serialLine = "";
        } else if (_serialLine.length() < kSerialLineCapacity - 1) {
            _serialLine += ch;
        } else {
            _serialLine = "";
            _status = "Command too long";
            Serial.println("[WIFI] command rejected: line too long");
        }
    }

    if (_active && _portalStarted) {
        dnsServer.processNextRequest();
        portalServer.handleClient();
        pollWifiScan();
    }

    if (_active && _connectPending) {
        _connectPending = false;
        beginStation();
    }

    if (_stationTestPending) {
        _stationTestPending = false;
        runStationStartTest();
    }

    static uint32_t statusTick = 0;
    if (_stationStarted && millis() - statusTick >= 1000) {
        statusTick = millis();
        refreshStatus();
    }
}
