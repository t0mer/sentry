// switchbot_log_reader.ino — who locked/unlocked a SwitchBot lock.
//
// The cloud knows *when* the lock changed; only the lock's on-device log knows
// *who*. This firmware watches for a state change (cloud poll, webhook, or a
// plain BLE poll), then reads the identity out of the lock's log over BLE and
// republishes the merged event over MQTT and HTTP. See CLAUDE.md for the spec.

#include <WiFi.h>
#include <time.h>
#include <vector>

#include "cloud.h"
#include "config.h"
#include "mqtt.h"
#include "provisioning.h"
#include "sb_lock_client.h"
#include "sb_proto.h"
#include "users.h"
#include "webui.h"

static const uint8_t BOOT_BUTTON_PIN = 0;
static const uint32_t BUTTON_HOLD_MS = 5000;
static const uint32_t WIFI_TIMEOUT_MS = 60000;
static const uint8_t MAX_ENTRIES_PER_FETCH = 10;
static const uint32_t WEBHOOK_RETRY_MS = 300000;   // 5 min
static const uint32_t WEBHOOK_FALLBACK_POLL_MS = 60000;
static const uint32_t BLE_RETRY_DELAY_MS = 3000;   // §7: the log may not be flushed yet

static Config g_cfg;
static bool g_provisioning = false;

static uint32_t g_lastCloudPollMs = 0;
static uint32_t g_lastBlePollMs = 0;
static uint32_t g_lastWebhookSetupMs = 0;
static bool g_webhookRegistered = false;
static String g_lastLockState;
static uint32_t g_buttonDownMs = 0;

// ---------------------------------------------------------------- helpers

static String buildEventJson(const SbLogEntry &e) {
  String user = userLookup(e.source, e.value);
  String json = "{";
  json += "\"ts\":" + String(e.ts);
  json += ",\"index\":" + String(e.index);
  json += ",\"source\":" + String(e.source);
  json += ",\"source_name\":\"" + String(sbSourceName(e.source)) + "\"";
  json += ",\"action\":" + String(e.action);
  json += ",\"action_name\":\"" + String(sbActionName(e.action)) + "\"";
  json += ",\"value\":" + String(e.value);
  json += ",\"payload\":\"" + bytesToHex(e.payload, e.payloadLen) + "\"";
  json += ",\"user\":";
  if (user.isEmpty()) {
    json += "null";
  } else {
    json += "\"";
    for (unsigned i = 0; i < user.length(); i++) {
      char c = user[i];
      if (c == '"' || c == '\\') json += '\\';
      if ((uint8_t)c >= 0x20) json += c;
    }
    json += "\"";
  }
  json += "}";
  return json;
}

// Reads the lock's log and publishes everything newer than the stored
// timestamp. Returns true when at least one new entry was emitted.
static bool fetchAndPublish(const char *reason) {
  uint32_t baseTime = configLastSeenTs();
  std::vector<SbLogEntry> entries;
  String err;

  Serial.printf("[fetch] %s (base_time=%lu)\n", reason, (unsigned long)baseTime);
  bool ok = sbLockFetchLog(g_cfg, baseTime, MAX_ENTRIES_PER_FETCH, entries, err);

  // A cloud "when" is authoritative: if the entry is missing the log simply has
  // not flushed yet, so retry once (§7). A bare BLE poll has nothing to wait
  // for, so it never doubles up its BLE traffic.
  bool triggeredByEvent = strcmp(reason, "ble-poll") != 0;
  if (triggeredByEvent && (!ok || entries.empty())) {
    delay(BLE_RETRY_DELAY_MS);
    entries.clear();
    ok = sbLockFetchLog(g_cfg, baseTime, MAX_ENTRIES_PER_FETCH, entries, err);
  }
  if (!ok) {
    Serial.printf("[fetch] failed: %s\n", err.c_str());
    webuiSetNote("Last BLE fetch failed: " + err);
    return false;
  }

  uint32_t newest = baseTime;
  size_t published = 0;
  for (size_t i = 0; i < entries.size(); i++) {
    const SbLogEntry &e = entries[i];
    if (e.ts <= baseTime) continue;  // already emitted before a reboot

    String json = buildEventJson(e);
    Serial.printf("[event] %s\n", json.c_str());
    webuiPushEvent(e, json);
    if (mqttEnabled() && !mqttPublish(json)) {
      Serial.printf("[mqtt] publish failed: %s\n", mqttLastError().c_str());
    }
    if (e.ts > newest) newest = e.ts;
    published++;
  }

  if (newest > baseTime) configSetLastSeenTs(newest);
  if (published == 0) {
    webuiSetNote("Last fetch returned no new entries.");
  } else {
    webuiSetNote("");
  }
  return published > 0;
}

static bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPass.c_str());

  Serial.printf("[wifi] joining %s", g_cfg.wifiSsid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] failed");
    return false;
  }
  Serial.printf("[wifi] ip %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// TLS and the Open API signature both need a real clock.
static void syncTime(bool required) {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  uint32_t limit = required ? 20000 : 5000;
  while (time(nullptr) < 1700000000 && millis() - start < limit) delay(200);
  Serial.printf("[time] %lu\n", (unsigned long)time(nullptr));
}

static void startProvisioning() {
  g_provisioning = true;
  provisioningStart(g_cfg);
}

// Hold BOOT/GPIO0 for ~5 s to wipe the stored config and reboot into setup.
static void handleResetButton() {
  bool pressed = digitalRead(BOOT_BUTTON_PIN) == LOW;
  if (!pressed) {
    g_buttonDownMs = 0;
    return;
  }
  if (g_buttonDownMs == 0) {
    g_buttonDownMs = millis();
    return;
  }
  if (millis() - g_buttonDownMs >= BUTTON_HOLD_MS) {
    Serial.println("[button] held: erasing config and rebooting into setup");
    configClear();
    delay(200);
    ESP.restart();
  }
}

// ---------------------------------------------------------------- setup

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SwitchBot Lock Log Reader ===");

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  configLoad(g_cfg);
  usersBegin();

  // BOOT held through power-up is also a config reset.
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    Serial.println("[button] held at boot, waiting 5s...");
    uint32_t start = millis();
    while (digitalRead(BOOT_BUTTON_PIN) == LOW && millis() - start < BUTTON_HOLD_MS) delay(50);
    if (millis() - start >= BUTTON_HOLD_MS) {
      Serial.println("[button] config erased");
      configClear();
      g_cfg = Config();
    }
  }

  if (!g_cfg.complete() || configProvisioningRequested()) {
    Serial.println(g_cfg.complete() ? "[boot] reconfigure requested" : "[boot] config incomplete");
    startProvisioning();
    return;
  }

  if (!connectWifi()) {
    Serial.println("[boot] falling back to provisioning so WiFi can be corrected");
    startProvisioning();
    return;
  }

  // Belt and braces: the provisioning form always sets one, but never leave the
  // station UI reachable without a login.
  if (g_cfg.uiPass.isEmpty()) {
    g_cfg.uiPass = randomToken(10);
    configSave(g_cfg);
    Serial.printf("[boot] generated web ui login: %s / %s\n", g_cfg.uiUser.c_str(),
                  g_cfg.uiPass.c_str());
  }

  syncTime(g_cfg.usesCloud());
  sbLockClientBegin();
  mqttBegin(g_cfg);
  webuiBegin(g_cfg, [](const char *reason) { return fetchAndPublish(reason); });

  Serial.printf("[boot] mode=%s ui=http://%s/\n", g_cfg.triggerMode.c_str(),
                WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------- loop

static void loopCloudPoll(uint32_t intervalMs) {
  if (millis() - g_lastCloudPollMs < intervalMs) return;
  g_lastCloudPollMs = millis();

  LockStatus status;
  String err;
  if (!cloudGetStatus(g_cfg, status, err)) {
    Serial.printf("[cloud] %s\n", err.c_str());
    webuiSetNote("Cloud poll failed: " + err);
    return;
  }
  webuiSetLockState(status.lockState, status.doorState);

  if (g_lastLockState.isEmpty()) {
    g_lastLockState = status.lockState;  // first sample is the baseline, not an event
    return;
  }
  if (status.lockState != g_lastLockState) {
    Serial.printf("[cloud] %s -> %s\n", g_lastLockState.c_str(), status.lockState.c_str());
    g_lastLockState = status.lockState;
    fetchAndPublish("cloud-poll");
  }
}

static void loopWebhookSetup() {
  if (g_webhookRegistered) return;
  if (g_lastWebhookSetupMs != 0 && millis() - g_lastWebhookSetupMs < WEBHOOK_RETRY_MS) return;
  g_lastWebhookSetupMs = millis();

  String err;
  if (cloudSetupWebhook(g_cfg, err)) {
    g_webhookRegistered = true;
    Serial.println("[cloud] webhook registered");
  } else {
    Serial.printf("[cloud] setupWebhook: %s\n", err.c_str());
    webuiSetNote("Webhook registration failed: " + err);
  }
}

static void loopBlePoll() {
  uint32_t intervalMs = g_cfg.blePollIntervalS * 1000UL;
  if (g_lastBlePollMs != 0 && millis() - g_lastBlePollMs < intervalMs) return;
  g_lastBlePollMs = millis();
  fetchAndPublish("ble-poll");
}

void loop() {
  if (g_provisioning) {
    provisioningLoop();
    handleResetButton();
    delay(2);
    return;
  }

  handleResetButton();
  webuiLoop();
  mqttLoop();

  if (WiFi.status() != WL_CONNECTED) {
    delay(200);
    return;  // WiFi.setAutoReconnect handles the rejoin
  }

  if (g_cfg.triggerMode == TRIGGER_CLOUD_POLL) {
    loopCloudPoll(g_cfg.pollIntervalS * 1000UL);
  } else if (g_cfg.triggerMode == TRIGGER_WEBHOOK) {
    loopWebhookSetup();
    // Fallback poll, so a dropped webhook does not mean a missed event.
    loopCloudPoll(WEBHOOK_FALLBACK_POLL_MS);
  } else {
    loopBlePoll();
  }

  delay(5);
}
