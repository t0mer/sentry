#include "webui.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

#include "mqtt.h"
#include "sb_lock_client.h"
#include "users.h"

static const size_t MAX_RECENT = 20;

static WebServer g_server(80);
static Config g_cfg;
static WebUiFetchFn g_fetch;
static bool g_begun = false;

static SbLogEntry g_recent[MAX_RECENT];
static String g_recentJson[MAX_RECENT];
static size_t g_recentCount = 0;  // number of valid slots
static size_t g_recentHead = 0;   // next write position

static String g_lockState;
static String g_doorState;
static String g_note;

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

static String fmtTime(uint32_t ts) {
  if (ts == 0) return String("-");
  time_t t = (time_t)ts;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%SZ", &tmv);
  return String(buf);
}

// Iterate newest-first over the ring buffer.
static void forEachRecent(const std::function<void(const SbLogEntry &, const String &)> &fn) {
  for (size_t i = 0; i < g_recentCount; i++) {
    size_t idx = (g_recentHead + MAX_RECENT - 1 - i) % MAX_RECENT;
    fn(g_recent[idx], g_recentJson[idx]);
  }
}

static String pageHead(const char *title) {
  String s = F("<!doctype html><html><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>");
  s += title;
  s += F("</title><style>:root{color-scheme:light dark}"
         "body{font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;padding:20px;"
         "max-width:820px;margin-inline:auto}h1{font-size:20px;margin:0 0 12px}"
         "h2{font-size:16px;margin:24px 0 8px}table{border-collapse:collapse;width:100%;font-size:13px}"
         "th,td{text-align:left;padding:6px 8px;border-bottom:1px solid #8883}"
         "th{opacity:.7;font-weight:600}dl{display:grid;grid-template-columns:auto 1fr;gap:4px 16px;"
         "font-size:14px;margin:0}dt{opacity:.7}dd{margin:0}"
         "button,.btn{padding:8px 14px;border:1px solid #8886;border-radius:6px;background:transparent;"
         "color:inherit;font:inherit;cursor:pointer;text-decoration:none;display:inline-block}"
         "button.p{background:#2d6cdf;color:#fff;border-color:#2d6cdf}"
         "button.d{border-color:#d33;color:#d33}"
         "input{padding:6px;border:1px solid #8886;border-radius:5px;background:transparent;color:inherit;font:inherit}"
         "form.inline{display:inline}.note{opacity:.7;font-size:13px}</style></head><body>");
  return s;
}

static String statusPage() {
  String s = pageHead("SwitchBot Log Reader");
  s += F("<h1>SwitchBot Log Reader</h1><dl>");
  s += "<dt>Mode</dt><dd>" + htmlEscape(g_cfg.triggerMode) + "</dd>";
  s += "<dt>WiFi</dt><dd>" + htmlEscape(WiFi.SSID()) + " &middot; " + WiFi.localIP().toString() +
       " (" + String(WiFi.RSSI()) + " dBm)</dd>";
  s += "<dt>Lock</dt><dd>" + htmlEscape(g_cfg.lockMac) + "</dd>";
  s += "<dt>Lock state</dt><dd>" + htmlEscape(g_lockState.isEmpty() ? String("unknown") : g_lockState) +
       (g_doorState.isEmpty() ? String("") : " / " + htmlEscape(g_doorState)) + "</dd>";
  s += "<dt>Cloud</dt><dd>" +
       String(g_cfg.usesCloud() ? (g_cfg.deviceId.isEmpty() ? "not configured" : "configured") : "not used") +
       "</dd>";
  s += "<dt>MQTT</dt><dd>";
  if (g_cfg.mqttHost.isEmpty()) {
    s += "not configured";
  } else {
    s += htmlEscape(g_cfg.mqttHost) + ":" + String(g_cfg.mqttPort) + " &rarr; " +
         htmlEscape(g_cfg.mqttTopic) + " &middot; " + (mqttConnected() ? "connected" : "disconnected");
    String me = mqttLastError();
    if (!me.isEmpty()) s += " (" + htmlEscape(me) + ")";
  }
  s += "</dd>";
  s += "<dt>Last BLE fetch</dt><dd>";
  uint32_t fetchMs = sbLockLastFetchMillis();
  s += fetchMs == 0 ? String("never") : String((millis() - fetchMs) / 1000) + "s ago";
  String be = sbLockLastError();
  if (!be.isEmpty()) s += " &middot; error: " + htmlEscape(be);
  s += "</dd>";
  s += "<dt>Newest entry</dt><dd>" + fmtTime(configLastSeenTs()) + "</dd>";
  s += "<dt>Web UI login</dt><dd>" + htmlEscape(g_cfg.uiUser) + " (password stored on device)</dd>";
  s += "<dt>Uptime</dt><dd>" + String(millis() / 1000) + "s &middot; heap " + String(ESP.getFreeHeap()) +
       " B</dd>";
  s += F("</dl>");

  if (!g_note.isEmpty()) s += "<p class=\"note\">" + htmlEscape(g_note) + "</p>";

  s += F("<h2>Recent events</h2>");
  if (g_recentCount == 0) {
    s += F("<p class=\"note\">Nothing yet.</p>");
  } else {
    s += F("<table><tr><th>Time</th><th>Action</th><th>Source</th><th>Credential</th><th>Who</th></tr>");
    forEachRecent([&s](const SbLogEntry &e, const String &json) {
      (void)json;
      String who = userLookup(e.source, e.value);
      s += "<tr><td>" + fmtTime(e.ts) + "</td><td>" + sbActionName(e.action) + " (" + String(e.action) +
           ")</td><td>" + sbSourceName(e.source) + " (" + String(e.source) + ")</td><td>" +
           String(e.value) + "</td><td>" + (who.isEmpty() ? String("&mdash;") : htmlEscape(who)) +
           "</td></tr>";
    });
    s += F("</table>");
  }

  s += F("<h2>Actions</h2>"
         "<form class=\"inline\" method=\"POST\" action=\"/fetch-now\">"
         "<button class=\"p\" type=\"submit\">Fetch now</button></form> "
         "<a class=\"btn\" href=\"/users\">Learn users</a> "
         "<a class=\"btn\" href=\"/logs\">/logs JSON</a> "
         "<form class=\"inline\" method=\"POST\" action=\"/reconfigure\">"
         "<button type=\"submit\">Reconfigure</button></form> "
         "<form class=\"inline\" method=\"POST\" action=\"/factory-reset\" "
         "onsubmit=\"return confirm('Erase all settings and reboot into setup?')\">"
         "<button class=\"d\" type=\"submit\">Factory reset</button></form>"
         "<p class=\"note\">Secrets are stored on the device and are never shown here. "
         "The credential id is a number the lock reports; name it on the Learn users page.</p>"
         "</body></html>");
  return s;
}

