#include "mqtt.h"

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "cloud.h"  // cloudAttachCaBundle

static WiFiClient g_plain;
static WiFiClientSecure g_tls;
static PubSubClient g_mqtt;

static Config g_cfg;
static bool g_begun = false;
static String g_clientId;
static String g_statusTopic;
static String g_lastError;

static uint32_t g_nextAttemptMs = 0;
static uint32_t g_backoffMs = 1000;  // 1s → 20s cap, per the MQTT guidelines

static const uint32_t BACKOFF_MIN_MS = 1000;
static const uint32_t BACKOFF_MAX_MS = 20000;

bool mqttEnabled() { return g_begun && !g_cfg.mqttHost.isEmpty(); }

void mqttBegin(const Config &cfg) {
  g_cfg = cfg;
  g_begun = true;
  if (g_cfg.mqttHost.isEmpty()) return;

  // One connection per client id: derive it from the chip MAC so it is unique
  // per install and stable across reboots and settings changes.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  g_clientId = "sb-logreader-" + bytesToHex(mac, sizeof(mac));
  g_statusTopic = g_cfg.mqttTopic + "/status";

  if (g_cfg.mqttTls) {
    cloudAttachCaBundle(g_tls);
    g_mqtt.setClient(g_tls);
  } else {
    g_mqtt.setClient(g_plain);
  }
  g_mqtt.setServer(g_cfg.mqttHost.c_str(), g_cfg.mqttPort);
  g_mqtt.setBufferSize(768);
  g_mqtt.setKeepAlive(45);
  g_nextAttemptMs = 0;
}

static bool mqttConnect() {
  const char *user = g_cfg.mqttUser.isEmpty() ? nullptr : g_cfg.mqttUser.c_str();
  const char *pass = g_cfg.mqttPass.isEmpty() ? nullptr : g_cfg.mqttPass.c_str();

  // LWT: retained "0" on the status topic, re-published as "1" once connected.
  bool ok = g_mqtt.connect(g_clientId.c_str(), user, pass, g_statusTopic.c_str(), 1, true, "0");
  if (ok) {
    g_mqtt.publish(g_statusTopic.c_str(), "1", true);
    g_lastError = "";
    g_backoffMs = BACKOFF_MIN_MS;
  } else {
    g_lastError = "connect failed (state " + String(g_mqtt.state()) + ")";
    g_backoffMs = min(g_backoffMs * 2, BACKOFF_MAX_MS);
  }
  return ok;
}

void mqttLoop() {
  if (!mqttEnabled()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!g_mqtt.connected()) {
    uint32_t now = millis();
    if ((int32_t)(now - g_nextAttemptMs) < 0) return;
    g_nextAttemptMs = now + g_backoffMs;
    mqttConnect();
    return;
  }
  g_mqtt.loop();
}

bool mqttConnected() { return mqttEnabled() && g_mqtt.connected(); }

bool mqttPublish(const String &json) {
  if (!mqttEnabled()) return false;
  if (!g_mqtt.connected() && !mqttConnect()) return false;
  bool ok = g_mqtt.publish(g_cfg.mqttTopic.c_str(), json.c_str(), false);
  if (!ok) g_lastError = "publish failed";
  return ok;
}

String mqttLastError() { return g_lastError; }
