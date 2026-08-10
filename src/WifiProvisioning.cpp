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
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kWifiReconnectInitialDelayMs = 2000;
constexpr uint32_t kWifiReconnectMaxDelayMs = 30000;
const IPAddress kPortalIp(192, 168, 4, 1);
const IPAddress kPortalGateway(192, 168, 4, 1);
const IPAddress kPortalSubnet(255, 255, 255, 0);

const char kPortalHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b1220">
<title>WiFi 配网</title>
<style>
:root{color-scheme:dark;--bg:#0b1220;--card:#121c2d;--card-2:#172337;--line:#26364d;--text:#f4f7fb;--muted:#91a0b5;--accent:#39d0c8;--accent-strong:#79eee4;--danger:#ff7f86;--radius:20px}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;background:radial-gradient(circle at 50% -10%,#1d3950 0,#0b1220 48%);color:var(--text);font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
button,input,select{font:inherit}
button{cursor:pointer}
.shell{width:min(100%,500px);margin:0 auto;padding:24px 16px 32px}
.header{padding:4px 4px 20px}
.eyebrow{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:12px;letter-spacing:.04em}
.eyebrow strong{color:var(--accent-strong);font-weight:650}
.online-dot,.status-dot{width:8px;height:8px;border-radius:50%;background:var(--accent);box-shadow:0 0 0 4px #39d0c81c}
.header h1{margin:16px 0 8px;font-size:32px;line-height:1.15;letter-spacing:-.03em}
.header p{max-width:410px;margin:0;color:var(--muted);font-size:15px;line-height:1.65}
.card{padding:20px;border:1px solid #ffffff0d;border-radius:var(--radius);background:linear-gradient(145deg,#17253a,#101a2a);box-shadow:0 20px 45px #0000002b}
.steps{display:flex;align-items:center;gap:10px;margin-bottom:22px;color:var(--muted);font-size:12px}
.step{display:flex;align-items:center;gap:7px;white-space:nowrap}
.step-num{display:grid;place-items:center;width:22px;height:22px;border:1px solid var(--line);border-radius:50%;color:var(--text);font-size:11px}
.step.active{color:var(--text)}
.step.active .step-num{border-color:var(--accent);background:#39d0c81c;color:var(--accent-strong)}
.step-line{flex:1;height:1px;background:var(--line)}
.field{margin-top:18px}
.field-head{display:flex;align-items:baseline;justify-content:space-between;gap:12px;margin-bottom:9px}
.field label{font-size:14px;font-weight:650}
.field-hint,.network-count{color:var(--muted);font-size:12px}
.control-row{display:flex;gap:10px}
.select-wrap{position:relative;flex:1}
.select-wrap:after{position:absolute;right:14px;top:50%;width:7px;height:7px;border-right:2px solid var(--muted);border-bottom:2px solid var(--muted);transform:translateY(-70%) rotate(45deg);pointer-events:none;content:""}
select,input{width:100%;height:50px;border:1px solid var(--line);border-radius:12px;background:#0b1423;color:var(--text);outline:none;padding:0 42px 0 14px;font-size:15px;transition:border-color .18s,box-shadow .18s}
select:focus,input:focus{border-color:var(--accent);box-shadow:0 0 0 3px #39d0c81f}
select{-webkit-appearance:none;appearance:none}
.control-row>#rescan{flex:0 0 82px;height:50px;border:1px solid var(--line);border-radius:12px;background:var(--card-2);color:var(--text);font-size:13px;font-weight:650}
button:disabled{cursor:wait;opacity:.55}
.password-wrap{position:relative}
.password-wrap input{padding-right:70px}
.toggle-password{position:absolute;right:8px;top:7px;height:36px;border:0;border-radius:9px;background:transparent;color:var(--accent-strong);font-size:12px;font-weight:650;padding:0 10px}
.toggle-password:hover{background:#ffffff0b}
.primary{display:flex;align-items:center;justify-content:center;gap:10px;width:100%;height:52px;margin-top:24px;border:0;border-radius:13px;background:var(--accent);color:#061918;font-size:15px;font-weight:750;box-shadow:0 8px 20px #39d0c82e}
.primary:hover{background:var(--accent-strong)}
.primary span{font-size:20px;line-height:0}
.status{display:flex;align-items:center;gap:9px;min-height:22px;margin-top:16px;color:var(--muted);font-size:13px}
.status[data-state=loading]{color:var(--accent-strong)}
.status[data-state=error]{color:var(--danger)}
.status[data-state=error] .status-dot{background:var(--danger);box-shadow:0 0 0 4px #ff7f861c}
.info{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:12px}
.info-item{padding:13px 14px;border:1px solid #ffffff0b;border-radius:13px;background:#0d1727}
.info-item span{display:block;margin-bottom:5px;color:var(--muted);font-size:11px}
.info-item strong{display:block;overflow:hidden;color:var(--text);font-size:13px;font-weight:650;text-overflow:ellipsis;white-space:nowrap}
.saved{max-width:430px;margin:9vh auto 0;text-align:center}
.saved .mark{display:grid;place-items:center;width:54px;height:54px;margin:0 auto 20px;border:1px solid var(--accent);border-radius:50%;background:#39d0c81c;color:var(--accent-strong);font-size:28px}
.saved h1{margin:0 0 10px;font-size:27px}
.saved p{margin:0;color:var(--muted);font-size:15px;line-height:1.7}
.saved .next{margin-top:22px;padding:14px;border-radius:12px;background:#0d1727;color:var(--muted);font-size:13px}
.saved a{display:inline-block;margin-top:20px;color:var(--accent-strong);font-size:13px;text-decoration:none}
@media (max-width:380px){.shell{padding:18px 12px 26px}.card{padding:16px}.header h1{font-size:29px}.info-item{padding:11px}.control-row>#rescan{flex-basis:74px}}
</style>
</head>
<body>
<main class="shell">
<header class="header">
  <div class="eyebrow"><span class="online-dot"></span><span>设备热点</span><strong>MY-SMALL-BOX</strong></div>
  <h1>连接到 WiFi</h1>
  <p>选择附近的 2.4GHz 网络，输入密码后保存。设备会自动尝试连接。</p>
</header>
<section class="card">
  <div class="steps" aria-label="配网步骤">
    <div class="step active"><span class="step-num">1</span><span>选择网络</span></div>
    <span class="step-line"></span>
    <div class="step active"><span class="step-num">2</span><span>输入密码</span></div>
  </div>
  <form method="post" action="/save" id="wifiForm">
    <div class="field">
      <div class="field-head"><label for="ssid">WiFi 网络</label><span id="networkCount" class="network-count">扫描中</span></div>
      <div class="control-row">
        <div class="select-wrap"><select id="ssid" name="ssid" required aria-describedby="networkCount"><option value="">正在搜索附近 WiFi…</option></select></div>
        <button type="button" id="rescan">刷新</button>
      </div>
    </div>
    <div class="field">
      <div class="field-head"><label for="password">WiFi 密码</label><span class="field-hint">开放网络可留空</span></div>
      <div class="password-wrap"><input id="password" name="password" type="password" maxlength="63" autocomplete="current-password" placeholder="请输入密码"><button type="button" class="toggle-password" id="togglePassword">显示</button></div>
    </div>
    <button class="primary" type="submit" id="saveButton"><span>保存并连接</span><span aria-hidden="true">→</span></button>
  </form>
  <div id="status" class="status" data-state="loading" role="status" aria-live="polite"><span class="status-dot"></span><span class="status-text">正在搜索附近 WiFi…</span></div>
</section>
<section class="info" aria-label="配网信息">
  <div class="info-item"><span>热点名称</span><strong>MY-SMALL-BOX</strong></div>
  <div class="info-item"><span>管理地址</span><strong>192.168.4.1</strong></div>
</section>
</main>
<script>
const list=document.getElementById('ssid');
const statusBox=document.getElementById('status');
const statusText=statusBox.querySelector('.status-text');
const countEl=document.getElementById('networkCount');
const rescan=document.getElementById('rescan');
const form=document.getElementById('wifiForm');
const saveButton=document.getElementById('saveButton');
const password=document.getElementById('password');
const togglePassword=document.getElementById('togglePassword');
let pollTimer=0;
function setStatus(message,state='info'){
  statusText.textContent=message;
  statusBox.dataset.state=state;
}
function showEmpty(message){
  list.innerHTML='';
  const option=document.createElement('option');
  option.value='';option.textContent=message;list.appendChild(option);
}
async function loadNetworks(){
  window.clearTimeout(pollTimer);
  try{
    const response=await fetch('/scan',{cache:'no-store'});
    if(!response.ok) throw new Error('scan');
    const data=await response.json();
    if(data.scanning){
      rescan.disabled=true;countEl.textContent='扫描中';
      setStatus('正在搜索附近 WiFi…','loading');
      pollTimer=window.setTimeout(loadNetworks,700);return;
    }
    rescan.disabled=false;
    list.innerHTML='';
    if(!data.networks.length){
      showEmpty('未找到 WiFi，请点击刷新');countEl.textContent='暂无网络';
      setStatus('没有发现可用网络','error');return;
    }
    data.networks.forEach(network=>{
      const option=document.createElement('option');
      option.value=network.ssid;
      option.textContent=network.ssid+(network.secure?'  · 已加密':'  · 开放');
      list.appendChild(option);
    });
    countEl.textContent=data.networks.length+' 个网络';
    setStatus('已找到附近网络，请选择一个','info');
  }catch(error){
    rescan.disabled=false;countEl.textContent='加载失败';
    showEmpty('网络列表加载失败');setStatus('加载失败，请点击刷新重试','error');
  }
}
rescan.addEventListener('click',async()=>{
  rescan.disabled=true;countEl.textContent='扫描中';setStatus('正在重新搜索附近 WiFi…','loading');
  try{await fetch('/rescan',{method:'POST'});}catch(error){}
  pollTimer=window.setTimeout(loadNetworks,250);
});
togglePassword.addEventListener('click',()=>{
  const visible=password.type==='text';
  password.type=visible?'password':'text';
  togglePassword.textContent=visible?'显示':'隐藏';
});
form.addEventListener('submit',()=>{
  saveButton.disabled=true;saveButton.querySelector('span').textContent='正在连接…';
  setStatus('凭据已保存，设备正在连接','loading');
});
loadNetworks();
</script>
</body>
</html>
)HTML";

const char kSavedHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b1220">
<title>正在连接 WiFi</title>
<style>
:root{color-scheme:dark;--bg:#0b1220;--card:#121c2d;--line:#26364d;--text:#f4f7fb;--muted:#91a0b5;--accent:#39d0c8;--accent-strong:#79eee4}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 50% -10%,#1d3950 0,#0b1220 48%);color:var(--text);font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
.saved{width:min(calc(100% - 32px),430px);margin:10vh auto 0;padding:28px 22px;border:1px solid #ffffff0d;border-radius:20px;background:linear-gradient(145deg,#17253a,#101a2a);text-align:center;box-shadow:0 20px 45px #0000002b}
.mark{display:grid;place-items:center;width:56px;height:56px;margin:0 auto 20px;border:1px solid var(--accent);border-radius:50%;background:#39d0c81c;color:var(--accent-strong);font-size:28px}
.eyebrow{margin:0 0 14px;color:var(--muted);font-size:12px;letter-spacing:.08em;text-transform:uppercase}.saved h1{margin:0 0 10px;font-size:28px}.saved p{margin:0;color:var(--muted);font-size:15px;line-height:1.7}.next{margin-top:22px;padding:13px 14px;border-radius:12px;background:#0d1727;color:var(--muted);font-size:13px}.saved a{display:inline-block;margin-top:20px;color:var(--accent-strong);font-size:13px;text-decoration:none}
</style>
</head>
<body>
<main class="saved">
  <div class="mark" aria-hidden="true">✓</div>
  <p class="eyebrow">WIFI SETUP</p>
  <h1>正在连接 WiFi</h1>
  <p>网络信息已保存，设备正在尝试连接。请返回设备屏幕查看最新状态。</p>
  <div class="next">连接成功后，设备会自动关闭配网热点。</div>
  <a href="/">返回配网页</a>
</main>
</body>
</html>
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

// 取 JSON 字符串字面量的值，处理转义。起点 p 必须指向开引号后的第一个
// 字符；返回解码后的内容，并把 endOut 指到收尾引号。
// 必须与 jsonEscape 配对使用：SSID/密码里的引号若不解码，
// 还原出来的凭据会在第一个引号处被截断（连不上且难以察觉）。
String jsonUnescapeAt(const char *p, const char **endOut) {
    String out;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    // \uXXXX：本用途只会出现 <0x20 的控制字符
                    if (p[1] && p[2] && p[3] && p[4]) {
                        char hex[5] = {p[1], p[2], p[3], p[4], '\0'};
                        out += (char)strtol(hex, nullptr, 16);
                        p += 4;
                    }
                    break;
                }
                default: out += *p; break;   // \" 与 \\ 走这里
            }
            p++;
        } else {
            out += *p++;
        }
    }
    if (endOut) *endOut = p;
    return out;
}

// 定位第 index 个 key 的值（key 形如 "s" 或 "p"），带转义解码。
// 扫描时跳过字符串字面量内部，避免值里含 "s": 之类的内容被误判为键。
String jsonFieldAt(const String &json, const char *key, uint8_t index) {
    const char *p = json.c_str();
    const size_t keyLen = strlen(key);
    uint8_t seen = 0;
    bool inStr = false;

    while (*p) {
        if (inStr) {
            // 字符串内部：只管跳过，转义字符连带跳过
            if (*p == '\\' && p[1]) p += 2;
            else if (*p == '"') { inStr = false; p++; }
            else p++;
            continue;
        }
        if (*p == '"') {
            // 可能是一个键：比对 "key":
            if (strncmp(p + 1, key, keyLen) == 0 && p[1 + keyLen] == '"') {
                const char *q = p + 2 + keyLen;
                while (*q == ' ') q++;
                if (*q == ':') {
                    q++;
                    while (*q == ' ') q++;
                    if (*q == '"') {
                        q++;
                        const char *end = nullptr;
                        String val = jsonUnescapeAt(q, &end);
                        if (seen == index) return val;
                        seen++;
                        p = end && *end ? end + 1 : q;
                        continue;
                    }
                }
            }
            inStr = true;
            p++;
            continue;
        }
        p++;
    }
    return "";
}

// 统计数组里 key 出现的次数。与 jsonFieldAt 同口径：跳过字符串字面量
// 内部，值里含 "s": 之类的内容不会被误算成一个条目。
uint8_t jsonFieldCount(const String &json, const char *key, uint8_t limit) {
    const char *p = json.c_str();
    const size_t keyLen = strlen(key);
    uint8_t n = 0;
    bool inStr = false;

    while (*p && n < limit) {
        if (inStr) {
            if (*p == '\\' && p[1]) p += 2;
            else if (*p == '"') { inStr = false; p++; }
            else p++;
            continue;
        }
        if (*p == '"') {
            if (strncmp(p + 1, key, keyLen) == 0 && p[1 + keyLen] == '"') {
                const char *q = p + 2 + keyLen;
                while (*q == ' ') q++;
                if (*q == ':') {
                    q++;
                    while (*q == ' ') q++;
                    if (*q == '"') {
                        n++;
                        q++;
                        const char *end = nullptr;
                        jsonUnescapeAt(q, &end);   // 跳过整个值
                        p = end && *end ? end + 1 : q;
                        continue;
                    }
                }
            }
            inStr = true;
            p++;
            continue;
        }
        p++;
    }
    return n;
}

void wifiDebug(const char *phase) {
    Serial.printf("[WIFI][%lu][heap=%u] %s\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(), phase);
}

// 断开原因码 → 可读名称。ESP-IDF 4.4 的 wifi_err_reason_t（esp_wifi_types.h）
const char *wifiDisconnectReasonName(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
        case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
        case WIFI_REASON_NOT_AUTHED: return "NOT_AUTHED";
        case WIFI_REASON_NOT_ASSOCED: return "NOT_ASSOCED";
        case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
        case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "DISASSOC_PWRCAP_BAD";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "DISASSOC_SUPCHAN_BAD";
        case WIFI_REASON_IE_INVALID: return "IE_INVALID";
        case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
        case WIFI_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
        case WIFI_REASON_AKMP_INVALID: return "AKMP_INVALID";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION";
        case WIFI_REASON_INVALID_RSN_IE_CAP: return "INVALID_RSN_IE_CAP";
        case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
        case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
        case WIFI_REASON_AP_TSF_RESET: return "AP_TSF_RESET";
        case WIFI_REASON_ROAMING: return "ROAMING";
        default: return "UNKNOWN";
    }
}

// STA 事件落到串口，用于区分"主动断开/自伤"与"路由器/密码/频段拒绝"。
// 打印三个阶段：CONNECTED（握手完成，进入 DHCP）、GOT_IP（彻底连上）、
// DISCONNECTED（断开原因）。只读日志，不改任何状态；init() 里注册一次。
void onWifiEventLog(arduino_event_t *event) {
    switch (event->event_id) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED: {
            const wifi_event_sta_connected_t &info = event->event_info.wifi_sta_connected;
            Serial.printf("[WIFI][%lu][heap=%u] STA_CONNECTED ssid=%.*s channel=%u auth=%u\n",
                          (unsigned long)millis(),
                          (unsigned)ESP.getFreeHeap(),
                          (int)info.ssid_len,
                          (const char *)info.ssid,
                          (unsigned)info.channel,
                          (unsigned)info.authmode);
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
            const ip_event_got_ip_t &info = event->event_info.got_ip;
            Serial.printf("[WIFI][%lu][heap=%u] STA_GOT_IP ip=" IPSTR " gw=" IPSTR " mask=" IPSTR "\n",
                          (unsigned long)millis(),
                          (unsigned)ESP.getFreeHeap(),
                          IP2STR(&info.ip_info.ip),
                          IP2STR(&info.ip_info.gw),
                          IP2STR(&info.ip_info.netmask));
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t &info = event->event_info.wifi_sta_disconnected;
            Serial.printf("[WIFI][%lu][heap=%u] STA_DISCONNECTED reason=%u (%s) ssid=%.*s rssi=%d\n",
                          (unsigned long)millis(),
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)info.reason,
                          wifiDisconnectReasonName(info.reason),
                          (int)info.ssid_len,
                          (const char *)info.ssid,
                          (int)info.rssi);
            break;
        }
        default:
            break;
    }
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

    _serialLine.reserve(kSerialLineCapacity);
    _qrPayload.reserve(96);

    // 读旧版单网络键（用于迁移），再加载多网络列表
    if (prefs.begin("wifi", true)) {
        _savedSsid = prefs.getString("ssid", "");
        _savedPassword = prefs.getString("password", "");
        prefs.end();
    }
    loadNetworks();

    // STA 连接各阶段（握手完成 / 拿到 IP / 断开）落到串口，
    // 帮助区分自伤与外部拒绝。回调按 event_id 分发，三次注册指向同一个。
    WiFi.onEvent(onWifiEventLog, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(onWifiEventLog, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(onWifiEventLog, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    configurePortalRoutes();
    _initialized = true;
    Serial.printf("[WIFI][%lu][heap=%u] init: %u saved network(s), current=%s\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)_netCount,
                  _currentSsid.c_str());
}

// ---- 多网络管理：NVS 以单条 JSON 持久化，兼容旧的单 ssid/password 键 ----
// 格式：{"networks":[{"s":"..","p":".."},...],"cur":"当前ssid"}

void WifiProvisioningClass::loadNetworks() {
    _networks = "";
    _netCount = 0;
    _currentSsid = "";

    if (prefs.begin("wifi", true)) {
        _networks = prefs.getString("networks", "");
        if (_networks.length()) _currentSsid = prefs.getString("cur", "");
        prefs.end();
    } else {
        Serial.println("[WIFI] init: preferences unavailable");
        return;
    }

    // 旧版单网络数据迁移：若从未存过 networks，但有旧的 ssid/password 键
    if (!_networks.length() && _savedSsid.length()) {
        // _savedSsid/_savedPassword 由 init 早期读入，这里搬到列表里
        _networks = "{\"networks\":[{\"s\":\"" + jsonEscape(_savedSsid) +
                    "\",\"p\":\"" + jsonEscape(_savedPassword) +
                    "\"}],\"cur\":\"" + jsonEscape(_savedSsid) + "\"}";
        _currentSsid = _savedSsid;
        _netCount = 1;          // 迁移后列表里就是这一条
        saveNetworks();
        Serial.println("[WIFI] migrated legacy single-network data");
    } else if (_networks.length()) {
        // 按 SSID 字段计数（跳过字符串内部，值里含 "s": 不会被误算）
        _netCount = jsonFieldCount(_networks, "s", kMaxNetworks);
    }
    Serial.printf("[WIFI] loadNetworks: %u network(s), cur=%s\n",
                  (unsigned)_netCount, _currentSsid.c_str());
}

void WifiProvisioningClass::saveNetworks() {
    if (!prefs.begin("wifi", false)) return;
    prefs.putString("networks", _networks);
    prefs.putString("cur", _currentSsid);
    prefs.end();
}

String WifiProvisioningClass::ssidByIndex(uint8_t index) const {
    if (index >= _netCount) return "";
    return jsonFieldAt(_networks, "s", index);
}

String WifiProvisioningClass::passwordByIndex(uint8_t index) const {
    if (index >= _netCount) return "";
    return jsonFieldAt(_networks, "p", index);
}

bool WifiProvisioningClass::networkAt(uint8_t index,
                                      String &ssid, String &password) const {
    if (index >= _netCount) return false;
    ssid = ssidByIndex(index);
    password = passwordByIndex(index);
    return ssid.length() > 0;
}

bool WifiProvisioningClass::addNetwork(const String &ssid,
                                       const String &password) {
    if (!ssid.length()) return false;

    // 已存在则只更新密码（保持顺序）
    for (uint8_t i = 0; i < _netCount; i++) {
        if (ssidByIndex(i) == ssid) {
            // 重建列表，仅更新第 i 个的密码
            String rebuilt = "{\"networks\":[";
            for (uint8_t j = 0; j < _netCount; j++) {
                if (j) rebuilt += ",";
                rebuilt += "{\"s\":\"" + jsonEscape(ssidByIndex(j)) +
                           "\",\"p\":\"" + jsonEscape(j == i ? password
                                                   : passwordByIndex(j)) +
                           "\"}";
            }
            rebuilt += "],\"cur\":\"" + jsonEscape(_currentSsid) + "\"}";
            _networks = rebuilt;
            saveNetworks();
            return true;
        }
    }

    if (_netCount >= kMaxNetworks) {
        Serial.println("[WIFI] network list full");
        return false;
    }
    // 追加到末尾
    String rebuilt = "{\"networks\":[";
    for (uint8_t j = 0; j < _netCount; j++) {
        if (j) rebuilt += ",";
        rebuilt += "{\"s\":\"" + jsonEscape(ssidByIndex(j)) +
                   "\",\"p\":\"" + jsonEscape(passwordByIndex(j)) + "\"}";
    }
    if (_netCount) rebuilt += ",";
    rebuilt += "{\"s\":\"" + jsonEscape(ssid) +
               "\",\"p\":\"" + jsonEscape(password) + "\"}";
    rebuilt += "],\"cur\":\"" + jsonEscape(_currentSsid) + "\"}";
    _networks = rebuilt;
    _netCount++;
    saveNetworks();
    Serial.printf("[WIFI] added network: %s (%u total)\n",
                  ssid.c_str(), (unsigned)_netCount);
    return true;
}

bool WifiProvisioningClass::removeNetwork(uint8_t index) {
    if (index >= _netCount) return false;
    const String removed = ssidByIndex(index);

    // 若删除的是当前网络，当前连接不动，只清掉"当前"标记避免重启又连
    if (removed == _currentSsid) _currentSsid = "";

    String rebuilt = "{\"networks\":[";
    for (uint8_t j = 0; j < _netCount; j++) {
        if (j == index) continue;
        if (rebuilt.length() > 14) rebuilt += ",";
        rebuilt += "{\"s\":\"" + jsonEscape(ssidByIndex(j)) +
                   "\",\"p\":\"" + jsonEscape(passwordByIndex(j)) + "\"}";
    }
    rebuilt += "],\"cur\":\"" + jsonEscape(_currentSsid) + "\"}";
    _networks = rebuilt;
    _netCount--;
    saveNetworks();
    Serial.printf("[WIFI] removed network: %s (%u left)\n",
                  removed.c_str(), (unsigned)_netCount);
    return true;
}

bool WifiProvisioningClass::selectNetwork(uint8_t index) {
    if (index >= _netCount) return false;
    const String ssid = ssidByIndex(index);
    const String pass = passwordByIndex(index);
    if (!ssid.length()) return false;

    _currentSsid = ssid;
    _savedSsid = ssid;
    _savedPassword = pass;
    saveNetworks();

    // 若正在配网页，先退出（避免 AP+STA 同时拉扯）
    if (_portalStarted) {
        stopPortal();
    }
    resetReconnectState();
    _stationStarted = true;
    beginStation();
    Serial.printf("[WIFI] switching to: %s\n", ssid.c_str());
    return true;
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

    // 开机连接当前选中网络；没有选中则取列表第一个
    if (!_currentSsid.length() && _netCount > 0) {
        _currentSsid = ssidByIndex(0);
        saveNetworks();
        Serial.printf("[WIFI] autoConnect: default to first network %s\n",
                      _currentSsid.c_str());
    }
    if (!_currentSsid.length()) {
        Serial.println("[WIFI] autoConnect: no saved SSID; waiting for provisioning");
        return;
    }

    // 按 ssid 找对应密码；区分"找到了（开放网络空密码是合法的）"与
    // "列表里根本没有当前 SSID"两种情形，避免把开放网络误判成找不到
    // 而悄悄切去连列表第一个网络。
    _savedSsid = _currentSsid;
    _savedPassword = "";
    bool foundCurrent = false;
    for (uint8_t i = 0; i < _netCount; i++) {
        if (ssidByIndex(i) == _currentSsid) {
            _savedPassword = passwordByIndex(i);
            foundCurrent = true;
            break;
        }
    }
    if (!foundCurrent && _netCount > 0) {
        _currentSsid = ssidByIndex(0);
        _savedPassword = passwordByIndex(0);
        saveNetworks();
    }

    Serial.printf("[WIFI][%lu][heap=%u] autoConnect: using saved credentials ssid=%s\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  _currentSsid.c_str());
    beginStation();
}

void WifiProvisioningClass::stop() {
    wifiDebug("stop: enter");
    if (!_active) {
        wifiDebug("stop: already inactive");
        return;
    }
    _active = false;
    // 注意：不清 _connectPending。用户刚在配网页点了保存，连接请求还在
    // 队列里；此时若离开网络设置页（或热点被关闭）就把它丢掉，新配的
    // WiFi 永远不会被连接——表现就是"扫码配网后不自动连接"。
    // 下面会兜底把它执行掉。
    _stationTestPending = false;
    _serialLine = "";
    stopPortal();
    _qrPayload = "";

    // 兜底：有待处理的连接请求就立刻执行（stop 之后 update() 里
    // `_active && _connectPending` 的条件不再成立，不执行就永远丢失）
    if (_connectPending) {
        _connectPending = false;
        Serial.println("[WIFI] stop: flushing pending connect request");
        beginStation();
    }

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
    _scanResults = "[]";
    _scanFailed = false;
    _staPausedForScan = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(_stationStarted ? WIFI_STA : WIFI_OFF);
    _portalStarted = false;
    // 配网页里为扫描暂停过 STA 的话，退出后立即恢复重连
    if (_stationStarted && !isStationConnected()) {
        scheduleReconnect("portal closed");
    }
    wifiDebug("portal: stopped");
}

void WifiProvisioningClass::handlePortalRoot() {
    portalServer.send_P(200, "text/html; charset=utf-8", kPortalHtml);
}

void WifiProvisioningClass::handlePortalScan() {
    // 读缓存的扫描结果，不直接访问内核扫描缓冲区（已被释放，避免竞态）
    String response;
    response.reserve(1024);
    response = "{\"scanning\":";
    response += _scanInProgress ? "true" : "false";
    response += ",\"networks\":";
    response += _scanResults;
    response += "}";
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

    // 正在扫描时忽略重复请求；结果已就绪但未消费时立即重扫
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        _status = "Scanning nearby WiFi";
        _scanInProgress = true;
        return;
    }

    // STA 连接中会占射频（esp_wifi_scan_start 立即 -2），而 disconnect
    // 是异步的——断开事件落在扫描中途会把扫描中止（跑几秒后变 -2）。
    // 所以分两步：先停连接（含 SDK autoReconnect），借重试通道等 600ms
    // 让断开事件落地，下一轮再真正启动扫描。
    if (_stationStarted && WiFi.status() != WL_CONNECTED && !_staPausedForScan) {
        Serial.println("[WIFI] scan: pause STA connect attempts to free the radio");
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false);
        _connectStartedMs = 0;
        _staPausedForScan = true;
        _scanInProgress = false;
        _scanFailed = true;
        _scanRetryMs = millis() + 600;
        _status = "Scanning nearby WiFi";
        return;
    }

    WiFi.scanDelete();
    _scanCount = 0;
    _scanResults = "[]";
    _scanFailed = false;
    _scanInProgress = true;
    _scanStartedMs = millis();
    _status = "Scanning nearby WiFi";

    // async 主动扫描 + 显示隐藏 SSID。每信道 150ms：AP 模式带客户端时
    // SDK 扫描变慢，默认 300ms/信道会撞上库内 6s 软超时（result 提前变 -2）。
    const int16_t result = WiFi.scanNetworks(true, true, false, 150);
    Serial.printf("[WIFI][%lu][heap=%u] scan started result=%d\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  (int)result);
    if (result == WIFI_SCAN_FAILED) {
        _scanInProgress = false;
        _scanFailed = true;
        // 刚打断连接时 STA 尚未完全空闲，短间隔重试即可成功
        _scanRetryMs = millis() + 1500;
        _status = "WiFi scan failed; retrying...";
        Serial.println("[WIFI] scan failed immediately, retry scheduled");
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

    // 硬超时兜底：SDK 扫描在 AP 带客户端时可能拖很久，超时强制重扫
    if (millis() - _scanStartedMs > 10000) {
        _scanInProgress = false;
        _scanFailed = true;
        _scanRetryMs = millis() + 1000;
        _status = "WiFi scan timeout; retrying...";
        Serial.println("[WIFI] scan timeout, retrying");
        WiFi.scanDelete();
        return;
    }

    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;
    if (result == WIFI_SCAN_FAILED) {
        // Arduino 库的软超时会在 SDK 完成前清掉扫描标志并报 -2，
        // 但 SCAN_DONE 事件稍后仍会带着结果到达——继续等硬超时，
        // 不要在这里判死，否则刚出炉的结果会被丢弃并无限重扫。
        return;
    }

    _scanInProgress = false;

    // 构建结果 JSON 缓存，随后立即释放内核扫描缓冲区，
    // 避免高频 /scan 轮询叠加导致下一次扫描 WIFI_SCAN_FAILED。
    _scanResults = "[";
    uint8_t emitted = 0;
    for (int16_t i = 0; i < result && emitted < kMaxPortalNetworks; i++) {
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

        if (emitted++) _scanResults += ',';
        _scanResults += "{\"ssid\":\"";
        _scanResults += jsonEscape(ssid);
        _scanResults += "\",\"rssi\":";
        _scanResults += String(WiFi.RSSI(i));
        _scanResults += ",\"secure\":";
        _scanResults += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true";
        _scanResults += '}';
    }
    _scanResults += "]";

    WiFi.scanDelete();
    _scanCount = emitted;
    _scanFailed = false;
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
    // 存入多网络列表，并设为当前选中
    addNetwork(ssid, password);
    _currentSsid = ssid;
    _savedSsid = ssid;
    _savedPassword = password;
    saveNetworks();
    _status = "WiFi credentials saved";
    _ip = _portalStarted ? kPortalIp.toString() : "SERIAL";
    Serial.printf("[WIFI][%lu][heap=%u] credentials saved: %s (%u networks)\n",
                  (unsigned long)millis(),
                  (unsigned)ESP.getFreeHeap(),
                  ssid.c_str(), (unsigned)_netCount);
}

void WifiProvisioningClass::resetReconnectState() {
    _reconnectScheduled = false;
    _wasStationConnected = false;
    _nextReconnectMs = 0;
    _reconnectAttempts = 0;
    _connectStartedMs = 0;
}

void WifiProvisioningClass::scheduleReconnect(const char *reason) {
    if (!_stationStarted || !_savedSsid.length() || _reconnectScheduled) return;

    uint8_t delayShift = _reconnectAttempts;
    if (delayShift > 4) delayShift = 4;
    uint32_t delayMs = kWifiReconnectInitialDelayMs << delayShift;
    if (delayMs > kWifiReconnectMaxDelayMs) delayMs = kWifiReconnectMaxDelayMs;

    const uint32_t now = millis();
    _nextReconnectMs = now + delayMs;
    _reconnectScheduled = true;
    _connectStartedMs = 0;
    _status = _wasStationConnected ? "WiFi disconnected; retrying"
                                   : "Connection failed; retrying";
    _ip = _portalStarted ? kPortalIp.toString() : "SERIAL";
    Serial.printf("[WIFI][%lu][heap=%u] reconnect scheduled in %lums attempt=%u wl=%d reason=%s\n",
                  (unsigned long)now,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned long)delayMs,
                  (unsigned)(_reconnectAttempts + 1),
                  (int)WiFi.status(),
                  reason ? reason : "unknown");
}

void WifiProvisioningClass::serviceReconnect() {
    if (!_stationStarted || !_reconnectScheduled) return;

    const uint32_t now = millis();
    if ((int32_t)(now - _nextReconnectMs) < 0) return;

    // 配网页打开期间不做后台重连：用户正在重新配网，重连的 STA 连接
    // 会抢射频导致扫描失败。提交新凭据走 beginStation 直连不受影响，
    // 退出配网页后重连恢复。
    if (_portalStarted) {
        _nextReconnectMs = now + 1000;
        return;
    }

    _reconnectScheduled = false;
    if (_reconnectAttempts < 255) ++_reconnectAttempts;

    const wifi_mode_t requestedMode = _portalStarted ? WIFI_AP_STA : WIFI_STA;
    bool modeOk = (WiFi.getMode() & WIFI_MODE_STA) != 0;
    if (!modeOk) {
        WiFi.persistent(false);
        modeOk = WiFi.mode(requestedMode);
    }

    bool connectAccepted = false;
    if (modeOk) {
        // 不走 WiFi.reconnect()：它内部是 esp_wifi_disconnect()+esp_wifi_connect()，
        // 异步断开事件会打断紧随的 connect（同 beginWifiScan 注释里的竞态），
        // 把刚起的关联从零重来。直接 WiFi.begin 重放凭据更稳。
        WiFi.setAutoReconnect(true);
        const wl_status_t beginStatus =
            WiFi.begin(_savedSsid.c_str(), _savedPassword.c_str());
        connectAccepted = beginStatus != WL_CONNECT_FAILED;
        Serial.printf("[WIFI][%lu][heap=%u] reconnect WiFi.begin wl=%d\n",
                      (unsigned long)millis(),
                      (unsigned)ESP.getFreeHeap(),
                      (int)beginStatus);
    }

    if (connectAccepted) {
        _connectStartedMs = millis();
        _status = "Reconnecting to WiFi";
        _ip = _portalStarted ? kPortalIp.toString() : "SERIAL";
        Serial.printf("[WIFI][%lu][heap=%u] reconnect attempt=%u issued wl=%d\n",
                      (unsigned long)_connectStartedMs,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)_reconnectAttempts,
                      (int)WiFi.status());
    } else {
        Serial.printf("[WIFI][%lu][heap=%u] reconnect attempt=%u could not start mode=%d\n",
                      (unsigned long)millis(),
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)_reconnectAttempts,
                      (int)WiFi.getMode());
        scheduleReconnect("start failed");
    }
}

void WifiProvisioningClass::beginStation() {
    if (!_savedSsid.length()) {
        _status = "SSID required: send WIFI first";
        Serial.println("[WIFI] CONNECT rejected: no SSID");
        return;
    }

    // A user-triggered connection (including new credentials) starts a fresh
    // retry window. Subsequent retries are handled by serviceReconnect().
    resetReconnectState();
    _staPausedForScan = false;
    _stationStarted = true;
    _status = "Starting WiFi STA";
    _ip = _portalStarted ? kPortalIp.toString() : "SERIAL";
    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: before WiFi STA init\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap());

    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: before WiFi.persistent(false)\n",
                  (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: after WiFi.persistent(false) autoReconnect=true\n",
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
        _status = "WiFi STA start failed; retrying";
        scheduleReconnect("STA mode failed");
        return;
    }

    const wl_status_t beginStatus =
        WiFi.begin(_savedSsid.c_str(), _savedPassword.c_str());
    _connectStartedMs = millis();
    _status = "Connecting to selected WiFi";
    Serial.printf("[WIFI][%lu][heap=%u] CONNECT: WiFi.begin issued wl=%d\n",
                  (unsigned long)_connectStartedMs,
                  (unsigned)ESP.getFreeHeap(),
                  (int)beginStatus);
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

    const uint32_t now = millis();
    const wl_status_t status = WiFi.status();
    // 连接中（_connectStartedMs 非零）的 WL_DISCONNECTED 是关联未完成的正常
    // 中间态（STA_START 事件就会置为 6），不是失败。只有明确失败码或
    // 超时才判失败，避免把刚发起的连接在几毫秒内拆掉重来。
    const bool connecting = _connectStartedMs != 0;
    const bool connectTimedOut =
        connecting && now - _connectStartedMs >= kWifiConnectTimeoutMs;
    const bool linkLost = status == WL_DISCONNECTED || status == WL_CONNECTION_LOST;
    const bool connectionLost =
        _wasStationConnected && !connecting && linkLost;
    const bool connectionFailed =
        status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL || connectTimedOut;

    if (status == WL_CONNECTED) {
        _wasStationConnected = true;
        _reconnectScheduled = false;
        _nextReconnectMs = 0;
        _reconnectAttempts = 0;
        _connectStartedMs = 0;
        _status = "WiFi connected";
        _ip = _portalStarted ? WiFi.softAPIP().toString()
                             : WiFi.localIP().toString();
    } else {
        if (connectionFailed || connectionLost) {
            const char *reason = connectTimedOut ? "connect timeout"
                                 : (connectionLost ? "connection lost" : "connect failed");
            scheduleReconnect(reason);
        }
        // 配网页扫描期间状态行归扫描所有，避免每秒被重连文案覆盖闪烁
        if (!(_portalStarted && (_scanInProgress || _scanFailed))) {
            _status = _reconnectScheduled
                          ? (_wasStationConnected ? "WiFi disconnected; retrying"
                                                  : "Connection failed; retrying")
                          : "Connecting to selected WiFi";
        }
        _ip = _portalStarted ? WiFi.softAPIP().toString() : "SERIAL";
    }

    static int lastLoggedStatus = -100;
    if ((int)status != lastLoggedStatus) {
        lastLoggedStatus = (int)status;
        Serial.printf("[WIFI][%lu][heap=%u] status: wl=%d active=%s ip=%s message=%s\n",
                      (unsigned long)now,
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
        // 扫描失败后自动重试，无需用户手动点刷新
        if (!_scanInProgress && _scanFailed &&
            (int32_t)(millis() - _scanRetryMs) >= 0) {
            beginWifiScan();
        }
    }

    // 待连接请求与 _active 无关：串口配网时 _active 为 false，
    // 若要求 _active 才处理，串口存的凭据同样连不上
    if (_connectPending) {
        _connectPending = false;
        beginStation();
    }

    if (_stationTestPending) {
        _stationTestPending = false;
        runStationStartTest();
    }

    static uint32_t statusTick = 0;
    const uint32_t now = millis();
    if (_stationStarted && now - statusTick >= 1000) {
        statusTick = now;
        refreshStatus();
    }
    serviceReconnect();
}
