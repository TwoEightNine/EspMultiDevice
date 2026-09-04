#include "BrowserText.h"

#include <ctype.h>

namespace BrowserText {
namespace {

// U+FFFD is used for invalid byte sequences and undefined legacy-code-page
// positions before the text reaches the Unicode renderer.
constexpr uint16_t Replacement = 0xfffd;

String asciiLower(const String &source) {
  String result = source;
  for (uint16_t i = 0; i < result.length(); ++i) {
    if (result[i] >= 'A' && result[i] <= 'Z') result.setCharAt(i, result[i] + ('a' - 'A'));
  }
  return result;
}

String compactName(const String &source) {
  String result;
  for (uint16_t i = 0; i < source.length(); ++i) {
    const char value = source[i];
    if ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')) result += value;
  }
  return result;
}

String charsetParameter(const String &source) {
  const String lower = asciiLower(source);
  int index = lower.indexOf("charset");
  if (index < 0) return "";
  index += 7;
  while (index < (int)source.length() && isspace((unsigned char)source[index])) ++index;
  if (index >= (int)source.length() || source[index] != '=') return "";
  ++index;
  while (index < (int)source.length() && isspace((unsigned char)source[index])) ++index;
  if (index >= (int)source.length()) return "";
  const char quote = source[index] == '\'' || source[index] == '"' ? source[index++] : 0;
  const int start = index;
  while (index < (int)source.length()) {
    const char value = source[index];
    if ((quote && value == quote) || (!quote && (isspace((unsigned char)value) || value == ';' || value == '>'))) break;
    ++index;
  }
  return compactName(asciiLower(source.substring(start, index)));
}

String metaCharset(const String &source) {
  const uint16_t limit = source.length() > 4096 ? 4096 : source.length();
  const String prefix = source.substring(0, limit);
  const String lower = asciiLower(prefix);
  int start = 0;
  while ((start = lower.indexOf("<meta", start)) >= 0) {
    const int end = lower.indexOf('>', start + 5);
    if (end < 0) break;
    const String charset = charsetParameter(prefix.substring(start, end + 1));
    if (charset.length()) return charset;
    start = end + 1;
  }
  return "";
}

bool validUtf8(const String &source, uint32_t start = 0) {
  for (uint32_t i = start; i < source.length();) {
    const uint8_t first = (uint8_t)source[i++];
    if (first < 0x80) continue;
    uint8_t extra = 0;
    uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) { extra = 1; codepoint = first & 0x1f; }
    else if (first >= 0xe0 && first <= 0xef) { extra = 2; codepoint = first & 0x0f; }
    else if (first >= 0xf0 && first <= 0xf4) { extra = 3; codepoint = first & 0x07; }
    else return false;
    if (i + extra > source.length()) return false;
    for (uint8_t j = 0; j < extra; ++j) {
      const uint8_t next = (uint8_t)source[i++];
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((extra == 1 && codepoint < 0x80) || (extra == 2 && codepoint < 0x800) ||
        (extra == 3 && codepoint < 0x10000) || (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint > 0x10ffff) return false;
  }
  return true;
}

void appendUtf8(String &target, uint32_t codepoint) {
  if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) codepoint = Replacement;
  if (codepoint < 0x80) target += (char)codepoint;
  else if (codepoint < 0x800) {
    target += (char)(0xc0 | (codepoint >> 6));
    target += (char)(0x80 | (codepoint & 0x3f));
  } else if (codepoint < 0x10000) {
    target += (char)(0xe0 | (codepoint >> 12));
    target += (char)(0x80 | ((codepoint >> 6) & 0x3f));
    target += (char)(0x80 | (codepoint & 0x3f));
  } else {
    target += (char)(0xf0 | (codepoint >> 18));
    target += (char)(0x80 | ((codepoint >> 12) & 0x3f));
    target += (char)(0x80 | ((codepoint >> 6) & 0x3f));
    target += (char)(0x80 | (codepoint & 0x3f));
  }
}

uint16_t windows1252(uint8_t value) {
  static const uint16_t special[] = {
    0x20ac, Replacement, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, Replacement, 0x017d, Replacement,
    Replacement, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, Replacement, 0x017e, 0x0178
  };
  return value < 0x80 ? value : (value < 0xa0 ? special[value - 0x80] : value);
}

uint16_t windows1251(uint8_t value) {
  static const uint16_t special[] = {
    0x0402, 0x0403, 0x201a, 0x0453, 0x201e, 0x2026, 0x2020, 0x2021,
    0x20ac, 0x2030, 0x0409, 0x2039, 0x040a, 0x040c, 0x040b, 0x040f,
    0x0452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    Replacement, 0x2122, 0x0459, 0x203a, 0x045a, 0x045c, 0x045b, 0x045f,
    0x00a0, 0x040e, 0x045e, 0x0408, 0x00a4, 0x0490, 0x00a6, 0x00a7,
    0x0401, 0x00a9, 0x0404, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x0407,
    0x00b0, 0x00b1, 0x0406, 0x0456, 0x0491, 0x00b5, 0x00b6, 0x00b7,
    0x0451, 0x2116, 0x0454, 0x00bb, 0x0458, 0x0405, 0x0455, 0x0457
  };
  if (value < 0x80) return value;
  if (value >= 0xc0) return 0x0410 + value - 0xc0;
  return special[value - 0x80];
}

uint16_t iso88595(uint8_t value) {
  if (value < 0xa0) return value;
  if (value == 0xa0 || value == 0xad) return value;
  if (value >= 0xb0 && value <= 0xcf) return 0x0410 + value - 0xb0;
  if (value >= 0xd0 && value <= 0xef) return 0x0430 + value - 0xd0;
  if (value == 0xf0) return 0x2116;
  if (value >= 0xf1 && value <= 0xfc) return 0x0451 + value - 0xf1;
  if (value == 0xfd) return 0x00a7;
  if (value == 0xfe) return 0x045e;
  if (value == 0xff) return 0x045f;
  if (value >= 0xa1 && value <= 0xac) return 0x0401 + value - 0xa1;
  if (value == 0xae) return 0x040e;
  if (value == 0xaf) return 0x040f;
  return Replacement;
}

uint16_t cp866(uint8_t value) {
  if (value < 0x80) return value;
  if (value <= 0x9f) return 0x0410 + value - 0x80;
  if (value <= 0xbf) return 0x0430 + value - 0xa0;
  switch (value) {
    case 0xf0: return 0x0401; case 0xf1: return 0x0451;
    case 0xf2: return 0x0404; case 0xf3: return 0x0454;
    case 0xf4: return 0x0407; case 0xf5: return 0x0457;
    case 0xf6: return 0x040e; case 0xf7: return 0x045e;
    default: return windows1252(value);
  }
}

uint16_t koi8(uint8_t value, bool ukrainian) {
  static const uint16_t lower[] = {
    0x044e, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433,
    0x0445, 0x0438, 0x0439, 0x043a, 0x043b, 0x043c, 0x043d, 0x043e,
    0x043f, 0x044f, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432,
    0x044c, 0x044b, 0x0437, 0x0448, 0x044d, 0x0449, 0x0447, 0x044a
  };
  if (value >= 0xc0 && value <= 0xdf) return lower[value - 0xc0];
  if (value >= 0xe0) return lower[value - 0xe0] - 0x20;
  if (ukrainian) {
    switch (value) {
      case 0xa4: return 0x0454; case 0xa6: return 0x0456; case 0xa7: return 0x0457; case 0xad: return 0x0491;
      case 0xb4: return 0x0404; case 0xb6: return 0x0406; case 0xb7: return 0x0407; case 0xbd: return 0x0490;
    }
  }
  return windows1252(value);
}

enum class Encoding : uint8_t { Utf8, Ascii, Windows1251, Koi8R, Koi8U, Iso88595, Cp866, Iso88591, Windows1252, Unsupported };

Encoding encodingFor(const String &name) {
  if (name == "utf8" || name == "unicode11utf8") return Encoding::Utf8;
  if (name == "ascii" || name == "usascii") return Encoding::Ascii;
  if (name == "windows1251" || name == "cp1251" || name == "xcp1251" || name == "mscyrillic") return Encoding::Windows1251;
  if (name == "koi8r") return Encoding::Koi8R;
  if (name == "koi8u") return Encoding::Koi8U;
  if (name == "iso88595" || name == "isoir144" || name == "cyrillic") return Encoding::Iso88595;
  if (name == "cp866" || name == "ibm866" || name == "866") return Encoding::Cp866;
  if (name == "iso88591" || name == "latin1" || name == "l1") return Encoding::Iso88591;
  if (name == "windows1252" || name == "cp1252" || name == "ansi") return Encoding::Windows1252;
  return Encoding::Unsupported;
}

uint16_t decodeByte(Encoding encoding, uint8_t value) {
  switch (encoding) {
    case Encoding::Ascii: return value < 0x80 ? value : Replacement;
    case Encoding::Windows1251: return windows1251(value);
    case Encoding::Koi8R: return koi8(value, false);
    case Encoding::Koi8U: return koi8(value, true);
    case Encoding::Iso88595: return iso88595(value);
    case Encoding::Cp866: return cp866(value);
    case Encoding::Iso88591: return value;
    case Encoding::Windows1252: return windows1252(value);
    default: return value;
  }
}

uint8_t utf8Bytes(uint16_t codepoint) {
  return codepoint < 0x80 ? 1 : (codepoint < 0x800 ? 2 : 3);
}

}  // namespace

void begin(TFT_eSPI &tft, Size size) {
  UiText::begin(tft, size);
}

int16_t lineHeight(Size size) {
  return UiText::lineHeight(size);
}

int16_t measure(const String &text, Size size) {
  return UiText::measure(text, size);
}

void end(TFT_eSPI &tft) {
  UiText::end(tft);
}

bool normalizeToUtf8(const String &source, const String &contentType,
                     String &normalized, String &error) {
  error = "";
  normalized = "";
  uint32_t bodyStart = 0;
  String charset;
  if (source.length() >= 3 && (uint8_t)source[0] == 0xef && (uint8_t)source[1] == 0xbb && (uint8_t)source[2] == 0xbf) {
    charset = "utf8";
    bodyStart = 3;
  } else if (source.length() >= 2 && (((uint8_t)source[0] == 0xff && (uint8_t)source[1] == 0xfe) ||
                                       ((uint8_t)source[0] == 0xfe && (uint8_t)source[1] == 0xff))) {
    error = "Unsupported text encoding: UTF-16";
    return false;
  } else {
    charset = charsetParameter(contentType);
    if (!charset.length()) charset = metaCharset(source);
    if (!charset.length()) charset = validUtf8(source) ? "utf8" : "windows1251";
  }
  const Encoding encoding = encodingFor(charset);
  if (encoding == Encoding::Unsupported) {
    error = "Unsupported text encoding: " + charset;
    return false;
  }
  if (encoding == Encoding::Utf8) {
    if (!validUtf8(source, bodyStart)) {
      error = "Invalid UTF-8 page";
      return false;
    }
    normalized = source.substring(bodyStart);
    return true;
  }
  // Most legacy-declared HTML is still ASCII (notably Google responses).
  // Do not reserve a worst-case 2x buffer up front: an 84 KiB response then
  // asks the ESP32's internal heap for 168 KiB even though its decoded output
  // is normally the same size. That can fail before rendering starts.
  bool needsExpansion = false;
  for (uint32_t i = bodyStart; i < source.length(); ++i) {
    if ((uint8_t)source[i] >= 0x80) {
      needsExpansion = true;
      break;
    }
  }
  if (!needsExpansion) {
    normalized = source.substring(bodyStart);
    return true;
  }
  uint32_t normalizedLength = 0;
  for (uint32_t i = bodyStart; i < source.length(); ++i) {
    normalizedLength += utf8Bytes(decodeByte(encoding, (uint8_t)source[i]));
  }
  if (!normalized.reserve((unsigned int)normalizedLength)) {
    error = "Text page is too large to decode";
    return false;
  }
  for (uint32_t i = bodyStart; i < source.length(); ++i) {
    const uint8_t value = (uint8_t)source[i];
    appendUtf8(normalized, decodeByte(encoding, value));
  }
  return true;
}

int16_t width(TFT_eSPI &tft, const String &text, Size size) {
  return UiText::width(tft, text, size);
}

void draw(TFT_eSPI &tft, const String &text, int16_t x, int16_t y,
          uint16_t color, uint16_t background, Size size) {
  UiText::draw(tft, text, x, y, color, background, size);
}

String ellipsize(TFT_eSPI &tft, const String &text, int16_t maxWidth, Size size) {
  (void)tft;
  return UiText::ellipsize(text, maxWidth, size);
}

}  // namespace BrowserText
