// config.h — persisted device configuration (NVS / Preferences).
#pragma once

#include <Arduino.h>

static const size_t SB_ENC_KEY_LEN = 16;

// Trigger modes (see CLAUDE.md §1).
static const char TRIGGER_CLOUD_POLL[] = "cloud-poll";
static const char TRIGGER_WEBHOOK[] = "webhook";
static const char TRIGGER_BLE_POLL[] = "ble-poll";

struct Config {
  // WiFi
  String wifiSsid;
  String wifiPass;

  // Lock (BLE)
  String lockMac;  // normalized "AA:BB:CC:DD:EE:FF"
  uint8_t keyId = 0;
  uint8_t encKey[SB_ENC_KEY_LEN] = {0};
  bool encKeySet = false;

  // Mode
  String triggerMode = TRIGGER_CLOUD_POLL;

  // Cloud (Open API v1.1)
  String apiToken;
  String apiSecret;
  String deviceId;
  uint32_t pollIntervalS = 5;

  // Webhook mode
  String publicUrl;

  // MQTT
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPass;
  bool mqttTls = false;
  String mqttTopic = "switchbot/lock/log";

  // BLE poll mode
  uint32_t blePollIntervalS = 60;

  // Web UI login. The password is generated on first save when left blank, so
  // the station-mode UI is never reachable unauthenticated.
  String uiUser = "admin";
  String uiPass;

  bool usesCloud() const { return triggerMode != TRIGGER_BLE_POLL; }
  bool usesWebhook() const { return triggerMode == TRIGGER_WEBHOOK; }

  // True when every field required for the selected mode is present.
  bool complete() const;
};

// Load config from NVS. Returns false when nothing has been stored yet.
bool configLoad(Config &cfg);
bool configSave(const Config &cfg);
void configClear();

// SoftAP password for provisioning: random per device, created on first use and
// kept in NVS. Printed on the serial console when the portal starts.
String configApPassword();

// Random alphanumeric token, used for generated passwords.
String randomToken(size_t len);

// Set by the web UI's "Reconfigure" button: boot into provisioning once more
// without wiping the stored config.
void configRequestProvisioning(bool requested);
bool configProvisioningRequested();

// Newest log-entry timestamp already emitted, so reboots don't re-publish.
uint32_t configLastSeenTs();
void configSetLastSeenTs(uint32_t ts);

// Helpers shared with the provisioning form and the BLE client.
bool hexToBytes(const String &hex, uint8_t *out, size_t outLen);
String bytesToHex(const uint8_t *in, size_t len);
bool normalizeMac(const String &in, String &out);
bool isValidTriggerMode(const String &mode);
