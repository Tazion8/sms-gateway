#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <pdulib.h>
#define ENABLE_SMTP
#define ENABLE_DEBUG
#include <ReadyMail.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ========== 串口引脚 ==========
#define TXD 3
#define RXD 4
#define MODEM_EN_PIN 5
#define LED_BUILTIN 8

// ========== 默认配置 ==========
#define DEFAULT_WIFI_SSID   "你的WiFi名称"
#define DEFAULT_WIFI_PASS   "你的WiFi密码"
#define AP_SSID             "SMS-AP"
#define AP_PASS             "12345678"
#define DEFAULT_WEB_USER    "admin"
#define DEFAULT_WEB_PASS    "admin"

// ========== 默认推送配置 ==========
#define DEFAULT_SMTP_SERVER "smtp.qq.com"
#define DEFAULT_SMTP_PORT   465
#define DEFAULT_SMTP_USER   "2764467512@qq.com"
#define DEFAULT_SMTP_PASS   "dsfotrqtrmiedggj"
#define DEFAULT_SMTP_TO     "你的邮箱号码"
#define DEFAULT_WX_WEBHOOK  "https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=xxxxxxxxxxxx"

// ========== 远程服务器配置 ==========
#define DEFAULT_REMOTE_URL  "https://你的域名/api.php"
#define DEFAULT_REMOTE_KEY  "123456"
#define REMOTE_POLL_INTERVAL_MS  2000
#define REMOTE_REPORT_INTERVAL_MS 30000
#define REMOTE_HEARTBEAT_INTERVAL_MS 60000

// ========== 配置结构体 ==========
struct Config {
  String wifiSsid;
  String wifiPass;
  String webUser;
  String webPass;
  String smtpServer;
  int    smtpPort;
  String smtpUser;
  String smtpPass;
  String smtpSendTo;
  String wxWebhook;
  bool   wxEnabled;
  bool   emailEnabled;
  String remoteUrl;
  String remoteKey;
  bool   remoteEnabled;
};

Config config;
Preferences preferences;
WebServer server(80);
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);

bool inApMode = false;

#define SERIAL_BUFFER_SIZE 500
char serialBuf[SERIAL_BUFFER_SIZE];
int serialBufLen = 0;

// ========== 长短信合并 ==========
#define MAX_CONCAT_PARTS 10
#define CONCAT_TIMEOUT_MS 30000
#define MAX_CONCAT_MESSAGES 5

struct SmsPart { bool valid; String text; };
struct ConcatSms {
  bool inUse;
  int refNumber;
  String sender;
  String timestamp;
  int totalParts;
  int receivedParts;
  unsigned long firstPartTime;
  SmsPart parts[MAX_CONCAT_PARTS];
};
ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];

// ========== 远程待发送短信队列 ==========
#define MAX_REMOTE_SMS_QUEUE 10
struct RemoteSms {
  bool valid;
  String id;
  String phone;
  String content;
  unsigned long addTime;
  bool ackSent;
  int retryCount;
};
RemoteSms remoteSmsQueue[MAX_REMOTE_SMS_QUEUE];

// ========== 已发送短信ID缓存（防重复）==========
#define MAX_SENT_ID_CACHE 50
String sentIdCache[MAX_SENT_ID_CACHE];
int sentIdCacheIndex = 0;

// ========== 已接收短信缓存 ==========
#define MAX_RECEIVED_SMS 20
struct ReceivedSms {
  bool valid;
  bool reported;
  String sender;
  String content;
  String timestamp;
};
ReceivedSms receivedSmsBuffer[MAX_RECEIVED_SMS];
int receivedSmsIndex = 0;

// ========== 轮询计时器 ==========
unsigned long lastPollTime = 0;
unsigned long lastReportTime = 0;
unsigned long lastHeartbeatTime = 0;

// ========== 保存/加载配置 ==========
void saveConfig() {
  preferences.begin("sms_cfg", false);
  preferences.putString("wifiSsid", config.wifiSsid);
  preferences.putString("wifiPass", config.wifiPass);
  preferences.putString("webUser", config.webUser);
  preferences.putString("webPass", config.webPass);
  preferences.putString("smtpServer", config.smtpServer);
  preferences.putInt("smtpPort", config.smtpPort);
  preferences.putString("smtpUser", config.smtpUser);
  preferences.putString("smtpPass", config.smtpPass);
  preferences.putString("smtpTo", config.smtpSendTo);
  preferences.putString("wxHook", config.wxWebhook);
  preferences.putBool("wxEn", config.wxEnabled);
  preferences.putBool("emailEn", config.emailEnabled);
  preferences.putString("remoteUrl", config.remoteUrl);
  preferences.putString("remoteKey", config.remoteKey);
  preferences.putBool("remoteEn", config.remoteEnabled);
  preferences.end();
  Serial.println("配置已保存");
}

void loadConfig() {
  preferences.begin("sms_cfg", true);
  config.wifiSsid   = preferences.getString("wifiSsid", DEFAULT_WIFI_SSID);
  config.wifiPass   = preferences.getString("wifiPass", DEFAULT_WIFI_PASS);
  config.webUser    = preferences.getString("webUser", DEFAULT_WEB_USER);
  config.webPass    = preferences.getString("webPass", DEFAULT_WEB_PASS);
  config.smtpServer = preferences.getString("smtpServer", DEFAULT_SMTP_SERVER);
  config.smtpPort   = preferences.getInt("smtpPort", DEFAULT_SMTP_PORT);
  config.smtpUser   = preferences.getString("smtpUser", DEFAULT_SMTP_USER);
  config.smtpPass   = preferences.getString("smtpPass", DEFAULT_SMTP_PASS);
  config.smtpSendTo = preferences.getString("smtpTo", DEFAULT_SMTP_TO);
  config.wxWebhook  = preferences.getString("wxHook", DEFAULT_WX_WEBHOOK);
  config.wxEnabled  = preferences.getBool("wxEn", true);
  config.emailEnabled = preferences.getBool("emailEn", true);
  config.remoteUrl  = preferences.getString("remoteUrl", DEFAULT_REMOTE_URL);
  config.remoteKey  = preferences.getString("remoteKey", DEFAULT_REMOTE_KEY);
  config.remoteEnabled = preferences.getBool("remoteEn", false);
  preferences.end();
  Serial.println("配置已加载");
}

