#include "sb_proto.h"

#include <mbedtls/aes.h>
#include <string.h>

const char SB_SERVICE_UUID[] = "cba20d00-224d-11e6-9fb8-0002a5d5c51b";
const char SB_WRITE_CHAR_UUID[] = "cba20002-224d-11e6-9fb8-0002a5d5c51b";
const char SB_NOTIFY_CHAR_UUID[] = "cba20003-224d-11e6-9fb8-0002a5d5c51b";

bool sbAesCtr(const uint8_t key[SB_ENC_KEY_LEN], const uint8_t iv[SB_IV_LEN],
              const uint8_t *in, size_t len, uint8_t *out) {
  if (len == 0) return true;
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }
  uint8_t nonce[SB_IV_LEN];
  memcpy(nonce, iv, SB_IV_LEN);
  uint8_t stream[16] = {0};
  size_t ncOff = 0;  // every command/response restarts the keystream
  int rc = mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, nonce, stream, in, out);
  mbedtls_aes_free(&ctx);
  return rc == 0;
}

size_t sbBuildIvRequest(uint8_t keyId, uint8_t *out, size_t outCap) {
  static const uint8_t kReq[] = {0x57, 0x00, 0x00, 0x00, 0x0F, 0x21, 0x03};
  if (outCap < sizeof(kReq) + 1) return 0;
  memcpy(out, kReq, sizeof(kReq));
  out[sizeof(kReq)] = keyId;
  return sizeof(kReq) + 1;
}

bool sbParseIvResponse(const uint8_t *resp, size_t len, uint8_t iv[SB_IV_LEN]) {
  if (len < 4 + SB_IV_LEN) return false;
  if (resp[0] != 0x01) return false;
  memcpy(iv, resp + 4, SB_IV_LEN);
  return true;
}

size_t sbBuildFrame(const uint8_t key[SB_ENC_KEY_LEN], const uint8_t iv[SB_IV_LEN], uint8_t keyId,
                    const uint8_t *payload, size_t payloadLen, uint8_t *out, size_t outCap) {
  if (payloadLen == 0 || outCap < payloadLen + 4) return 0;
  out[0] = 0x57;
  out[1] = keyId;
  out[2] = iv[0];
  out[3] = iv[1];
  if (!sbAesCtr(key, iv, payload, payloadLen, out + 4)) return 0;
  return payloadLen + 4;
}

bool sbParseResponse(const uint8_t key[SB_ENC_KEY_LEN], const uint8_t iv[SB_IV_LEN],
                     const uint8_t *resp, size_t respLen, uint8_t *out, size_t outCap,
                     size_t *outLen, uint8_t *status) {
  if (respLen < 4) return false;
  size_t ctLen = respLen - 4;
  if (ctLen > outCap) return false;
  if (status) *status = resp[0];
  if (!sbAesCtr(key, iv, resp + 4, ctLen, out)) return false;
  if (outLen) *outLen = ctLen;
  return true;
}

size_t sbBuildSetBaseTime(uint32_t ts, uint8_t *out, size_t outCap) {
  if (outCap < 7) return 0;
  out[0] = 0x00;
  out[1] = 0x14;
  out[2] = 0x01;
  out[3] = (uint8_t)(ts >> 24);
  out[4] = (uint8_t)(ts >> 16);
  out[5] = (uint8_t)(ts >> 8);
  out[6] = (uint8_t)(ts);
  return 7;
}

size_t sbBuildReadEntry(uint8_t *out, size_t outCap) {
  if (outCap < 3) return 0;
  out[0] = 0x00;
  out[1] = 0x14;
  out[2] = 0x05;
  return 3;
}

bool sbParseLogEntry(const uint8_t *plain, size_t len, SbLogEntry *entry) {
  if (len < 8 || entry == nullptr) return false;

  bool allZero = true;
  for (size_t i = 0; i < len; i++) {
    if (plain[i] != 0) {
      allZero = false;
      break;
    }
  }
  if (allZero) return false;  // end of log

  entry->ts = ((uint32_t)plain[0] << 24) | ((uint32_t)plain[1] << 16) |
              ((uint32_t)plain[2] << 8) | (uint32_t)plain[3];
  entry->index = plain[4];
  entry->source = plain[5];
  entry->action = plain[6];
  entry->value = plain[7];

  size_t extra = len - 8;
  if (extra > sizeof(entry->payload)) extra = sizeof(entry->payload);
  memcpy(entry->payload, plain + 8, extra);
  entry->payloadLen = extra;
  return true;
}

// Only the values documented in CLAUDE.md §5 are named; anything else stays
// "unknown" on purpose — the raw ids are always published so the mapping can be
// learned empirically (§8) rather than guessed here.
const char *sbSourceName(uint8_t source) {
  switch (source) {
    case 0: return "app";
    case 1: return "keypad";
    case 2: return "manual";
    default: return "unknown";
  }
}

const char *sbActionName(uint8_t action) {
  switch (action) {
    case 0: return "lock";
    case 1: return "unlock";
    case 2: return "jam";
    default: return "unknown";
  }
}
