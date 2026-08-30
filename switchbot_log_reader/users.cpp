#include "users.h"

#include <ArduinoJson.h>
#include <Preferences.h>

// Stored as one small JSON object under a single NVS key: the table is tiny and
// Preferences cannot enumerate keys.
static const char NVS_NS[] = "sbusers";
static const char NVS_KEY[] = "map";

static JsonDocument g_doc;
static bool g_loaded = false;

static String mapKey(uint8_t source, uint8_t value) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%u_%u", (unsigned)source, (unsigned)value);
  return String(buf);
}

static void usersSaveLocked() {
  String out;
  serializeJson(g_doc, out);
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putString(NVS_KEY, out);
  p.end();
}

void usersBegin() {
  if (g_loaded) return;
  Preferences p;
  String raw;
  if (p.begin(NVS_NS, true)) {
    raw = p.getString(NVS_KEY, "");
    p.end();
  }
  g_doc.clear();
  if (raw.isEmpty() || deserializeJson(g_doc, raw)) {
    g_doc.to<JsonObject>();
  }
  g_loaded = true;
}

String userLookup(uint8_t source, uint8_t value) {
  usersBegin();
  JsonVariant v = g_doc[mapKey(source, value)];
  if (v.isNull()) return String("");
  return String(v.as<const char *>());
}

bool userSet(uint8_t source, uint8_t value, const String &name) {
  usersBegin();
  String key = mapKey(source, value);
  JsonObject obj = g_doc.as<JsonObject>();
  if (!obj[key].is<const char *>() && obj.size() >= USERS_MAX) return false;
  obj[key] = name;
  usersSaveLocked();
  return true;
}

bool userRemove(uint8_t source, uint8_t value) {
  usersBegin();
  JsonObject obj = g_doc.as<JsonObject>();
  String key = mapKey(source, value);
  if (!obj[key].is<const char *>()) return false;
  obj.remove(key);
  usersSaveLocked();
  return true;
}

void usersForEach(const std::function<void(uint8_t, uint8_t, const String &)> &fn) {
  usersBegin();
  for (JsonPair kv : g_doc.as<JsonObject>()) {
    unsigned source = 0, value = 0;
    if (sscanf(kv.key().c_str(), "%u_%u", &source, &value) != 2) continue;
    fn((uint8_t)source, (uint8_t)value, String(kv.value().as<const char *>()));
  }
}

size_t usersCount() {
  usersBegin();
  return g_doc.as<JsonObject>().size();
}