// ========== 获取设备URL ==========
String getDeviceUrl() {
  if (inApMode) return "http://192.168.4.1/";
  return "http://" + WiFi.localIP().toString() + "/";
}

// ========== 时间戳格式化 ==========
String formatTimestamp(const char* pduTime) {
  String t = String(pduTime);
  if (t.length() < 12) return t;
  String year   = "20" + t.substring(0, 2);
  String month  = t.substring(2, 4);
  String day    = t.substring(4, 6);
  String hour   = t.substring(6, 8);
  String minute = t.substring(8, 10);
  String second = t.substring(10, 12);
  return year + "-" + month + "-" + day + " " + hour + ":" + minute + ":" + second;
}

String getCurrentTimestamp() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[32];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d", 
          t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
          t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

// ========== WiFi扫描结果 ==========
String wifiScanResult = "";
unsigned long lastScanTime = 0;

void scanWiFiNetworks() {
  Serial.println("开始扫描WiFi...");
  int n = WiFi.scanNetworks();
  wifiScanResult = "";
  if (n == 0) {
    wifiScanResult = "<div class='wifi-empty'>未找到WiFi网络</div>";
  } else {
    wifiScanResult = "<div class='wifi-list'>";
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      String encrypt = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "开放" : "加密";
      String signalClass = rssi > -50 ? "excellent" : (rssi > -70 ? "good" : "weak");
      String signalText = rssi > -50 ? "信号强" : (rssi > -70 ? "信号中" : "信号弱");

      wifiScanResult += "<div class='wifi-item' onclick=\"selectWiFi('" + ssid + "')\">";
      wifiScanResult += "<div class='wifi-info'>";
      wifiScanResult += "<div class='wifi-name'>" + ssid + "</div>";
      wifiScanResult += "<div class='wifi-meta'>" + encrypt + " | " + signalText + " (" + String(rssi) + "dBm)</div>";
      wifiScanResult += "</div>";
      wifiScanResult += "<div class='wifi-signal " + signalClass + "'>" + String(rssi) + "dBm</div>";
      wifiScanResult += "</div>";
    }
    wifiScanResult += "</div>";
  }
  lastScanTime = millis();
  WiFi.scanDelete();
  Serial.println("WiFi扫描完成，找到 " + String(n) + " 个网络");
}

