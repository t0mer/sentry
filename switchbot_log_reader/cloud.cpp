#include "cloud.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <time.h>

static const char API_BASE[] = "https://api.switch-bot.com/v1.1";

// The ESP-IDF certificate bundle, linked into the image by the ESP32 core.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

// setCACertBundle() gained a size argument in newer ESP32 cores. Pick whichever
// overload this core actually has, so the sketch builds on both.
template <typename T>
static auto attachBundle(T &client, int)
    -> decltype(client.setCACertBundle(rootca_crt_bundle_start, (size_t)0), void()) {
  client.setCACertBundle(rootca_crt_bundle_start,
                         (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start));
}

template <typename T>
static auto attachBundle(T &client, long)
    -> decltype(client.setCACertBundle(rootca_crt_bundle_start), void()) {
  client.setCACertBundle(rootca_crt_bundle_start);
}

void cloudAttachCaBundle(WiFiClientSecure &client) { attachBundle(client, 0); }

static String randomNonce() {
  char buf[33];
  for (int i = 0; i < 4; i++) {
    snprintf(buf + i * 8, 9, "%08x", (unsigned)esp_random());
  }
  buf[32] = '\0';
  return String(buf);
}

// sign = base64( HMAC-SHA256(secret, token + t + nonce) ), as used by the
// official Open API v1.1 samples.
static bool signRequest(const Config &cfg, String &t, String &nonce, String &sign) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // SNTP has not synced yet; TLS + sign would fail

  char tbuf[24];
  snprintf(tbuf, sizeof(tbuf), "%llu", (unsigned long long)now * 1000ULL);
  t = tbuf;
  nonce = randomNonce();

  String data = cfg.apiToken + t + nonce;

  uint8_t digest[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) return false;
  if (mbedtls_md_hmac(info, (const uint8_t *)cfg.apiSecret.c_str(), cfg.apiSecret.length(),
                      (const uint8_t *)data.c_str(), data.length(), digest) != 0) {
    return false;
  }

  unsigned char b64[64];
  size_t b64Len = 0;
  if (mbedtls_base64_encode(b64, sizeof(b64), &b64Len, digest, sizeof(digest)) != 0) return false;
  b64[b64Len] = '\0';
  sign = String((const char *)b64);
  return true;
}

static bool addSignedHeaders(HTTPClient &http, const Config &cfg, String &err) {
  String t, nonce, sign;
  if (!signRequest(cfg, t, nonce, sign)) {
    err = "cannot sign request (clock not synced?)";
    return false;
  }
  http.addHeader("Authorization", cfg.apiToken);
  http.addHeader("sign", sign);
  http.addHeader("t", t);
  http.addHeader("nonce", nonce);
  http.addHeader("Content-Type", "application/json");
  return true;
}

bool cloudGetStatus(const Config &cfg, LockStatus &status, String &err) {
  if (cfg.deviceId.isEmpty()) {
    err = "no device id configured";
    return false;
  }

  WiFiClientSecure client;
  cloudAttachCaBundle(client);
  HTTPClient http;
  http.setTimeout(8000);

  String url = String(API_BASE) + "/devices/" + cfg.deviceId + "/status";
  if (!http.begin(client, url)) {
    err = "http begin failed";
    return false;
  }
  if (!addSignedHeaders(http, cfg, err)) {
    http.end();
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    err = "getStatus HTTP " + String(code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) {
    err = String("getStatus JSON: ") + jerr.c_str();
    return false;
  }
  int statusCode = doc["statusCode"] | -1;
  if (statusCode != 100) {
    err = "getStatus statusCode " + String(statusCode);
    return false;
  }

  status.lockState = doc["body"]["lockState"] | "";
  status.doorState = doc["body"]["doorState"] | "";
  status.fetchedAt = (uint32_t)time(nullptr);
  return true;
}

bool cloudSetupWebhook(const Config &cfg, String &err) {
  if (cfg.publicUrl.isEmpty()) {
    err = "no public URL configured";
    return false;
  }

  WiFiClientSecure client;
  cloudAttachCaBundle(client);
  HTTPClient http;
  http.setTimeout(8000);

  String url = String(API_BASE) + "/webhook/setupWebhook";
  if (!http.begin(client, url)) {
    err = "http begin failed";
    return false;
  }
  if (!addSignedHeaders(http, cfg, err)) {
    http.end();
    return false;
  }

  JsonDocument req;
  req["action"] = "setupWebhook";
  req["url"] = cfg.publicUrl;
  req["deviceList"] = "ALL";
  String payload;
  serializeJson(req, payload);

  int code = http.POST(payload);
  String body = http.getString();
  http.end();

  if (code != 200) {
    err = "setupWebhook HTTP " + String(code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    err = "setupWebhook: malformed response";
    return false;
  }
  int statusCode = doc["statusCode"] | -1;
  if (statusCode != 100) {
    err = "setupWebhook statusCode " + String(statusCode);
    return false;
  }
  return true;
}
