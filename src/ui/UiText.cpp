#include "UiText.h"

#include "fonts/BrowserFreeSans14.h"
#include "fonts/BrowserFreeSans.h"
#include "fonts/BrowserFreeSans24.h"
#include "fonts/AssistantNotoSansSC18.h"

namespace UiText {
namespace {
constexpr uint16_t Replacement = 0xfffd;

TFT_eSPI *fontTft = nullptr;
uint8_t fontDepth = 0;
Size activeSize = Size::Content;
enum class Face : uint8_t { Ui, AssistantCjk };
Face activeFace = Face::Ui;

const uint8_t *fontFor(Size size, Face face = Face::Ui) {
  if (face == Face::AssistantCjk) return AssistantNotoSansSC18;
  switch (size) {
    case Size::Compact: return BrowserFreeSans14Unicode;
    case Size::Title: return BrowserFreeSans24Unicode;
    default: return BrowserFreeSansUnicode;
  }
}

void selectFont(TFT_eSPI &tft, Size size, Face face = Face::Ui) {
  if (fontTft != &tft || fontDepth == 0 || activeSize != size || activeFace != face) {
    tft.loadFont(fontFor(size, face));
    fontTft = &tft;
    activeSize = size;
    activeFace = face;
  }
}

bool needsActivation(TFT_eSPI &tft) {
  return fontTft != &tft || fontDepth == 0;
}

uint32_t fontValue(const uint8_t *font, uint32_t offset) {
  return ((uint32_t)pgm_read_byte(font + offset) << 24) |
         ((uint32_t)pgm_read_byte(font + offset + 1) << 16) |
         ((uint32_t)pgm_read_byte(font + offset + 2) << 8) |
         (uint32_t)pgm_read_byte(font + offset + 3);
}

uint8_t sizeIndex(Size size) {
  return size == Size::Compact ? 0 : (size == Size::Title ? 2 : 1);
}

bool nextCodepoint(const String &text, uint16_t &offset, uint32_t &codepoint) {
  if (offset >= text.length()) return false;
  const uint8_t first = (uint8_t)text[offset++];
  if (first < 0x80) { codepoint = first; return true; }
  uint8_t extra = first >= 0xc2 && first <= 0xdf ? 1 : (first >= 0xe0 && first <= 0xef ? 2 :
                  (first >= 0xf0 && first <= 0xf4 ? 3 : 0));
  if (!extra || offset + extra > text.length()) { codepoint = Replacement; return true; }
  codepoint = first & (extra == 1 ? 0x1f : (extra == 2 ? 0x0f : 0x07));
  for (uint8_t i = 0; i < extra; ++i) {
    const uint8_t next = (uint8_t)text[offset++];
    if ((next & 0xc0) != 0x80) { codepoint = Replacement; return true; }
    codepoint = (codepoint << 6) | (next & 0x3f);
  }
  return true;
}

bool containsHan(const String &text) {
  uint16_t offset = 0;
  uint32_t codepoint = 0;
  while (nextCodepoint(text, offset, codepoint)) {
    if ((codepoint >= 0x3400 && codepoint <= 0x4dbf) ||
        (codepoint >= 0x4e00 && codepoint <= 0x9fff) ||
        (codepoint >= 0xf900 && codepoint <= 0xfaff)) return true;
  }
  return false;
}

Face assistantFace(const String &text) {
  return containsHan(text) ? Face::AssistantCjk : Face::Ui;
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
    target += (char)(0x80 | (codepoint & 0x3f));
  }
}

int16_t glyphAdvance(const uint8_t *font, uint16_t glyphCount, uint32_t codepoint, Size size,
                     Face face, int16_t fallback) {
  static uint8_t cache[2][3][384] = {};
  const int16_t cacheIndex = codepoint < 0x80 ? (int16_t)codepoint :
    (codepoint >= 0x0400 && codepoint <= 0x04ff ? (int16_t)(128 + codepoint - 0x0400) : -1);
  uint8_t &cached = cache[(uint8_t)face][sizeIndex(size)][cacheIndex >= 0 ? cacheIndex : 0];
  if (cacheIndex >= 0 && cached) return cached;
  int16_t advance = fallback;
  uint16_t first = 0;
  uint16_t last = glyphCount;
  while (first < last) {
    const uint16_t glyph = first + (last - first) / 2;
    const uint32_t value = fontValue(font, 24UL + (uint32_t)glyph * 28UL);
    if (value < codepoint) first = glyph + 1;
    else last = glyph;
  }
  if (first < glyphCount &&
      fontValue(font, 24UL + (uint32_t)first * 28UL) == codepoint) {
    advance = (int16_t)fontValue(font, 24UL + (uint32_t)first * 28UL + 12);
  }
  if (cacheIndex >= 0 && advance > 0 && advance < 256) cached = advance;
  return advance;
}

bool hasGlyph(const uint8_t *font, uint16_t glyphCount, uint32_t codepoint) {
  if (codepoint == ' ' || codepoint == '\r' || codepoint == '\n') return true;
  uint16_t first = 0;
  uint16_t last = glyphCount;
  while (first < last) {
    const uint16_t glyph = first + (last - first) / 2;
    const uint32_t value = fontValue(font, 24UL + (uint32_t)glyph * 28UL);
    if (value < codepoint) first = glyph + 1;
    else last = glyph;
  }
  return first < glyphCount &&
         fontValue(font, 24UL + (uint32_t)first * 28UL) == codepoint;
}

int16_t measureWithFace(const String &text, Size size, Face face) {
  const uint8_t *font = fontFor(size, face);
  const uint16_t glyphCount = (uint16_t)fontValue(font, 0);
  const int16_t spaceWidth = (int16_t)((fontValue(font, 16) + fontValue(font, 20)) * 2 / 7);
  int16_t result = 0;
  uint16_t offset = 0;
  uint32_t codepoint = 0;
  while (nextCodepoint(text, offset, codepoint)) {
    if (codepoint == '\r' || codepoint == '\n') continue;
    if (codepoint == ' ') { result += spaceWidth; continue; }
    result += glyphAdvance(font, glyphCount, codepoint, size, face, spaceWidth + 1);
  }
  return result;
}

String normalizeWithFace(const String &text, Size size, Face face) {
  const uint8_t *font = fontFor(size, face);
  const uint16_t glyphCount = (uint16_t)fontValue(font, 0);
  String result;
  result.reserve(text.length());
  uint16_t offset = 0;
  uint32_t codepoint = 0;
  while (nextCodepoint(text, offset, codepoint)) {
    switch (codepoint) {
      case 0x00a0: codepoint = ' '; break;
      case 0x2018:
      case 0x2019: codepoint = '\''; break;
      case 0x201c:
      case 0x201d: codepoint = '"'; break;
      case 0x2013:
      case 0x2014: codepoint = '-'; break;
      case 0x2026:
        result += "...";
        continue;
      default: break;
    }
    appendUtf8(result, hasGlyph(font, glyphCount, codepoint) ? codepoint : '?');
  }
  return result;
}

String ellipsizeWithFace(const String &text, int16_t maxWidth, Size size, Face face) {
  if (maxWidth <= 0) return "";
  if (measureWithFace(text, size, face) <= maxWidth) return text;
  const int16_t suffixWidth = measureWithFace("...", size, face);
  if (maxWidth < suffixWidth) return "";
  String result;
  uint16_t offset = 0;
  uint32_t codepoint = 0;
  while (nextCodepoint(text, offset, codepoint)) {
    String candidate = result;
    appendUtf8(candidate, codepoint);
    if (measureWithFace(candidate, size, face) + suffixWidth > maxWidth) break;
    result = candidate;
  }
  return result.length() ? result + "..." : "";
}
}  // namespace

