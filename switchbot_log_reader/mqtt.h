// mqtt.h — PubSubClient wrapper with backoff, LWT and optional TLS.
#pragma once

#include <Arduino.h>

#include "config.h"

void mqttBegin(const Config &cfg);
void mqttLoop();
bool mqttConnected();
bool mqttEnabled();

// Publish one event JSON document to the configured topic.
bool mqttPublish(const String &json);

String mqttLastError();
