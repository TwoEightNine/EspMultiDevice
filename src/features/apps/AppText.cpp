#include "AppText.h"

#include <string.h>

namespace AppText {
namespace {
bool unreserved(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

int hexValue(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool equalsIgnoreCase(const char *a, size_t length, const char *b) {
  if (strlen(b) != length) return false;
  for (size_t i = 0; i < length; ++i) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != b[i]) return false;
  }
  return true;
}

struct NamedEntity { const char *name; uint32_t codepoint; };
const NamedEntity Entities[] = {
  {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
  {"nbsp", 0xA0}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"mdash", 0x2014},
  {"ndash", 0x2013}, {"hellip", 0x2026}, {"copy", 0xA9}, {"reg", 0xAE},
  {"trade", 0x2122}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
  {"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0xB7}, {"deg", 0xB0},
  {"times", 0xD7}, {"euro", 0x20AC},
};

// Tags whose end (or the tag itself, for void elements) starts a new line.
bool breakingTag(const char *name, size_t length) {
  static const char *const tags[] = {
    "br", "p", "div", "li", "ul", "ol", "tr", "table", "blockquote", "pre",
    "h1", "h2", "h3", "h4", "h5", "h6", "hr", "section", "article"};
  for (const char *tag : tags) if (equalsIgnoreCase(name, length, tag)) return true;
  return false;
}

bool hiddenTag(const char *name, size_t length) {
  return equalsIgnoreCase(name, length, "script") || equalsIgnoreCase(name, length, "style");
}
}

std::string urlEncode(const char *text, size_t length) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(length * 3);
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (unreserved(c)) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

bool urlDecode(const char *text, size_t length, bool plusAsSpace, std::string &out) {
  out.clear();
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (c == '%') {
      if (i + 2 >= length) return false;
      const int high = hexValue(static_cast<uint8_t>(text[i + 1]));
      const int low = hexValue(static_cast<uint8_t>(text[i + 2]));
      if (high < 0 || low < 0) return false;
      out += static_cast<char>((high << 4) | low);
      i += 2;
    } else if (c == '+' && plusAsSpace) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return true;
}

void appendUtf8(std::string &out, uint32_t codepoint) {
  if (codepoint == 0 || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
    codepoint = 0xFFFD;
  if (codepoint < 0x80) {
    out += static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    out += static_cast<char>(0xC0 | (codepoint >> 6));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    out += static_cast<char>(0xE0 | (codepoint >> 12));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (codepoint >> 18));
    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  }
}

bool validUtf8(const char *text, size_t length) {
  size_t i = 0;
  while (i < length) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    uint8_t need = 0;
    uint8_t low = 0x80, high = 0xBF;
    if (c < 0x80) { ++i; continue; }
    if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c >= 0xE0 && c <= 0xEF) {
      need = 2;
      if (c == 0xE0) low = 0xA0;
      if (c == 0xED) high = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      need = 3;
      if (c == 0xF0) low = 0x90;
      if (c == 0xF4) high = 0x8F;
    } else return false;
    if (i + need >= length) return false;
    for (uint8_t k = 1; k <= need; ++k) {
      const uint8_t next = static_cast<uint8_t>(text[i + k]);
      if (k == 1 ? (next < low || next > high) : (next < 0x80 || next > 0xBF)) return false;
    }
    i += need + 1;
  }
  return true;
}

size_t utf8Prefix(const char *text, size_t length, size_t maxBytes) {
  if (length <= maxBytes) return length;
  size_t end = maxBytes;
  while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) --end;
  return end;
}