void begin(TFT_eSPI &tft, Size size) {
  if (fontTft && fontTft != &tft) {
    fontTft->unloadFont();
    fontDepth = 0;
    fontTft = nullptr;
  }
  selectFont(tft, size);
  ++fontDepth;
}

void end(TFT_eSPI &tft) {
  if (fontTft != &tft || fontDepth == 0) return;
  if (--fontDepth == 0) {
    tft.unloadFont();
    fontTft = nullptr;
  }
}

int16_t lineHeight(Size size) {
  switch (size) {
    case Size::Compact: return 16;
    case Size::Title: return 27;
    default: return 21;
  }
}

int16_t measure(const String &text, Size size) { return measureWithFace(text, size, Face::Ui); }

int16_t width(TFT_eSPI &tft, const String &text, Size size) {
  const bool temporary = needsActivation(tft);
  if (temporary) begin(tft, size);
  else selectFont(tft, size);
  const int16_t result = tft.textWidth(text);
  if (temporary) end(tft);
  return result;
}

String ellipsize(const String &text, int16_t maxWidth, Size size) {
  return ellipsizeWithFace(text, maxWidth, size, Face::Ui);
}

String normalizeForFont(const String &text, Size size) {
  return normalizeWithFace(text, size, Face::Ui);
}

String normalizeAssistantCaption(const String &text) {
  return normalizeWithFace(text, Size::Content, assistantFace(text));
}

int16_t measureAssistantCaption(const String &text) {
  return measureWithFace(text, Size::Content, assistantFace(text));
}

String ellipsizeAssistantCaption(const String &text, int16_t maxWidth) {
  return ellipsizeWithFace(text, maxWidth, Size::Content, assistantFace(text));
}

void draw(TFT_eSPI &tft, const String &text, int16_t x, int16_t y,
          uint16_t color, uint16_t background, Size size, uint8_t datum) {
  const bool temporary = needsActivation(tft);
  if (temporary) begin(tft, size);
  else selectFont(tft, size);
  tft.setTextDatum(datum);
  tft.setTextColor(color, background, false);
  tft.drawString(text, x, y);
  if (temporary) end(tft);
  tft.setTextDatum(TL_DATUM);
}

void drawAssistantCaption(TFT_eSPI &tft, const String &text, int16_t x, int16_t y,
                          uint16_t color, uint16_t background, uint8_t datum) {
  const Face face = assistantFace(text);
  const bool temporary = needsActivation(tft);
  if (temporary) {
    selectFont(tft, Size::Content, face);
    fontDepth = 1;
  } else {
    selectFont(tft, Size::Content, face);
  }
  tft.setTextDatum(datum);
  tft.setTextColor(color, background, false);
  tft.drawString(text, x, y);
  if (temporary) end(tft);
  tft.setTextDatum(TL_DATUM);
}

}  // namespace UiText
