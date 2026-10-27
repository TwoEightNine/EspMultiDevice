#include "AppTextLayout.h"

namespace AppTextLayout {
namespace {
uint8_t sequenceLength(uint8_t lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;  // A stray continuation byte measures as one replacement glyph.
}

int16_t measure(const char *bytes, uint8_t count, UiText::Size size) {
  char glyph[5] = {};
  memcpy(glyph, bytes, count);
  return UiText::measure(String(glyph), size);
}
}

uint16_t wrap(const char *text, size_t length, int16_t width, UiText::Size size,
              Line *lines, uint16_t maxLines, size_t maxLineBytes, bool &more) {
  uint16_t count = 0;
  size_t position = 0;
  while (position < length && text[position] == ' ') ++position;
  while (position < length && count < maxLines) {
    if (text[position] == '\n') {
      lines[count++] = {static_cast<uint32_t>(position), 0};
      ++position;
      continue;
    }
    const size_t start = position;
    size_t index = position;
    size_t lastSpace = 0;
    bool hasSpace = false;
    int32_t lineWidth = 0;
    size_t end = length;
    size_t next = length;
    while (index < length) {
      const char c = text[index];
      if (c == '\n') { end = index; next = index + 1; break; }
      uint8_t sequence = sequenceLength(static_cast<uint8_t>(c));
      if (index + sequence > length) sequence = static_cast<uint8_t>(length - index);
      const int16_t glyph = measure(text + index, sequence, size);
      if (index > start && (index + sequence - start > maxLineBytes || lineWidth + glyph > width)) {
        if (hasSpace) { end = lastSpace; next = lastSpace + 1; }
        else { end = index; next = index; }
        break;
      }
      if (c == ' ') { lastSpace = index; hasSpace = true; }
      lineWidth += glyph;
      index += sequence;
    }
    if (index >= length) { end = length; next = length; }
    while (end > start && text[end - 1] == ' ') --end;
    lines[count++] = {static_cast<uint32_t>(start), static_cast<uint32_t>(end - start)};
    position = next;
    while (position < length && text[position] == ' ') ++position;
  }
  size_t rest = position;
  while (rest < length && (text[rest] == ' ' || text[rest] == '\n')) ++rest;
  more = rest < length;
  return count;
}

}  // namespace AppTextLayout