std::string htmlToText(const char *html, size_t length, size_t maxBytes, bool &truncated) {
  truncated = false;
  std::string out;
  out.reserve(length < maxBytes ? length : maxBytes);
  bool pendingSpace = false;
  uint8_t pendingBreaks = 0;
  bool hidden = false;
  char hiddenName[8] = {};
  size_t hiddenLength = 0;

  auto lineBreak = [&]() {
    pendingSpace = false;
    if (!out.empty() && pendingBreaks < 3) ++pendingBreaks;
  };
  auto emit = [&](const char *bytes, size_t count) {
    if (truncated) return;
    std::string piece;
    if (pendingBreaks) {
      piece.append(pendingBreaks, '\n');
      pendingBreaks = 0;
      pendingSpace = false;
    } else if (pendingSpace && !out.empty()) {
      piece += ' ';
    }
    pendingSpace = false;
    piece.append(bytes, count);
    if (out.size() + piece.size() > maxBytes) {
      const size_t room = maxBytes - out.size();
      out.append(piece, 0, utf8Prefix(piece.data(), piece.size(), room));
      truncated = true;
      return;
    }
    out += piece;
  };

  size_t i = 0;
  while (i < length && !truncated) {
    const char c = html[i];
    if (c == '<') {
      size_t end = i + 1;
      while (end < length && html[end] != '>') ++end;
      if (end >= length) break;  // An unterminated tag ends the fragment.
      size_t nameStart = i + 1;
      const bool closing = nameStart < end && html[nameStart] == '/';
      if (closing) ++nameStart;
      size_t nameEnd = nameStart;
      while (nameEnd < end && ((html[nameEnd] >= 'a' && html[nameEnd] <= 'z') ||
                               (html[nameEnd] >= 'A' && html[nameEnd] <= 'Z') ||
                               (html[nameEnd] >= '0' && html[nameEnd] <= '9'))) ++nameEnd;
      const char *name = html + nameStart;
      const size_t nameLength = nameEnd - nameStart;
      if (hidden) {
        if (closing && nameLength == hiddenLength &&
            equalsIgnoreCase(name, nameLength, hiddenName)) hidden = false;
      } else if (!closing && hiddenTag(name, nameLength)) {
        hidden = true;
        hiddenLength = nameLength;
        for (size_t k = 0; k < nameLength && k < sizeof(hiddenName) - 1; ++k)
          hiddenName[k] = static_cast<char>(name[k] | 0x20);
        hiddenName[nameLength < sizeof(hiddenName) ? nameLength : sizeof(hiddenName) - 1] = 0;
      } else if (breakingTag(name, nameLength)) {
        lineBreak();
      }
      i = end + 1;
      continue;
    }
    if (hidden) { ++i; continue; }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pendingSpace = true;
      ++i;
      continue;
    }
    if (c == '&') {
      size_t end = i + 1;
      while (end < length && end - i <= 10 && html[end] != ';' && html[end] != '&' &&
             html[end] != '<' && html[end] != ' ') ++end;
      if (end < length && html[end] == ';') {
        const char *entity = html + i + 1;
        const size_t entityLength = end - i - 1;
        uint32_t codepoint = 0;
        bool known = false;
        if (entityLength > 1 && entity[0] == '#') {
          const bool hex = entity[1] == 'x' || entity[1] == 'X';
          known = entityLength > (hex ? 2u : 1u);
          for (size_t k = hex ? 2 : 1; k < entityLength && known; ++k) {
            const int digit = hex ? hexValue(static_cast<uint8_t>(entity[k]))
                                  : (entity[k] >= '0' && entity[k] <= '9' ? entity[k] - '0' : -1);
            if (digit < 0 || codepoint > 0x10FFFF) known = false;
            else codepoint = codepoint * (hex ? 16 : 10) + static_cast<uint32_t>(digit);
          }
        } else {
          for (const NamedEntity &named : Entities) {
            if (entityLength == strlen(named.name) && memcmp(entity, named.name, entityLength) == 0) {
              codepoint = named.codepoint;
              known = true;
              break;
            }
          }
        }
        if (known) {
          if (codepoint == 0xA0 || codepoint == ' ') {
            pendingSpace = true;
          } else {
            std::string encoded;
            appendUtf8(encoded, codepoint);
            emit(encoded.data(), encoded.size());
          }
          i = end + 1;
          continue;
        }
      }
    }
    // Copy one UTF-8 sequence; invalid bytes become U+FFFD.
    const uint8_t lead = static_cast<uint8_t>(c);
    size_t count = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC2 ? 2 : 0;
    if (count == 0 || i + count > length || !validUtf8(html + i, count)) {
      static const char replacement[] = "\xEF\xBF\xBD";
      emit(replacement, 3);
      ++i;
      continue;
    }
    emit(html + i, count);
    i += count;
  }
  return out;
}

}  // namespace AppText
