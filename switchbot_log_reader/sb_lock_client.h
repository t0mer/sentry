// sb_lock_client.h — NimBLE central that reads the lock's operation log.
//
// Connects only for the duration of a fetch and disconnects again, so the
// SwitchBot hub keeps its own link to the lock (CLAUDE.md §5 step 7, §14).
#pragma once

#include <Arduino.h>
#include <vector>

#include "config.h"
#include "sb_proto.h"

// Call once from setup(), after WiFi is up. Idempotent.
void sbLockClientBegin();

// Fetch log entries newer than `baseTime` (0 = whole history).
// Returns false and fills `err` on any BLE/protocol failure.
bool sbLockFetchLog(const Config &cfg, uint32_t baseTime, uint8_t maxEntries,
                    std::vector<SbLogEntry> &out, String &err);

// Human-readable state of the last fetch, for the web UI.
String sbLockLastError();
uint32_t sbLockLastFetchMillis();
