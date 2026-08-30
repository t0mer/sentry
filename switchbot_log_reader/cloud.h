// cloud.h — SwitchBot Open API v1.1 client (CLAUDE.md §6).
#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include "config.h"

struct LockStatus {
  String lockState;  // "LOCKED" / "UNLOCKED"
  String doorState;  // "OPEN" / "CLOSED" (model dependent)
  uint32_t fetchedAt = 0;
};

// GET /v1.1/devices/{device_id}/status
bool cloudGetStatus(const Config &cfg, LockStatus &status, String &err);

// POST /v1.1/webhook/setupWebhook with the device's public URL.
bool cloudSetupWebhook(const Config &cfg, String &err);

// Attach the ESP-IDF root CA bundle to a TLS client (also used by mqtt.cpp).
void cloudAttachCaBundle(WiFiClientSecure &client);