// ========== 单页面HTML ==========
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>短信转发器</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;background:#f0f2f5;padding:16px;color:#333;}
.card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;box-shadow:0 1px 3px rgba(0,0,0,0.08);}
.card-title{font-size:15px;font-weight:600;color:#1a1a1a;margin-bottom:12px;display:flex;align-items:center;gap:6px;}
.status-bar{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:#fff;padding:16px;border-radius:12px;margin-bottom:12px;}
.status-bar h1{font-size:18px;margin-bottom:4px;}
.status-bar .ip{font-size:13px;opacity:0.9;}
.status-bar .mode{color:#ffe066;font-size:12px;margin-top:4px;}
.form-group{margin-bottom:12px;}
.form-group label{display:block;font-size:13px;color:#666;margin-bottom:4px;font-weight:500;}
.form-group input,.form-group textarea,.form-group select{width:100%;padding:10px 12px;border:1px solid #e0e0e0;border-radius:8px;font-size:14px;transition:border 0.2s;background:#fafafa;}
.form-group input:focus,.form-group textarea:focus,.form-group select:focus{outline:none;border-color:#667eea;background:#fff;}
.form-group textarea{resize:vertical;min-height:60px;font-family:inherit;}
.switch-row{display:flex;align-items:center;justify-content:space-between;padding:10px 0;border-bottom:1px solid #f0f0f0;}
.switch-row:last-child{border-bottom:none;}
.switch-row label{font-size:14px;color:#333;}
.switch{position:relative;display:inline-block;width:44px;height:24px;}
.switch input{opacity:0;width:0;height:0;}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;border-radius:24px;transition:0.3s;}
.slider:before{position:absolute;content:"";height:18px;width:18px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:0.3s;}
input:checked + .slider{background:#667eea;}
input:checked + .slider:before{transform:translateX(20px);}
.btn{display:block;width:100%;padding:12px;border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;transition:opacity 0.2s;}
.btn-primary{background:#667eea;color:#fff;}
.btn-primary:hover{opacity:0.9;}
.btn-success{background:#52c41a;color:#fff;margin-top:8px;}
.btn-success:hover{opacity:0.9;}
.btn-secondary{background:#e0e0e0;color:#333;}
.btn-secondary:hover{background:#d0d0d0;}
.hint{font-size:12px;color:#999;margin-top:4px;}
.divider{height:1px;background:#f0f0f0;margin:12px 0;}
.grid-2{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
.toast{position:fixed;top:16px;left:50%;transform:translateX(-50%) translateY(-100px);background:#333;color:#fff;padding:10px 20px;border-radius:20px;font-size:14px;opacity:0;transition:all 0.3s;z-index:1000;}
.toast.show{transform:translateX(-50%) translateY(0);opacity:1;}
.wifi-scan-btn{background:#2196F3;color:#fff;padding:8px 16px;border:none;border-radius:6px;font-size:13px;cursor:pointer;margin-bottom:10px;}
.wifi-scan-btn:hover{background:#1976D2;}
.wifi-scan-btn:disabled{background:#ccc;cursor:not-allowed;}
.wifi-list{max-height:250px;overflow-y:auto;border:1px solid #e0e0e0;border-radius:8px;}
.wifi-item{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;border-bottom:1px solid #f0f0f0;cursor:pointer;transition:background 0.2s;}
.wifi-item:last-child{border-bottom:none;}
.wifi-item:hover{background:#f5f5f5;}
.wifi-info{flex:1;}
.wifi-name{font-size:14px;font-weight:500;color:#333;}
.wifi-meta{font-size:12px;color:#999;margin-top:2px;}
.wifi-signal{font-size:12px;font-weight:600;padding:2px 8px;border-radius:4px;}
.wifi-signal.excellent{background:#e8f5e9;color:#2e7d32;}
.wifi-signal.good{background:#fff3e0;color:#e65100;}
.wifi-signal.weak{background:#ffebee;color:#c62828;}
.wifi-empty{padding:20px;text-align:center;color:#999;font-size:14px;}
.wifi-loading{padding:20px;text-align:center;color:#666;font-size:14px;}
.remote-status{padding:8px 12px;border-radius:6px;font-size:13px;margin-bottom:8px;}
.remote-status.online{background:#e8f5e9;color:#2e7d32;}
.remote-status.offline{background:#ffebee;color:#c62828;}
.remote-status.disabled{background:#f5f5f5;color:#999;}
.result-box{padding:10px;border-radius:6px;font-size:13px;margin-top:8px;display:none;}
.result-ok{background:#e8f5e9;color:#2e7d32;}
.result-err{background:#ffebee;color:#c62828;}
</style>
</head>
<body>
<div class="status-bar">
  <h1>📱 短信转发器</h1>
  <div class="ip">%IP%</div>
  <div class="mode">%MODE%</div>
</div>

<div class="card">
  <div class="card-title">🌐 远程控制状态</div>
  <div class="remote-status %REMOTE_STATUS_CLASS%">%REMOTE_STATUS%</div>
  <div class="hint">远程URL: %REMOTE_URL%</div>
  <div class="hint">轮询间隔: 15秒 | 上报间隔: 30秒</div>
</div>

<div class="card">
  <div class="card-title">📤 发送短信</div>
  <form id="smsForm">
    <div class="form-group">
      <label>目标号码</label>
      <input type="text" id="phone" placeholder="13800138000" required>
    </div>
    <div class="form-group">
      <label>短信内容</label>
      <textarea id="content" placeholder="请输入短信内容..." required></textarea>
    </div>
    <button type="submit" class="btn btn-success">📨 立即发送</button>
    <div class="result-box" id="smsResult"></div>
  </form>
</div>

<div class="card">
  <div class="card-title">🔔 推送开关</div>
  <div class="switch-row">
    <label>QQ邮箱推送</label>
    <label class="switch">
      <input type="checkbox" id="emailToggle" %EMAIL_CHECKED%>
      <span class="slider"></span>
    </label>
  </div>
  <div class="switch-row">
    <label>企业微信推送</label>
    <label class="switch">
      <input type="checkbox" id="wxToggle" %WX_CHECKED%>
      <span class="slider"></span>
    </label>
  </div>
  <div class="switch-row">
    <label>远程控制</label>
    <label class="switch">
      <input type="checkbox" id="remoteToggle" %REMOTE_CHECKED%>
      <span class="slider"></span>
    </label>
  </div>
  <div class="hint">开关状态修改后需点击底部保存按钮生效</div>
</div>

<div class="card">
  <div class="card-title">📧 QQ邮箱配置</div>
  <form id="cfgForm">
    <div class="grid-2">
      <div class="form-group">
        <label>SMTP服务器</label>
        <input type="text" name="smtpServer" value="%SMTP_SERVER%">
      </div>
      <div class="form-group">
        <label>端口</label>
        <input type="number" name="smtpPort" value="%SMTP_PORT%">
      </div>
    </div>
    <div class="form-group">
      <label>发件邮箱</label>
      <input type="text" name="smtpUser" value="%SMTP_USER%">
    </div>
    <div class="form-group">
      <label>授权码</label>
      <input type="password" name="smtpPass" value="%SMTP_PASS%">
    </div>
    <div class="form-group">
      <label>接收邮箱</label>
      <input type="text" name="smtpTo" value="%SMTP_TO%">
    </div>

    <div class="divider"></div>

    <div class="card-title">💬 企业微信配置</div>
    <div class="form-group">
      <label>Webhook地址</label>
      <input type="text" name="wxHook" value="%WX_HOOK%" placeholder="https://qyapi.weixin.qq.com/...">
    </div>

    <div class="divider"></div>

    <div class="card-title">🌐 远程服务器配置</div>
    <div class="form-group">
      <label>远程URL</label>
      <input type="text" name="remoteUrl" value="%REMOTE_URL%" placeholder="https://991123.xyz/sms/api.php">
    </div>
    <div class="form-group">
      <label>API密钥</label>
      <input type="password" name="remoteKey" value="%REMOTE_KEY%" placeholder="your-secret-key">
    </div>

    <div class="divider"></div>

    <div class="card-title">📶 WiFi设置</div>
    <div class="form-group">
      <label>WiFi名称</label>
      <input type="text" name="wifiSsid" id="wifiSsid" value="%WIFI_SSID%">
    </div>
    <div class="form-group">
      <label>WiFi密码</label>
      <input type="password" name="wifiPass" value="%WIFI_PASS%">
    </div>

    <div class="form-group">
      <button type="button" class="wifi-scan-btn" id="scanBtn" onclick="scanWiFi()">🔍 扫描附近WiFi</button>
      <div id="wifiList">%WIFI_LIST%</div>
    </div>

    <div class="hint">点击WiFi名称自动填入，修改后保存设备将自动重启</div>

    <div class="divider"></div>

    <div class="card-title">🔐 管理账号</div>
    <div class="grid-2">
      <div class="form-group">
        <label>账号</label>
        <input type="text" name="webUser" value="%WEB_USER%">
      </div>
      <div class="form-group">
        <label>密码</label>
        <input type="password" name="webPass" value="%WEB_PASS%">
      </div>
    </div>

    <button type="submit" class="btn btn-primary">💾 保存全部配置</button>
  </form>
</div>

<div class="toast" id="toast"></div>

<script>
function showToast(msg) {
  var t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  setTimeout(function(){t.classList.remove('show');}, 2000);
}

function selectWiFi(ssid) {
  document.getElementById('wifiSsid').value = ssid;
  showToast('✅ 已选择: ' + ssid);
}

function scanWiFi() {
  var btn = document.getElementById('scanBtn');
  var list = document.getElementById('wifiList');
  btn.disabled = true;
  btn.textContent = '扫描中...';
  list.innerHTML = '<div class="wifi-loading">正在扫描附近WiFi...</div>';

  fetch('/scanwifi')
    .then(r => r.text())
    .then(html => {
      list.innerHTML = html;
      btn.disabled = false;
      btn.textContent = '🔍 扫描附近WiFi';
    })
    .catch(err => {
      list.innerHTML = '<div class="wifi-empty">扫描失败，请重试</div>';
      btn.disabled = false;
      btn.textContent = '🔍 扫描附近WiFi';
    });
}

document.getElementById('smsForm').addEventListener('submit', function(e) {
  e.preventDefault();
  var phone = document.getElementById('phone').value.trim();
  var content = document.getElementById('content').value.trim();
  if (!phone || !content) return;

  var result = document.getElementById('smsResult');
  result.style.display = 'none';

  var btn = this.querySelector('button');
  var oldText = btn.textContent;
  btn.textContent = '发送中...';
  btn.disabled = true;

  var formData = new FormData();
  formData.append('phone', phone);
  formData.append('content', content);

  fetch('/sendsms', {method:'POST', body:formData})
    .then(r => r.text())
    .then(d => {
      btn.textContent = oldText;
      btn.disabled = false;
      result.style.display = 'block';
      if (d.indexOf('成功') >= 0) {
        result.className = 'result-box result-ok';
        result.innerHTML = '✅ 发送成功';
        document.getElementById('phone').value = '';
        document.getElementById('content').value = '';
      } else {
        result.className = 'result-box result-err';
        result.innerHTML = '❌ 发送失败';
      }
    })
    .catch(err => {
      btn.textContent = oldText;
      btn.disabled = false;
      result.style.display = 'block';
      result.className = 'result-box result-err';
      result.innerHTML = '❌ 请求失败';
    });
});

document.getElementById('cfgForm').addEventListener('submit', function(e) {
  e.preventDefault();
  var btn = this.querySelector('button[type="submit"]');
  var oldText = btn.textContent;
  btn.textContent = '保存中...';
  btn.disabled = true;

  var formData = new FormData(this);
  formData.append('emailEn', document.getElementById('emailToggle').checked ? 'on' : 'off');
  formData.append('wxEn', document.getElementById('wxToggle').checked ? 'on' : 'off');
  formData.append('remoteEn', document.getElementById('remoteToggle').checked ? 'on' : 'off');

  fetch('/save', {method:'POST', body:formData})
    .then(r => r.text())
    .then(d => {
      if (d === 'OK_RESTART') {
        showToast('✅ WiFi变更，设备重启中...');
        setTimeout(function(){location.reload();}, 3000);
      } else {
        btn.textContent = oldText;
        btn.disabled = false;
        showToast('✅ 保存成功');
        setTimeout(function(){location.reload();}, 1000);
      }
    })
    .catch(err => {
      btn.textContent = oldText;
      btn.disabled = false;
      showToast('❌ 保存失败');
    });
});
</script>
</body>
</html>
)rawliteral";

// ========== 认证检查 ==========
bool checkAuth() {
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "SMS", "请输入账号密码");
    return false;
  }
  return true;
}

// ========== 页面处理 ==========
void handleRoot() {
  if (!checkAuth()) return;

  if (wifiScanResult.length() == 0 || (millis() - lastScanTime > 30000)) {
    scanWiFiNetworks();
  }

  String html = String(htmlPage);
  html.replace("%IP%", inApMode ? "192.168.4.1" : WiFi.localIP().toString());
  html.replace("%MODE%", inApMode ? "配网模式 | 热点: SMS-AP / 12345678" : "正常模式 | 已连接: " + WiFi.SSID());
  html.replace("%WEB_USER%", config.webUser);
  html.replace("%WEB_PASS%", config.webPass);
  html.replace("%WIFI_SSID%", config.wifiSsid);
  html.replace("%WIFI_PASS%", config.wifiPass);
  html.replace("%SMTP_SERVER%", config.smtpServer);
  html.replace("%SMTP_PORT%", String(config.smtpPort));
  html.replace("%SMTP_USER%", config.smtpUser);
  html.replace("%SMTP_PASS%", config.smtpPass);
  html.replace("%SMTP_TO%", config.smtpSendTo);
  html.replace("%WX_HOOK%", config.wxWebhook);
  html.replace("%EMAIL_CHECKED%", config.emailEnabled ? "checked" : "");
  html.replace("%WX_CHECKED%", config.wxEnabled ? "checked" : "");

  html.replace("%REMOTE_URL%", config.remoteUrl);
  html.replace("%REMOTE_KEY%", config.remoteKey);
  html.replace("%REMOTE_CHECKED%", config.remoteEnabled ? "checked" : "");

  String remoteStatus, remoteStatusClass;
  if (!config.remoteEnabled) {
    remoteStatus = "远程控制已禁用";
    remoteStatusClass = "disabled";
  } else if (WiFi.status() == WL_CONNECTED) {
    remoteStatus = "远程控制已启用 - 最后轮询: " + getCurrentTimestamp();
    remoteStatusClass = "online";
  } else {
    remoteStatus = "远程控制已启用 - WiFi未连接";
    remoteStatusClass = "offline";
  }
  html.replace("%REMOTE_STATUS%", remoteStatus);
  html.replace("%REMOTE_STATUS_CLASS%", remoteStatusClass);

  html.replace("%WIFI_LIST%", wifiScanResult);
  server.send(200, "text/html", html);
}

void handleScanWiFi() {
  if (!checkAuth()) return;
  scanWiFiNetworks();
  server.send(200, "text/html", wifiScanResult);
}

// ========== AT命令 ==========
String sendAT(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        delay(50);
        while (Serial1.available()) resp += (char)Serial1.read();
        return resp;
      }
    }
  }
  return resp;
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  String r = sendAT(cmd, timeout);
  return r.indexOf("OK") >= 0;
}

// ========== 保存配置（智能重启）==========
void handleSave() {
  if (!checkAuth()) return;

  String oldWifiSsid = config.wifiSsid;
  String oldWifiPass = config.wifiPass;

  config.webUser = server.arg("webUser");
  config.webPass = server.arg("webPass");
  if (config.webUser.length() == 0) config.webUser = DEFAULT_WEB_USER;
  if (config.webPass.length() == 0) config.webPass = DEFAULT_WEB_PASS;

  config.wifiSsid = server.arg("wifiSsid");
  config.wifiPass = server.arg("wifiPass");
  if (config.wifiSsid.length() == 0) config.wifiSsid = DEFAULT_WIFI_SSID;
  if (config.wifiPass.length() == 0) config.wifiPass = DEFAULT_WIFI_PASS;

  config.smtpServer = server.arg("smtpServer");
  config.smtpPort = server.arg("smtpPort").toInt();
  if (config.smtpPort == 0) config.smtpPort = 465;
  config.smtpUser = server.arg("smtpUser");
  config.smtpPass = server.arg("smtpPass");
  config.smtpSendTo = server.arg("smtpTo");

  config.wxWebhook = server.arg("wxHook");

  config.remoteUrl = server.arg("remoteUrl");
  config.remoteKey = server.arg("remoteKey");
  if (config.remoteUrl.length() == 0) config.remoteUrl = DEFAULT_REMOTE_URL;
  if (config.remoteKey.length() == 0) config.remoteKey = DEFAULT_REMOTE_KEY;

  config.wxEnabled = server.arg("wxEn") == "on";
  config.emailEnabled = server.arg("emailEn") == "on";
  config.remoteEnabled = server.arg("remoteEn") == "on";

  saveConfig();

  bool wifiChanged = (oldWifiSsid != config.wifiSsid) || (oldWifiPass != config.wifiPass);

  if (wifiChanged) {
    server.send(200, "text/plain", "OK_RESTART");
    delay(500);
    ESP.restart();
  } else {
    server.send(200, "text/plain", "OK");
    if (config.remoteEnabled) {
      lastPollTime = 0;
      lastReportTime = 0;
      lastHeartbeatTime = 0;
    }
  }
}

// ========== 发送短信 ==========
bool sendSMS(const char* phone, const char* msg) {
  pdu.setSCAnumber();
  int pduLen = pdu.encodePDU(phone, msg);
  if (pduLen < 0) return false;

  String cmd = "AT+CMGS=" + String(pduLen);
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);

  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      if (c == '>') { gotPrompt = true; break; }
    }
  }
  if (!gotPrompt) return false;

  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);

  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) return true;
      if (resp.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

void handleSendSms() {
  if (!checkAuth()) return;
  String phone = server.arg("phone");
  String content = server.arg("content");
  bool ok = sendSMS(phone.c_str(), content.c_str());
  server.send(200, "text/plain", ok ? "发送成功" : "发送失败");
}

// ========== 推送功能 ==========
void sendEmail(const char* subject, const char* body) {
  if (!config.emailEnabled) {
    Serial.println("[邮箱] 已关闭，跳过");
    return;
  }
  if (config.smtpServer.length() == 0 || config.smtpUser.length() == 0 || 
      config.smtpPass.length() == 0 || config.smtpSendTo.length() == 0) {
    Serial.println("[邮箱] 配置不完整，跳过");
    return;
  }
  auto cb = [](SMTPStatus s){ Serial.println(s.text); };
  smtp.connect(config.smtpServer.c_str(), config.smtpPort, cb);
  if (smtp.isConnected()) {
    smtp.authenticate(config.smtpUser.c_str(), config.smtpPass.c_str(), readymail_auth_password);
    SMTPMessage msg;
    String from = "SMS <" + config.smtpUser + ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "User <" + config.smtpSendTo + ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    msg.timestamp = time(nullptr);
    smtp.send(msg);
    Serial.println("[邮箱] 已发送");
  } else {
    Serial.println("[邮箱] 服务器连接失败");
  }
}

void sendWechatWork(const char* sender, const char* message, const char* timestamp) {
  if (!config.wxEnabled) {
    Serial.println("[企业微信] 已关闭，跳过");
    return;
  }
  if (config.wxWebhook.length() == 0) {
    Serial.println("[企业微信] Webhook未配置");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[企业微信] WiFi未连接，跳过");
    return;
  }

  HTTPClient http;
  http.begin(config.wxWebhook);
  http.addHeader("Content-Type", "application/json");

  String text = String(message) + "\n";
  text += "号码: " + String(sender) + "\n";
  text += "时间: " + String(timestamp);

  String json = "{\"msgtype\":\"text\",\"text\":{\"content\":\"" + text + "\"}}";
  Serial.println("[企业微信] " + json);

  int code = http.POST(json);
  if (code > 0) {
    Serial.printf("[企业微信] 响应: %d\n", code);
    if (code == 200) Serial.println("[企业微信] 推送成功");
  } else {
    Serial.printf("[企业微信] 请求失败: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

void sendNotify(const char* sender, const char* message, const char* timestamp) {
  String fmtTime = formatTimestamp(timestamp);
  String subject = String(message);
  String body = "来自：" + String(sender) + "\n时间：" + fmtTime + "\n内容：" + String(message);
  sendEmail(subject.c_str(), body.c_str());
  sendWechatWork(sender, message, fmtTime.c_str());
}

// ========== 长短信处理 ==========
void initConcat() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
    }
  }
}

int findConcatSlot(int ref, const char* sender, int total) {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && concatBuffer[i].refNumber == ref &&
        concatBuffer[i].sender.equals(sender)) return i;
  }
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = ref;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = total;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }
  int oldest = 0;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < concatBuffer[oldest].firstPartTime) oldest = i;
  }
  concatBuffer[oldest].inUse = true;
  concatBuffer[oldest].refNumber = ref;
  concatBuffer[oldest].sender = String(sender);
  concatBuffer[oldest].totalParts = total;
  concatBuffer[oldest].receivedParts = 0;
  concatBuffer[oldest].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldest].parts[j].valid = false;
    concatBuffer[oldest].parts[j].text = "";
  }
  return oldest;
}

String assembleConcat(int slot) {
  String r = "";
  for (int i = 0; i < concatBuffer[slot].totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) r += concatBuffer[slot].parts[i].text;
    else r += "[缺失" + String(i+1) + "]";
  }
  return r;
}

void clearConcat(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS)) {
      Serial.println("长短信超时，强制转发");
      String fmtTime = formatTimestamp(concatBuffer[i].timestamp.c_str());
      sendNotify(concatBuffer[i].sender.c_str(), assembleConcat(i).c_str(), 
                 concatBuffer[i].timestamp.c_str());
      clearConcat(i);
    }
  }
}

// ========== 远程控制功能 ==========

bool isIdAlreadySent(const char* id) {
  for (int i = 0; i < MAX_SENT_ID_CACHE; i++) {
    if (sentIdCache[i] == String(id)) return true;
  }
  return false;
}

void recordSentId(const char* id) {
  sentIdCache[sentIdCacheIndex % MAX_SENT_ID_CACHE] = String(id);
  sentIdCacheIndex++;
}

// 前向声明
bool reportSmsResult(const char* id, bool success, const char* message);
void sendHeartbeat();
void sendAck(const char* id);

void initRemoteQueue() {
  for (int i = 0; i < MAX_REMOTE_SMS_QUEUE; i++) {
    remoteSmsQueue[i].valid = false;
    remoteSmsQueue[i].id = "";
    remoteSmsQueue[i].phone = "";
    remoteSmsQueue[i].content = "";
    remoteSmsQueue[i].ackSent = false;
    remoteSmsQueue[i].retryCount = 0;
  }
}

bool addRemoteSms(const char* id, const char* phone, const char* content) {
  if (isIdAlreadySent(id)) {
    Serial.printf("[远程] ID已发送过，跳过: %s\n", id);
    return false;
  }

  for (int i = 0; i < MAX_REMOTE_SMS_QUEUE; i++) {
    if (remoteSmsQueue[i].valid && remoteSmsQueue[i].id.equals(id)) {
      return false;
    }
  }

  for (int i = 0; i < MAX_REMOTE_SMS_QUEUE; i++) {
    if (!remoteSmsQueue[i].valid) {
      remoteSmsQueue[i].valid = true;
      remoteSmsQueue[i].id = String(id);
      remoteSmsQueue[i].phone = String(phone);
      remoteSmsQueue[i].content = String(content);
      remoteSmsQueue[i].addTime = millis();
      remoteSmsQueue[i].ackSent = false;
      remoteSmsQueue[i].retryCount = 0;
      Serial.printf("[远程] 添加待发短信 #%d: %s -> %s\n", i, phone, content);
      return true;
    }
  }

  int oldest = 0;
  for (int i = 1; i < MAX_REMOTE_SMS_QUEUE; i++) {
    if (remoteSmsQueue[i].addTime < remoteSmsQueue[oldest].addTime) oldest = i;
  }
  remoteSmsQueue[oldest].valid = true;
  remoteSmsQueue[oldest].id = String(id);
  remoteSmsQueue[oldest].phone = String(phone);
  remoteSmsQueue[oldest].content = String(content);
  remoteSmsQueue[oldest].addTime = millis();
  remoteSmsQueue[oldest].ackSent = false;
  remoteSmsQueue[oldest].retryCount = 0;
  Serial.printf("[远程] 队列满，替换 #%d: %s -> %s\n", oldest, phone, content);
  return true;
}

void markRemoteSmsSent(const char* id) {
  for (int i = 0; i < MAX_REMOTE_SMS_QUEUE; i++) {
    if (remoteSmsQueue[i].valid && remoteSmsQueue[i].id.equals(id)) {
      remoteSmsQueue[i].valid = false;
      Serial.printf("[远程] 标记已发送: %s\n", id);
      return;
    }
  }
}

void processRemoteQueue() {
  for (int i = 0; i < MAX_REMOTE_SMS_QUEUE; i++) {
    if (remoteSmsQueue[i].valid) {
      if (!remoteSmsQueue[i].ackSent) {
        sendAck(remoteSmsQueue[i].id.c_str());
        remoteSmsQueue[i].ackSent = true;
        delay(200);
      }

      Serial.printf("[远程] 正在发送 #%d: %s\n", i, remoteSmsQueue[i].phone.c_str());
      bool ok = sendSMS(remoteSmsQueue[i].phone.c_str(), remoteSmsQueue[i].content.c_str());

      if (ok) {
        Serial.println("[远程] 发送成功");
        recordSentId(remoteSmsQueue[i].id.c_str());

        bool reported = false;
        for (int retry = 0; retry < 3 && !reported; retry++) {
          if (reportSmsResult(remoteSmsQueue[i].id.c_str(), true, "发送成功")) {
            reported = true;
          } else {
            delay(500);
          }
        }

        markRemoteSmsSent(remoteSmsQueue[i].id.c_str());
      } else {
        Serial.println("[远程] 发送失败");
        remoteSmsQueue[i].retryCount++;
        if (remoteSmsQueue[i].retryCount >= 3) {
          recordSentId(remoteSmsQueue[i].id.c_str());
          reportSmsResult(remoteSmsQueue[i].id.c_str(), false, "发送失败，重试3次");
          markRemoteSmsSent(remoteSmsQueue[i].id.c_str());
        }
      }
      delay(2000);
    }
  }
}

void addReceivedSms(const char* sender, const char* content, const char* timestamp) {
  int idx = receivedSmsIndex % MAX_RECEIVED_SMS;
  receivedSmsBuffer[idx].valid = true;
  receivedSmsBuffer[idx].reported = false;
  receivedSmsBuffer[idx].sender = String(sender);
  receivedSmsBuffer[idx].content = String(content);
  receivedSmsBuffer[idx].timestamp = String(timestamp);
  receivedSmsIndex++;
  Serial.printf("[接收缓存] 添加 #%d: %s\n", idx, sender);
}

// ========== HTTP远程通信 ==========

String buildUrlWithKey() {
  String url = config.remoteUrl;
  if (url.indexOf("?") < 0) {
    url += "?key=" + config.remoteKey;
  } else if (url.indexOf("key=") < 0) {
    url += "&key=" + config.remoteKey;
  }
  return url;
}

void pollRemoteServer() {
  if (!config.remoteEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (config.remoteUrl.length() == 0) return;

  Serial.println("[远程] 开始轮询...");

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  http.begin(client, buildUrlWithKey());
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    Serial.println("[远程] 响应: " + payload);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      JsonArray smsList = doc["send"].as<JsonArray>();
      if (smsList) {
        for (JsonObject sms : smsList) {
          const char* id = sms["id"];
          const char* phone = sms["phone"];
          const char* content = sms["content"];
          if (id && phone && content) {
            addRemoteSms(id, phone, content);
          }
        }
      }

      const char* cmd = doc["cmd"];
      if (cmd && strcmp(cmd, "reboot") == 0) {
        Serial.println("[远程] 收到重启命令");
        ESP.restart();
      }
    } else {
      Serial.print("[远程] JSON解析失败: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.printf("[远程] 轮询失败: %d\n", code);
  }
  http.end();
}

void reportReceivedSms() {
  if (!config.remoteEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  bool hasUnreported = false;
  for (int i = 0; i < MAX_RECEIVED_SMS; i++) {
    if (receivedSmsBuffer[i].valid && !receivedSmsBuffer[i].reported) {
      hasUnreported = true;
      break;
    }
  }
  if (!hasUnreported) return;

  Serial.println("[远程] 开始上报接收短信...");

  JsonDocument doc;
  JsonArray smsArray = doc["received"].to<JsonArray>();

  for (int i = 0; i < MAX_RECEIVED_SMS; i++) {
    if (receivedSmsBuffer[i].valid && !receivedSmsBuffer[i].reported) {
      JsonObject sms = smsArray.add<JsonObject>();
      sms["sender"] = receivedSmsBuffer[i].sender;
      sms["content"] = receivedSmsBuffer[i].content;
      sms["timestamp"] = receivedSmsBuffer[i].timestamp;
      sms["device_ip"] = WiFi.localIP().toString();
    }
  }

  doc["device_id"] = WiFi.macAddress();
  doc["device_ip"] = WiFi.localIP().toString();
  doc["timestamp"] = getCurrentTimestamp();
  doc["unixtime"] = time(nullptr);
  doc["key"] = config.remoteKey;

  String jsonStr;
  serializeJson(doc, jsonStr);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  http.begin(client, buildUrlWithKey());
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  int code = http.POST(jsonStr);
  if (code == 200) {
    String resp = http.getString();
    Serial.println("[远程] 上报成功: " + resp);
    for (int i = 0; i < MAX_RECEIVED_SMS; i++) {
      if (receivedSmsBuffer[i].valid && !receivedSmsBuffer[i].reported) {
        receivedSmsBuffer[i].reported = true;
      }
    }
  } else {
    Serial.printf("[远程] 上报失败: %d\n", code);
  }
  http.end();
}

void sendAck(const char* id) {
  if (!config.remoteEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument doc;
  doc["type"] = "ack";
  doc["id"] = id;
  doc["device_id"] = WiFi.macAddress();
  doc["timestamp"] = getCurrentTimestamp();
  doc["unixtime"] = time(nullptr);
  doc["key"] = config.remoteKey;

  String jsonStr;
  serializeJson(doc, jsonStr);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  http.begin(client, buildUrlWithKey());
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int code = http.POST(jsonStr);
  Serial.printf("[远程] ACK [%s]: %d\n", id, code);
  http.end();
}

bool reportSmsResult(const char* id, bool success, const char* message) {
  if (!config.remoteEnabled) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  JsonDocument doc;
  doc["type"] = "result";
  doc["id"] = id;
  doc["success"] = success;
  doc["message"] = message;
  doc["device_id"] = WiFi.macAddress();
  doc["timestamp"] = getCurrentTimestamp();
  doc["unixtime"] = time(nullptr);
  doc["key"] = config.remoteKey;

  String jsonStr;
  serializeJson(doc, jsonStr);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  http.begin(client, buildUrlWithKey());
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  int code = http.POST(jsonStr);
  Serial.printf("[远程] 结果上报 [%s]: %d\n", id, code);
  http.end();

  return (code == 200);
}

void sendHeartbeat() {
  if (!config.remoteEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument doc;
  doc["type"] = "heartbeat";
  doc["device_id"] = WiFi.macAddress();
  doc["device_ip"] = WiFi.localIP().toString();
  doc["timestamp"] = getCurrentTimestamp();
  doc["unixtime"] = time(nullptr);
  doc["key"] = config.remoteKey;
  doc["rssi"] = WiFi.RSSI();
  doc["free_heap"] = ESP.getFreeHeap();

  String jsonStr;
  serializeJson(doc, jsonStr);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  http.begin(client, buildUrlWithKey());
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int code = http.POST(jsonStr);
  Serial.printf("[远程] 心跳: %d\n", code);
  http.end();
}

// ========== 短信处理 ==========
void processSms(const char* sender, const char* text, const char* timestamp) {
  String fmtTime = formatTimestamp(timestamp);
  Serial.println("=== 短信 ===");
  Serial.println("发送者: " + String(sender));
  Serial.println("时间: " + fmtTime);
  Serial.println("内容: " + String(text));

  addReceivedSms(sender, text, fmtTime.c_str());
  sendNotify(sender, text, timestamp);
}

String readSerialLine() {
  static char buf[SERIAL_BUFFER_SIZE];
  static int pos = 0;
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      buf[pos] = 0;
      String r = String(buf);
      pos = 0;
      return r;
    } else if (c != '\r') {
      if (pos < SERIAL_BUFFER_SIZE - 1) buf[pos++] = c;
      else pos = 0;
    }
  }
  return "";
}

bool isHex(const String& s) {
  if (s.length() == 0) return false;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

void checkURC() {
  static enum { IDLE, WAIT_PDU } state = IDLE;
  String line = readSerialLine();
  if (line.length() == 0) return;
  Serial.println("Debug> " + line);

  if (state == IDLE) {
    if (line.startsWith("+CMT:")) {
      Serial.println("检测到+CMT，等待PDU...");
      state = WAIT_PDU;
    }
  } else if (state == WAIT_PDU) {
    if (line.length() == 0) return;
    if (isHex(line)) {
      Serial.println("PDU: " + line);
      if (!pdu.decodePDU(line.c_str())) {
        Serial.println("PDU解析失败");
      } else {
        int* ci = pdu.getConcatInfo();
        int ref = ci[0], part = ci[1], total = ci[2];
        Serial.printf("长短信: ref=%d, part=%d, total=%d\n", ref, part, total);

        if (total > 1 && part > 0) {
          int slot = findConcatSlot(ref, pdu.getSender(), total);
          int idx = part - 1;
          if (idx >= 0 && idx < MAX_CONCAT_PARTS && !concatBuffer[slot].parts[idx].valid) {
            concatBuffer[slot].parts[idx].valid = true;
            concatBuffer[slot].parts[idx].text = String(pdu.getText());
            concatBuffer[slot].receivedParts++;
            if (concatBuffer[slot].receivedParts == 1) concatBuffer[slot].timestamp = String(pdu.getTimeStamp());
            Serial.printf("缓存分段 %d/%d\n", part, total);
          }
          if (concatBuffer[slot].receivedParts >= total) {
            Serial.println("长短信收齐，合并转发");
            processSms(concatBuffer[slot].sender.c_str(), assembleConcat(slot).c_str(),
                       concatBuffer[slot].timestamp.c_str());
            clearConcat(slot);
          }
        } else {
          processSms(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
        }
      }
      state = IDLE;
    } else {
      state = IDLE;
    }
  }
}

// ========== 模组控制 ==========
void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(1200);
  digitalWrite(MODEM_EN_PIN, HIGH);
  delay(6000);
}

bool waitCEREG() {
  String r = sendAT("AT+CEREG?", 2000);
  return r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0;
}

void blink() {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
}

// ========== WiFi连接 ==========
bool connectWiFi() {
  Serial.println("连接WiFi: " + config.wifiSsid);

  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_STA);
  delay(100);

  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
  Serial.println("WiFi开始连接...");

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 60) {
    blink();
    retry++;
    if (retry % 10 == 0) {
      Serial.println("WiFi连接中... 已尝试 " + String(retry) + " 次");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi已连接: " + WiFi.localIP().toString());
    Serial.println("信号强度: " + String(WiFi.RSSI()) + " dBm");
    return true;
  }

  Serial.println("WiFi连接失败，状态码: " + String(WiFi.status()));
  return false;
}

// ========== 启动配网热点 ==========
void startAP() {
  Serial.println("启动配网热点: " + String(AP_SSID));

  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_AP);
  delay(100);

  WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 4);

  IPAddress IP(192, 168, 4, 1);
  IPAddress NMask(255, 255, 255, 0);
  WiFi.softAPConfig(IP, IP, NMask);

  inApMode = true;
  Serial.println("热点IP: " + WiFi.softAPIP().toString());
  Serial.println("热点SSID: " + String(AP_SSID));
  Serial.println("热点密码: " + String(AP_PASS));
  Serial.println("热点信道: 6");
}

// ========== Setup & Loop ==========
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.begin(115200);
  delay(1500);

  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);

  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();

  initConcat();
  initRemoteQueue();
  loadConfig();

  while (!sendATandWaitOK("AT", 1000)) { Serial.println("AT无响应，重试..."); blink(); }
  Serial.println("模组AT正常");
  while (!sendATandWaitOK("AT+CGACT=0,1", 5000)) { Serial.println("CGACT失败，重试..."); blink(); }
  Serial.println("已禁用数据连接");
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)) { Serial.println("CNMI失败，重试..."); blink(); }
  Serial.println("CNMI设置完成");
  while (!sendATandWaitOK("AT+CMGF=0", 1000)) { Serial.println("PDU模式失败，重试..."); blink(); }
  Serial.println("PDU模式设置完成");
  while (!waitCEREG()) { Serial.println("等待网络注册..."); blink(); }
  Serial.println("网络已注册");

  if (!connectWiFi()) {
    startAP();
  }

  // ===== 修复时区：设置为北京时间 CST-8 =====
  setenv("TZ", "CST-8", 1);
  tzset();
  configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");

  int ntpRetry = 0;
  while (time(nullptr) < 100000 && ntpRetry < 100) { delay(100); ntpRetry++; }
  if (time(nullptr) >= 100000) {
    Serial.println("NTP同步成功: " + getCurrentTimestamp());
  } else {
    Serial.println("NTP同步失败");
  }

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/scanwifi", handleScanWiFi);
  server.begin();
  Serial.println("HTTP服务器已启动");

  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, LOW);

  String subject = "短信转发器已启动";
  String body = "设备已启动\nIP: " + getDeviceUrl();
  sendEmail(subject.c_str(), body.c_str());
  sendWechatWork("系统", body.c_str(), "启动时");
}

void loop() {
  server.handleClient();
  checkConcatTimeout();
  if (Serial.available()) Serial1.write(Serial.read());
  checkURC();

  if (config.remoteEnabled && WiFi.status() == WL_CONNECTED) {
    unsigned long now = millis();

    if (now - lastPollTime >= REMOTE_POLL_INTERVAL_MS) {
      lastPollTime = now;
      pollRemoteServer();
    }

    processRemoteQueue();

    if (now - lastReportTime >= REMOTE_REPORT_INTERVAL_MS) {
      lastReportTime = now;
      reportReceivedSms();
    }

    if (now - lastHeartbeatTime >= REMOTE_HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatTime = now;
      sendHeartbeat();
    }
  }
}