static String usersPage() {
  String s = pageHead("Learn users");
  s += F("<h1>Learn users</h1>"
         "<p class=\"note\">Unlock with one known credential, find its row below and give it a name. "
         "The lock only reports a numeric id &mdash; there is no name and no face image.</p>"
         "<h2>Recent credentials</h2>");

  if (g_recentCount == 0) {
    s += F("<p class=\"note\">No events captured yet.</p>");
  } else {
    s += F("<table><tr><th>Time</th><th>Source</th><th>Action</th><th>Value</th><th>Raw payload</th>"
           "<th>Name</th></tr>");
    forEachRecent([&s](const SbLogEntry &e, const String &json) {
      (void)json;
      String who = userLookup(e.source, e.value);
      s += "<tr><td>" + fmtTime(e.ts) + "</td><td>" + sbSourceName(e.source) + " (" + String(e.source) +
           ")</td><td>" + sbActionName(e.action) + " (" + String(e.action) + ")</td><td>" +
           String(e.value) + "</td><td><code>" + bytesToHex(e.payload, e.payloadLen) + "</code></td>";
      s += "<td><form class=\"inline\" method=\"POST\" action=\"/users/set\">"
           "<input type=\"hidden\" name=\"source\" value=\"" + String(e.source) + "\">"
           "<input type=\"hidden\" name=\"value\" value=\"" + String(e.value) + "\">"
           "<input name=\"name\" maxlength=\"24\" value=\"" + htmlEscape(who) + "\">"
           "<button type=\"submit\">Save</button></form></td></tr>";
    });
    s += F("</table>");
  }

  s += F("<h2>Named credentials</h2>");
  if (usersCount() == 0) {
    s += F("<p class=\"note\">None yet.</p>");
  } else {
    s += F("<table><tr><th>Source</th><th>Value</th><th>Name</th><th></th></tr>");
    usersForEach([&s](uint8_t source, uint8_t value, const String &name) {
      s += "<tr><td>" + String(sbSourceName(source)) + " (" + String(source) + ")</td><td>" +
           String(value) + "</td><td>" + htmlEscape(name) + "</td>";
      s += "<td><form class=\"inline\" method=\"POST\" action=\"/users/delete\">"
           "<input type=\"hidden\" name=\"source\" value=\"" + String(source) + "\">"
           "<input type=\"hidden\" name=\"value\" value=\"" + String(value) + "\">"
           "<button class=\"d\" type=\"submit\">Delete</button></form></td></tr>";
    });
    s += F("</table>");
  }

  s += F("<p><a class=\"btn\" href=\"/\">Back</a></p></body></html>");
  return s;
}

// Every station-mode route except the cloud webhook sits behind digest auth.
// The password is set (or generated) during provisioning, so the UI — which can
// wipe the config and expose the lock's history — is never open on the LAN.
static bool requireAuth() {
  if (g_cfg.uiPass.isEmpty()) return true;  // only if provisioning somehow left it blank
  if (g_server.authenticate(g_cfg.uiUser.c_str(), g_cfg.uiPass.c_str())) return true;
  g_server.requestAuthentication(DIGEST_AUTH, "SwitchBot Log Reader", "Authentication required");
  return false;
}

