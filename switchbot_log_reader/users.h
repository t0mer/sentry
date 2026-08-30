// users.h — credential id -> friendly name table (CLAUDE.md §8).
//
// The lock reports a numeric credential id, never a name. The table is keyed by
// (source, value) because the same numeric value can mean different things for
// a keypad code and an app user.
#pragma once

#include <Arduino.h>
#include <functional>

static const size_t USERS_MAX = 32;

void usersBegin();

// Empty String when the credential has not been named yet.
String userLookup(uint8_t source, uint8_t value);

bool userSet(uint8_t source, uint8_t value, const String &name);
bool userRemove(uint8_t source, uint8_t value);
void usersForEach(const std::function<void(uint8_t source, uint8_t value, const String &name)> &fn);
size_t usersCount();
