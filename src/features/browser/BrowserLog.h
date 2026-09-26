#pragma once

#include <Arduino.h>

// Browser URLs may contain Assistant search text, form values, or tokens.
// Keep transport metrics but never put a URL into Serial logs.
inline String browserRedactUrls(const String &message) {
  String safe;
  size_t cursor = 0;
  while (cursor < message.length()) {
    const int http = message.indexOf("http://", cursor);
    const int https = message.indexOf("https://", cursor);
    int start = http < 0 ? https : (https < 0 ? http : min(http, https));
    if (start < 0) {
      safe += message.substring(cursor);
      break;
    }
    safe += message.substring(cursor, start);
    safe += F("[url]");
    cursor = start;
    while (cursor < message.length() && message[cursor] != ' ' &&
           message[cursor] != '\n' && message[cursor] != '\r') ++cursor;
  }
  return safe;
}
