// sb_proto.h — SwitchBot encrypted BLE byte protocol (CLAUDE.md §5).
//
// Every command is framed as:
//     57 <key_id> <IV[0]> <IV[1]> <AES-128-CTR(enc_key, IV, payload)>
// where `payload` is the plaintext command bytes *after* the leading 0x57.
// Each command and each response restarts the CTR keystream from IV
// (nc_off = 0), so the same helper is used for both directions.
#pragma once

#include <Arduino.h>

#include "config.h"  // SB_ENC_KEY_LEN

static const size_t SB_IV_LEN = 16;
static const size_t SB_MAX_FRAME = 64;

// GATT UUIDs of the SwitchBot service.
extern const char SB_SERVICE_UUID[];
extern const char SB_WRITE_CHAR_UUID[];
extern const char SB_NOTIFY_CHAR_UUID[];

// One decoded row of the lock's on-device operation log.
struct SbLogEntry {
  uint32_t ts = 0;      // unix timestamp, big-endian in the wire format
  uint8_t index = 0;    // per-entry counter reported by the lock
  uint8_t source = 0;   // app / keypad / manual / ... (best effort)
  uint8_t action = 0;   // lock / unlock / jam / ...    (best effort)
  uint8_t value = 0;    // credential id (the "who" — map it yourself, §8)
  uint8_t payload[24] = {0};
  size_t payloadLen = 0;
};

// AES-128-CTR over `len` bytes; safe for in-place use (in == out).
bool sbAesCtr(const uint8_t key[SB_ENC_KEY_LEN], const uint8_t iv[SB_IV_LEN],
              const uint8_t *in, size_t len, uint8_t *out);

// Unencrypted IV handshake request: 57 00 00 00 0F 21 03 <key_id>. Returns len.
size_t sbBuildIvRequest(uint8_t keyId, uint8_t *out, size_t outCap);

// Response to the IV request: 01 xx xx xx <IV:16>.
bool sbParseIvResponse(const uint8_t *resp, size_t len, uint8_t iv[SB_IV_LEN]);

// Wrap a plaintext payload into an encrypted frame. Returns bytes written, 0 on error.
size_t sbBuildFrame(const uint8_t key[SB_ENC_KEY_LEN], const uint8_t iv[SB_IV_LEN], uint8_t keyId,
                    const uint8_t *payload, size_t payloadLen, uint8_t *out, size_t outCap);

// Decrypt a notify response: <status> xx xx <ciphertext>.
bool sbParseResponse(const uint8_t key[SB_ENC_KEY_LEN], const uint8_t iv[SB_IV_LEN],
                     const uint8_t *resp, size_t respLen, uint8_t *out, size_t outCap,
                     size_t *outLen, uint8_t *status);

// Payload builders. `ts` of 0 means "all history".
size_t sbBuildSetBaseTime(uint32_t ts, uint8_t *out, size_t outCap);
size_t sbBuildReadEntry(uint8_t *out, size_t outCap);

// Parse one decrypted log row. Returns false when the plaintext is all zeros,
// which is how the lock signals "no more entries".
bool sbParseLogEntry(const uint8_t *plain, size_t len, SbLogEntry *entry);

const char *sbSourceName(uint8_t source);
const char *sbActionName(uint8_t action);