static void redirectTo(const char *path) {
  g_server.sendHeader("Location", path, true);
  g_server.send(303, "text/plain", "");
}

static void handleRoot() {
  if (!requireAuth()) return;
  g_server.send(200, "text/html; charset=utf-8", statusPage());
}

// The stored strings are already the exact JSON documents that went out over
// MQTT, so the array is assembled textually rather than re-parsed.
static void handleLogs() {
  if (!requireAuth()) return;
  String out = "[";
  bool first = true;
  forEachRecent([&out, &first](const SbLogEntry &e, const String &json) {
    (void)e;
    if (json.isEmpty()) return;
    if (!first) out += ",";
    out += json;
    first = false;
  });
  out += "]";
  g_server.send(200, "application/json", out);
}

static void handleUsers() {
  if (!requireAuth()) return;
  g_server.send(200, "text/html; charset=utf-8", usersPage());
}

static void handleUserSet() {
  if (!requireAuth()) return;
  if (!g_server.hasArg("source") || !g_server.hasArg("value")) {
    g_server.send(400, "text/plain", "source and value required");
    return;
  }
  uint8_t source = (uint8_t)g_server.arg("source").toInt();
  uint8_t value = (uint8_t)g_server.arg("value").toInt();
  String name = g_server.arg("name");
  name.trim();

  if (name.isEmpty()) {
    userRemove(source, value);
  } else if (!userSet(source, value, name)) {
    g_server.send(507, "text/plain", "user table is full");
    return;
  }
  redirectTo("/users");
}

static void handleUserDelete() {
  if (!requireAuth()) return;
  uint8_t source = (uint8_t)g_server.arg("source").toInt();
  uint8_t value = (uint8_t)g_server.arg("value").toInt();
  userRemove(source, value);
  redirectTo("/users");
}

static void handleFetchNow() {
  if (!requireAuth()) return;
  if (g_fetch) g_fetch("manual");
  redirectTo("/");
}

// POST /switchbot-webhook — the Open API changeReport sink (CLAUDE.md §6).
static void handleWebhook() {
  String body = g_server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  const char *eventType = doc["eventType"] | "";
  if (strcmp(eventType, "changeReport") != 0) {
    g_server.send(200, "application/json", "{\"ok\":true,\"ignored\":true}");
    return;
  }

  // Match on deviceMac, not deviceType: the type string differs per lock model.
  String mac = doc["context"]["deviceMac"] | "";
  String normalized;
  if (!normalizeMac(mac, normalized) || !normalized.equalsIgnoreCase(g_cfg.lockMac)) {
    g_server.send(200, "application/json", "{\"ok\":true,\"ignored\":true}");
    return;
  }

  String lockState = doc["context"]["lockState"] | "";
  String doorState = doc["context"]["doorState"] | "";
  webuiSetLockState(lockState, doorState);

  g_server.send(200, "application/json", "{\"ok\":true}");
  if (g_fetch) g_fetch("webhook");
}

static void handleReconfigure() {
  if (!requireAuth()) return;
  configRequestProvisioning(true);
  g_server.send(200, "text/html; charset=utf-8",
                "<!doctype html><meta charset=\"utf-8\"><h2>Rebooting into setup.</h2>"
                "<p>Join the SB-LogReader AP to reconfigure. Settings are kept until you save.</p>");
  delay(800);
  ESP.restart();
}

static void handleFactoryReset() {
  if (!requireAuth()) return;
  configClear();
  g_server.send(200, "text/html; charset=utf-8",
                "<!doctype html><meta charset=\"utf-8\"><h2>Settings erased.</h2>"
                "<p>Rebooting into setup.</p>");
  delay(800);
  ESP.restart();
}

void webuiBegin(const Config &cfg, WebUiFetchFn fetchFn) {
  g_cfg = cfg;
  g_fetch = fetchFn;

  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/logs", HTTP_GET, handleLogs);
  g_server.on("/users", HTTP_GET, handleUsers);
  g_server.on("/users/set", HTTP_POST, handleUserSet);
  g_server.on("/users/delete", HTTP_POST, handleUserDelete);
  g_server.on("/fetch-now", HTTP_POST, handleFetchNow);
  g_server.on("/switchbot-webhook", HTTP_POST, handleWebhook);
  g_server.on("/reconfigure", HTTP_POST, handleReconfigure);
  g_server.on("/factory-reset", HTTP_POST, handleFactoryReset);
  g_server.begin();
  g_begun = true;
}

void webuiLoop() {
  if (!g_begun) return;
  g_server.handleClient();
}

void webuiPushEvent(const SbLogEntry &entry, const String &json) {
  g_recent[g_recentHead] = entry;
  g_recentJson[g_recentHead] = json;
  g_recentHead = (g_recentHead + 1) % MAX_RECENT;
  if (g_recentCount < MAX_RECENT) g_recentCount++;
}

void webuiSetLockState(const String &lockState, const String &doorState) {
  g_lockState = lockState;
  g_doorState = doorState;
}

void webuiSetNote(const String &note) { g_note = note; }
