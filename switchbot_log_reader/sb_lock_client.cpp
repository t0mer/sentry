#include "sb_lock_client.h"

#include <NimBLEDevice.h>
#include <string.h>

static bool g_bleReady = false;
static String g_lastError;
static uint32_t g_lastFetchMs = 0;

// Cached across fetches: scanning is the slow part, and the address type the
// scan reports is the one we must connect with.
static bool g_haveAddr = false;
static NimBLEAddress g_addr;

// A response never exceeds one notification at the negotiated MTU.
static volatile bool g_notifyPending = false;
static uint8_t g_notifyBuf[SB_MAX_FRAME];
static volatile size_t g_notifyLen = 0;

static const uint32_t SCAN_MS = 5000;
static const uint32_t NOTIFY_TIMEOUT_MS = 3000;

static void onNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
  (void)chr;
  (void)isNotify;
  if (len > sizeof(g_notifyBuf)) len = sizeof(g_notifyBuf);
  memcpy(g_notifyBuf, data, len);
  g_notifyLen = len;
  g_notifyPending = true;
}

void sbLockClientBegin() {
  if (g_bleReady) return;
  NimBLEDevice::init("");          // central only: no advertising, no MAC spoof
  NimBLEDevice::setMTU(128);       // room for longer log rows if the lock sends them
  g_bleReady = true;
}

static bool waitNotify(uint8_t *out, size_t outCap, size_t *outLen, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (!g_notifyPending) {
    if (millis() - start > timeoutMs) return false;
    delay(5);
  }
  size_t len = g_notifyLen;
  if (len > outCap) len = outCap;
  memcpy(out, (const void *)g_notifyBuf, len);
  *outLen = len;
  g_notifyPending = false;
  return true;
}

// Write one frame and wait for the matching notification.
static bool exchange(NimBLERemoteCharacteristic *writeChr, const uint8_t *frame, size_t frameLen,
                     uint8_t *resp, size_t respCap, size_t *respLen) {
  g_notifyPending = false;
  if (!writeChr->writeValue(frame, frameLen, false)) return false;
  return waitNotify(resp, respCap, respLen, NOTIFY_TIMEOUT_MS);
}

static bool resolveAddress(const String &lockMac, String &err) {
  String want;
  if (!normalizeMac(lockMac, want)) {
    err = "invalid lock MAC";
    return false;
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);
  NimBLEScanResults results = scan->getResults(SCAN_MS, false);

  bool found = false;
  for (int i = 0; i < (int)results.getCount(); i++) {
    const NimBLEAdvertisedDevice *dev = results.getDevice(i);
    if (dev == nullptr) continue;
    String addrStr = String(dev->getAddress().toString().c_str());
    if (addrStr.equalsIgnoreCase(want)) {
      g_addr = dev->getAddress();
      g_haveAddr = true;
      found = true;
      break;
    }
  }
  scan->clearResults();

  if (!found) err = "lock " + want + " not found in scan";
  return found;
}

