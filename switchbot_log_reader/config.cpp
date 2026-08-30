#include "config.h"

#include <Preferences.h>
#include <esp_random.h>

static const char NVS_NS[] = "sbcfg";

bool hexToBytes(const String &hex, uint8_t *out, size_t outLen) {
  if (hex.length() != outLen * 2) return false;
  for (size_t i = 0; i < outLen; i++) {
    uint8_t v = 0;
    for (size_t j = 0; j < 2; j++) {
      char c = hex[i * 2 + j];
      uint8_t nib;
      if (c >= '0' && c <= '9') nib = c - '0';
      else if (c >= 'a' && c <= 'f') nib = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') nib = c - 'A' + 10;
      else return false;
      v = (v << 4) | nib;
    }
    out[i] = v;
  }
  return true;
}

String bytesToHex(const uint8_t *in, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  String s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    s += kHex[in[i] >> 4];
    s += kHex[in[i] & 0x0F];
  }
  return s;
}

// Accepts "AABBCCDDEEFF", "aa:bb:cc:dd:ee:ff" and "aa-bb-...". Emits upper-case
// colon form, which is what NimBLEAddress::toString() compares against.
bool normalizeMac(const String &in, String &out) {
  String hex;
  hex.reserve(12);
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == ':' || c == '-' || c == ' ') continue;
    if (!isHexadecimalDigit(c)) return false;
    hex += c;
  }
  if (hex.length() != 12) return false;
  uint8_t raw[6];
  if (!hexToBytes(hex, raw, sizeof(raw))) return false;
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
  out = buf;
  return true;
}

bool isValidTriggerMode(const String &mode) {
  return mode == TRIGGER_CLOUD_POLL || mode == TRIGGER_WEBHOOK || mode == TRIGGER_BLE_POLL;
}

bool Config::complete() const {
  if (wifiSsid.isEmpty()) return false;
  String tmp;
  if (!normalizeMac(lockMac, tmp)) return false;
  if (!encKeySet) return false;
  if (!isValidTriggerMode(triggerMode)) return false;
  if (usesCloud()) {
    if (apiToken.isEmpty() || apiSecret.isEmpty() || deviceId.isEmpty()) return false;
  }
  return true;
}

bool configLoad(Config &cfg) {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;

  cfg.wifiSsid = p.getString("ssid", "");
  cfg.wifiPass = p.getString("pass", "");
  cfg.lockMac = p.getString("mac", "");
  cfg.keyId = p.getUChar("keyid", 0);

  size_t n = p.getBytesLength("enckey");
  if (n == SB_ENC_KEY_LEN) {
    p.getBytes("enckey", cfg.encKey, SB_ENC_KEY_LEN);
    cfg.encKeySet = true;
  }

  cfg.triggerMode = p.getString("mode", TRIGGER_CLOUD_POLL);
  cfg.apiToken = p.getString("token", "");
  cfg.apiSecret = p.getString("secret", "");
  cfg.deviceId = p.getString("devid", "");
  cfg.pollIntervalS = p.getUInt("poll", 5);
  cfg.publicUrl = p.getString("purl", "");

  cfg.mqttHost = p.getString("mhost", "");
  cfg.mqttPort = p.getUShort("mport", 1883);
  cfg.mqttUser = p.getString("muser", "");
  cfg.mqttPass = p.getString("mpass", "");
  cfg.mqttTls = p.getBool("mtls", false);
  cfg.mqttTopic = p.getString("mtopic", "switchbot/lock/log");

  cfg.blePollIntervalS = p.getUInt("blepoll", 60);

  cfg.uiUser = p.getString("uiuser", "admin");
  cfg.uiPass = p.getString("uipass", "");

  bool stored = p.isKey("ssid");
  p.end();
  return stored;
}

bool configSave(const Config &cfg) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return false;

  p.putString("ssid", cfg.wifiSsid);
  p.putString("pass", cfg.wifiPass);
  p.putString("mac", cfg.lockMac);
  p.putUChar("keyid", cfg.keyId);
  if (cfg.encKeySet) p.putBytes("enckey", cfg.encKey, SB_ENC_KEY_LEN);

  p.putString("mode", cfg.triggerMode);
  p.putString("token", cfg.apiToken);
  p.putString("secret", cfg.apiSecret);
  p.putString("devid", cfg.deviceId);
  p.putUInt("poll", cfg.pollIntervalS);
  p.putString("purl", cfg.publicUrl);

  p.putString("mhost", cfg.mqttHost);
  p.putUShort("mport", cfg.mqttPort);
  p.putString("muser", cfg.mqttUser);
  p.putString("mpass", cfg.mqttPass);
  p.putBool("mtls", cfg.mqttTls);
  p.putString("mtopic", cfg.mqttTopic);

  p.putUInt("blepoll", cfg.blePollIntervalS);

  p.putString("uiuser", cfg.uiUser);
  p.putString("uipass", cfg.uiPass);

  p.end();
  return true;
}

void configClear() {
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.clear();
    p.end();
  }
}

String randomToken(size_t len) {
  static const char kAlphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";  // no look-alikes
  String out;
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    out += kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
  }
  return out;
}

String configApPassword() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return String("lockreader");
  String pass = p.getString("appass", "");
  if (pass.length() < 8) {
    pass = randomToken(12);
    p.putString("appass", pass);
  }
  p.end();
  return pass;
}

void configRequestProvisioning(bool requested) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putBool("provreq", requested);
  p.end();
}

bool configProvisioningRequested() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;
  bool req = p.getBool("provreq", false);
  p.end();
  return req;
}

uint32_t configLastSeenTs() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return 0;
  uint32_t ts = p.getUInt("lastts", 0);
  p.end();
  return ts;
}

void configSetLastSeenTs(uint32_t ts) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUInt("lastts", ts);
  p.end();
}
