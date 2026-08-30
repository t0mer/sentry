#include "provisioning.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

static const byte DNS_PORT = 53;

static WebServer g_server(80);
static DNSServer g_dns;
static Config g_existing;
static bool g_active = false;
static String g_apSsid;

static const char FORM_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SwitchBot Log Reader setup</title>
<style>
 :root{color-scheme:light dark}
 body{font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;padding:20px;
      max-width:640px;margin-inline:auto}
 h1{font-size:20px;margin:0 0 4px} p.sub{margin:0 0 20px;opacity:.7}
 fieldset{border:1px solid #8884;border-radius:8px;margin:0 0 16px;padding:12px 14px}
 legend{padding:0 6px;font-weight:600}
 label{display:block;margin:10px 0 4px;font-size:13px;opacity:.85}
 input,select{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;
      border:1px solid #8886;background:transparent;color:inherit;font:inherit}
 .row{display:flex;gap:10px}.row>div{flex:1}
 .chk{display:flex;align-items:center;gap:8px;margin-top:12px}
 .chk input{width:auto}
 button{margin-top:8px;padding:10px 18px;border:0;border-radius:6px;background:#2d6cdf;
      color:#fff;font:inherit;font-weight:600;cursor:pointer}
 .err{background:#d332;border:1px solid #d336;padding:10px 12px;border-radius:6px;margin-bottom:16px}
 .hint{font-size:12px;opacity:.65;margin:4px 0 0}
</style></head><body>
<h1>SwitchBot Log Reader</h1>
<p class="sub">Device: {{AP}}</p>
{{ERR}}
<form method="POST" action="/save">
 <fieldset><legend>WiFi</legend>
  <label>SSID *</label><input name="wifi_ssid" value="{{SSID}}" required maxlength="32">
  <label>Password</label><input name="wifi_pass" type="password" placeholder="{{PASS_SET}}" maxlength="63">
 </fieldset>

 <fieldset><legend>Lock (BLE)</legend>
  <label>Lock MAC *</label>
  <input name="lock_mac" value="{{MAC}}" placeholder="AA:BB:CC:DD:EE:FF" required>
  <div class="row">
   <div><label>Key ID * (2 hex)</label><input name="key_id" value="{{KEYID}}" maxlength="2"></div>
   <div><label>Encryption key * (32 hex)</label>
    <input name="enc_key" type="password" placeholder="{{ENC_SET}}" maxlength="32"></div>
  </div>
  <p class="hint">Get both with: python -m switchbot.scripts.get_encryption_key &lt;MAC&gt; &lt;email&gt;</p>
 </fieldset>

 <fieldset><legend>Trigger mode</legend>
  <select name="trigger_mode" id="mode">
   <option value="cloud-poll" {{SEL_CLOUD}}>cloud-poll &mdash; poll the SwitchBot API (default)</option>
   <option value="webhook" {{SEL_HOOK}}>webhook &mdash; API pushes to this device</option>
   <option value="ble-poll" {{SEL_BLE}}>ble-poll &mdash; offline, no cloud account</option>
  </select>
 </fieldset>

 <fieldset id="cloud"><legend>SwitchBot Open API</legend>
  <label>Token *</label><input name="api_token" type="password" placeholder="{{TOKEN_SET}}">
  <label>Secret *</label><input name="api_secret" type="password" placeholder="{{SECRET_SET}}">
  <label>Device ID * (the lock)</label><input name="device_id" value="{{DEVID}}">
  <label>Poll interval (s)</label><input name="poll_interval_s" type="number" min="1" max="3600" value="{{POLL}}">
 </fieldset>

 <fieldset id="hook"><legend>Webhook</legend>
  <label>Public URL of this device</label>
  <input name="public_url" value="{{PURL}}" placeholder="https://lock.example.com/switchbot-webhook">
  <p class="hint">Must reach POST /switchbot-webhook on this device (e.g. via cloudflared).</p>
 </fieldset>

 <fieldset id="ble"><legend>BLE polling</legend>
  <label>BLE poll interval (s)</label>
  <input name="ble_poll_interval_s" type="number" min="10" max="3600" value="{{BLEPOLL}}">
 </fieldset>

 <fieldset><legend>Web UI login</legend>
  <label>Username</label><input name="ui_user" value="{{UIUSER}}" maxlength="24">
  <label>Password</label><input name="ui_pass" type="password" placeholder="{{UIPASS_SET}}" maxlength="32">
  <p class="hint">Protects the device page on your LAN. Leave blank and one is generated for you
   and shown once after saving.</p>
 </fieldset>

 <fieldset><legend>MQTT (optional)</legend>
  <div class="row">
   <div><label>Host</label><input name="mqtt_host" value="{{MHOST}}"></div>
   <div><label>Port</label><input name="mqtt_port" type="number" min="1" max="65535" value="{{MPORT}}"></div>
  </div>
  <div class="row">
   <div><label>Username</label><input name="mqtt_user" value="{{MUSER}}"></div>
   <div><label>Password</label><input name="mqtt_pass" type="password" placeholder="{{MPASS_SET}}"></div>
  </div>
  <label>Topic</label><input name="mqtt_topic" value="{{MTOPIC}}">
  <div class="chk"><input type="checkbox" name="mqtt_tls" value="1" id="tls" {{MTLS}}>
   <label for="tls" style="margin:0">Use TLS (port 8883)</label></div>
 </fieldset>

 <button type="submit">Save &amp; reboot</button>
</form>
<script>
 var mode=document.getElementById('mode');
 function sync(){
   var m=mode.value;
   document.getElementById('cloud').style.display=(m==='ble-poll')?'none':'';
   document.getElementById('hook').style.display=(m==='webhook')?'':'none';
   document.getElementById('ble').style.display=(m==='ble-poll')?'':'none';
 }
 mode.addEventListener('change',sync);sync();
 document.getElementById('tls').addEventListener('change',function(){
   var p=document.getElementsByName('mqtt_port')[0];
   if(!p.value||p.value==='1883'||p.value==='8883') p.value=this.checked?'8883':'1883';
 });
</script>
</body></html>)HTML";

static String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// Secrets are never rendered back — the form only says whether one is stored.
static const char *setMarker(bool isSet, const char *emptyHint) {
  return isSet ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2 set - leave blank to keep" : emptyHint;
}

static String renderForm(const String &error) {
  String page = FPSTR(FORM_HTML);
  char keyIdBuf[3];
  snprintf(keyIdBuf, sizeof(keyIdBuf), "%02x", g_existing.keyId);

  page.replace("{{AP}}", htmlEscape(g_apSsid));
  page.replace("{{ERR}}", error.isEmpty() ? String("") : "<div class=\"err\">" + htmlEscape(error) + "</div>");
  page.replace("{{SSID}}", htmlEscape(g_existing.wifiSsid));
  page.replace("{{PASS_SET}}", setMarker(!g_existing.wifiPass.isEmpty(), "WiFi password"));
  page.replace("{{MAC}}", htmlEscape(g_existing.lockMac));
  page.replace("{{KEYID}}", g_existing.encKeySet ? String(keyIdBuf) : String(""));
  page.replace("{{ENC_SET}}", setMarker(g_existing.encKeySet, "32 hex characters"));
  page.replace("{{SEL_CLOUD}}", g_existing.triggerMode == TRIGGER_CLOUD_POLL ? "selected" : "");
  page.replace("{{SEL_HOOK}}", g_existing.triggerMode == TRIGGER_WEBHOOK ? "selected" : "");
  page.replace("{{SEL_BLE}}", g_existing.triggerMode == TRIGGER_BLE_POLL ? "selected" : "");
  page.replace("{{TOKEN_SET}}", setMarker(!g_existing.apiToken.isEmpty(), "Open API token"));
  page.replace("{{SECRET_SET}}", setMarker(!g_existing.apiSecret.isEmpty(), "Open API secret"));
  page.replace("{{DEVID}}", htmlEscape(g_existing.deviceId));
  page.replace("{{POLL}}", String(g_existing.pollIntervalS));
  page.replace("{{PURL}}", htmlEscape(g_existing.publicUrl));
  page.replace("{{BLEPOLL}}", String(g_existing.blePollIntervalS));
  page.replace("{{MHOST}}", htmlEscape(g_existing.mqttHost));
  page.replace("{{MPORT}}", String(g_existing.mqttPort));
  page.replace("{{MUSER}}", htmlEscape(g_existing.mqttUser));
  page.replace("{{MPASS_SET}}", setMarker(!g_existing.mqttPass.isEmpty(), "MQTT password"));
  page.replace("{{MTOPIC}}", htmlEscape(g_existing.mqttTopic));
  page.replace("{{MTLS}}", g_existing.mqttTls ? "checked" : "");
  page.replace("{{UIUSER}}", htmlEscape(g_existing.uiUser));
  page.replace("{{UIPASS_SET}}", setMarker(!g_existing.uiPass.isEmpty(), "generated if left blank"));
  return page;
}

// Reads a field from either a JSON body or a form-encoded POST.
struct FormArgs {
  JsonDocument json;
  bool isJson = false;

  void parse() {
    if (!g_server.hasArg("plain")) return;
    String body = g_server.arg("plain");
    if (body.isEmpty() || body[0] != '{') return;
    if (!deserializeJson(json, body)) isJson = true;
  }
  bool has(const char *name) const {
    if (isJson) return !json[name].isNull();
    return g_server.hasArg(name);
  }
  String get(const char *name, const String &fallback = String("")) const {
    if (isJson) return json[name].isNull() ? fallback : json[name].as<String>();
    return g_server.hasArg(name) ? g_server.arg(name) : fallback;
  }
  bool getBool(const char *name) const {
    if (isJson) return json[name].as<bool>();
    if (!g_server.hasArg(name)) return false;
    String v = g_server.arg(name);
    return v == "1" || v == "on" || v == "true";
  }
};

static void handleRoot() { g_server.send(200, "text/html; charset=utf-8", renderForm("")); }

static void handleNotFound() {
  // Captive-portal probe: bounce everything to the form.
  g_server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  g_server.send(302, "text/plain", "");
}

static void handleSave() {
  FormArgs args;
  args.parse();

  Config cfg = g_existing;  // start from stored values so blank secrets are kept
  String err;

  cfg.wifiSsid = args.get("wifi_ssid", cfg.wifiSsid);
  cfg.wifiSsid.trim();
  if (!args.get("wifi_pass").isEmpty()) cfg.wifiPass = args.get("wifi_pass");

  String macIn = args.get("lock_mac", cfg.lockMac);
  macIn.trim();
  String mac;
  if (!normalizeMac(macIn, mac)) {
    err = "Lock MAC must look like AA:BB:CC:DD:EE:FF.";
  } else {
    cfg.lockMac = mac;
  }

  String keyIdIn = args.get("key_id");
  keyIdIn.trim();
  if (!keyIdIn.isEmpty()) {
    uint8_t kid[1];
    if (!hexToBytes(keyIdIn, kid, 1)) {
      if (err.isEmpty()) err = "Key ID must be exactly 2 hex characters.";
    } else {
      cfg.keyId = kid[0];
    }
  }

  String encIn = args.get("enc_key");
  encIn.trim();
  if (!encIn.isEmpty()) {
    uint8_t key[SB_ENC_KEY_LEN];
    if (!hexToBytes(encIn, key, SB_ENC_KEY_LEN)) {
      if (err.isEmpty()) err = "Encryption key must be exactly 32 hex characters.";
    } else {
      memcpy(cfg.encKey, key, SB_ENC_KEY_LEN);
      cfg.encKeySet = true;
    }
  }

  String mode = args.get("trigger_mode", cfg.triggerMode);
  mode.trim();
  if (!isValidTriggerMode(mode)) {
    if (err.isEmpty()) err = "Unknown trigger mode.";
  } else {
    cfg.triggerMode = mode;
  }

  if (!args.get("api_token").isEmpty()) cfg.apiToken = args.get("api_token");
  if (!args.get("api_secret").isEmpty()) cfg.apiSecret = args.get("api_secret");
  cfg.apiToken.trim();
  cfg.apiSecret.trim();
  cfg.deviceId = args.get("device_id", cfg.deviceId);
  cfg.deviceId.trim();

  long poll = args.get("poll_interval_s", String(cfg.pollIntervalS)).toInt();
  cfg.pollIntervalS = (poll >= 1 && poll <= 3600) ? (uint32_t)poll : 5;

  cfg.publicUrl = args.get("public_url", cfg.publicUrl);
  cfg.publicUrl.trim();

  long blePoll = args.get("ble_poll_interval_s", String(cfg.blePollIntervalS)).toInt();
  cfg.blePollIntervalS = (blePoll >= 10 && blePoll <= 3600) ? (uint32_t)blePoll : 60;

  cfg.mqttHost = args.get("mqtt_host", cfg.mqttHost);
  cfg.mqttHost.trim();
  long port = args.get("mqtt_port", String(cfg.mqttPort)).toInt();
  cfg.mqttPort = (port >= 1 && port <= 65535) ? (uint16_t)port : 1883;
  cfg.mqttUser = args.get("mqtt_user", cfg.mqttUser);
  cfg.mqttUser.trim();
  if (!args.get("mqtt_pass").isEmpty()) cfg.mqttPass = args.get("mqtt_pass");
  cfg.mqttTls = args.getBool("mqtt_tls");
  cfg.uiUser = args.get("ui_user", cfg.uiUser);
  cfg.uiUser.trim();
  if (cfg.uiUser.isEmpty()) cfg.uiUser = "admin";
  if (!args.get("ui_pass").isEmpty()) cfg.uiPass = args.get("ui_pass");

  String topic = args.get("mqtt_topic", cfg.mqttTopic);
  topic.trim();
  if (!topic.isEmpty()) cfg.mqttTopic = topic;

  if (err.isEmpty()) {
    if (cfg.wifiSsid.isEmpty()) {
      err = "WiFi SSID is required.";
    } else if (!cfg.encKeySet) {
      err = "The 32-hex encryption key is required.";
    } else if (cfg.usesCloud() &&
               (cfg.apiToken.isEmpty() || cfg.apiSecret.isEmpty() || cfg.deviceId.isEmpty())) {
      err = "Cloud modes need an Open API token, secret and device id.";
    } else if (cfg.usesWebhook() && cfg.publicUrl.isEmpty()) {
      err = "Webhook mode needs the public URL of this device.";
    }
  }

  if (!err.isEmpty()) {
    g_existing = cfg;  // keep what the user typed; secrets stay masked
    g_server.send(400, "text/html; charset=utf-8", renderForm(err));
    return;
  }

  // Never leave the station-mode UI open: mint a password when none was given.
  bool generated = false;
  if (cfg.uiPass.isEmpty()) {
    cfg.uiPass = randomToken(10);
    generated = true;
  }

  configSave(cfg);
  configRequestProvisioning(false);

  String done = "<!doctype html><meta charset=\"utf-8\"><h2>Saved.</h2><p>Rebooting and joining <b>" +
                htmlEscape(cfg.wifiSsid) + "</b>. Watch the serial console for the device IP.</p>";
  if (generated) {
    done += "<p><b>Web UI login</b><br>user: <code>" + htmlEscape(cfg.uiUser) +
            "</code><br>password: <code>" + htmlEscape(cfg.uiPass) +
            "</code><br>Write this down &mdash; it is shown only once.</p>";
    Serial.printf("[prov] web ui login: %s / %s\n", cfg.uiUser.c_str(), cfg.uiPass.c_str());
  }
  g_server.send(200, "text/html; charset=utf-8", done);
  delay(1200);
  ESP.restart();
}

void provisioningStart(const Config &existing) {
  g_existing = existing;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "SB-LogReader-%02X%02X", mac[4], mac[5]);
  g_apSsid = ssid;

  String apPass = configApPassword();  // random per device, kept in NVS

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_apSsid.c_str(), apPass.c_str());
  delay(200);

  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(DNS_PORT, "*", WiFi.softAPIP());

  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/save", HTTP_POST, handleSave);
  g_server.onNotFound(handleNotFound);
  g_server.begin();

  g_active = true;

  Serial.println();
  Serial.printf("[prov] AP   : %s\n", g_apSsid.c_str());
  Serial.printf("[prov] pass : %s\n", apPass.c_str());
  Serial.printf("[prov] open : http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void provisioningLoop() {
  if (!g_active) return;
  g_dns.processNextRequest();
  g_server.handleClient();
}

bool provisioningActive() { return g_active; }
String provisioningApSsid() { return g_apSsid; }
