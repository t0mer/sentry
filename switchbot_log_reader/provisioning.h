// provisioning.h — SoftAP + captive portal + config form (CLAUDE.md §3).
#pragma once

#include <Arduino.h>

#include "config.h"

// Starts the AP, the DNS catch-all and the HTTP config form. `existing` is used
// to pre-fill the form on a reconfigure; secrets are shown as "set", never in
// plaintext.
void provisioningStart(const Config &existing);
void provisioningLoop();
bool provisioningActive();
String provisioningApSsid();
