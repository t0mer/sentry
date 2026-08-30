// webui.h — station-mode web UI: status, /logs, webhook sink, Learn users.
#pragma once

#include <Arduino.h>
#include <functional>

#include "config.h"
#include "sb_proto.h"

// Called when an inbound webhook (or the "Fetch now" button) asks for a BLE
// read. Returns false if the fetch failed.
typedef std::function<bool(const char *reason)> WebUiFetchFn;

void webuiBegin(const Config &cfg, WebUiFetchFn fetchFn);
void webuiLoop();

// Record a published event so it shows up in / and /logs, and so its raw
// credential id can be named on the Learn users page.
void webuiPushEvent(const SbLogEntry &entry, const String &json);

// Status shown on the landing page.
void webuiSetLockState(const String &lockState, const String &doorState);
void webuiSetNote(const String &note);