// Runs the whole protocol on an already-connected client.
static bool runSession(NimBLEClient *client, const Config &cfg, uint32_t baseTime, uint8_t maxEntries,
                       std::vector<SbLogEntry> &out, String &err) {
  NimBLERemoteService *svc = client->getService(SB_SERVICE_UUID);
  if (svc == nullptr) {
    err = "SwitchBot service not found";
    return false;
  }
  NimBLERemoteCharacteristic *writeChr = svc->getCharacteristic(SB_WRITE_CHAR_UUID);
  NimBLERemoteCharacteristic *notifyChr = svc->getCharacteristic(SB_NOTIFY_CHAR_UUID);
  if (writeChr == nullptr || notifyChr == nullptr) {
    err = "SwitchBot characteristics not found";
    return false;
  }
  if (!notifyChr->subscribe(true, onNotify)) {
    err = "notify subscribe failed";
    return false;
  }

  uint8_t payload[SB_MAX_FRAME];  // plaintext command
  uint8_t cmd[SB_MAX_FRAME];      // framed (encrypted) command on the wire
  uint8_t resp[SB_MAX_FRAME];     // raw notification
  uint8_t plain[SB_MAX_FRAME];    // decrypted response
  size_t respLen = 0;
  size_t plainLen = 0;
  uint8_t status = 0;

  // 1. IV handshake (unencrypted).
  size_t n = sbBuildIvRequest(cfg.keyId, cmd, sizeof(cmd));
  if (n == 0 || !exchange(writeChr, cmd, n, resp, sizeof(resp), &respLen)) {
    err = "no response to IV request";
    return false;
  }
  uint8_t iv[SB_IV_LEN];
  if (!sbParseIvResponse(resp, respLen, iv)) {
    err = "bad IV response (wrong key id?)";
    return false;
  }

  // 2. Set the base time so only newer entries come back.
  n = sbBuildSetBaseTime(baseTime, payload, sizeof(payload));
  size_t cmdLen = sbBuildFrame(cfg.encKey, iv, cfg.keyId, payload, n, cmd, sizeof(cmd));
  if (cmdLen == 0) {
    err = "encrypt failed";
    return false;
  }
  if (!exchange(writeChr, cmd, cmdLen, resp, sizeof(resp), &respLen)) {
    err = "no response to set-base-time";
    return false;
  }
  if (!sbParseResponse(cfg.encKey, iv, resp, respLen, plain, sizeof(plain), &plainLen, &status) ||
      status != 0x01) {
    err = "set-base-time rejected (status 0x" + String(status, HEX) + ") — check the encryption key";
    return false;
  }

  // 3. Read entries until the lock returns an all-zero row.
  for (uint8_t i = 0; i < maxEntries; i++) {
    n = sbBuildReadEntry(payload, sizeof(payload));
    cmdLen = sbBuildFrame(cfg.encKey, iv, cfg.keyId, payload, n, cmd, sizeof(cmd));
    if (cmdLen == 0) {
      err = "encrypt failed";
      return false;
    }
    if (!exchange(writeChr, cmd, cmdLen, resp, sizeof(resp), &respLen)) {
      err = "no response to read-entry";
      return out.size() > 0;  // keep whatever we already decoded
    }
    if (!sbParseResponse(cfg.encKey, iv, resp, respLen, plain, sizeof(plain), &plainLen, &status)) {
      err = "decrypt failed";
      return false;
    }
    if (status != 0x01) break;

    SbLogEntry entry;
    if (!sbParseLogEntry(plain, plainLen, &entry)) break;  // all zeros: end of log
    out.push_back(entry);
  }

  return true;
}

bool sbLockFetchLog(const Config &cfg, uint32_t baseTime, uint8_t maxEntries,
                    std::vector<SbLogEntry> &out, String &err) {
  sbLockClientBegin();
  g_lastFetchMs = millis();

  if (!cfg.encKeySet) {
    err = "no encryption key configured";
    g_lastError = err;
    return false;
  }

  if (!g_haveAddr && !resolveAddress(cfg.lockMac, err)) {
    g_lastError = err;
    return false;
  }

  NimBLEClient *client = NimBLEDevice::createClient();
  if (client == nullptr) {
    err = "cannot create BLE client";
    g_lastError = err;
    return false;
  }
  client->setConnectTimeout(8000);  // NimBLE 2.x takes milliseconds

  bool ok = client->connect(g_addr);
  if (!ok) {
    // The cached address may be stale, or the hub is holding the link (§14).
    g_haveAddr = false;
    err = "BLE connect failed (hub may hold the link)";
  } else {
    ok = runSession(client, cfg, baseTime, maxEntries, out, err);
  }

  if (client->isConnected()) client->disconnect();
  delay(50);  // let the disconnect callback land before freeing the client
  NimBLEDevice::deleteClient(client);

  g_lastError = ok ? String("") : err;
  return ok;
}

String sbLockLastError() { return g_lastError; }
uint32_t sbLockLastFetchMillis() { return g_lastFetchMs; }
