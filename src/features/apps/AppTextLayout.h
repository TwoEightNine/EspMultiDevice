#pragma once

#include <Arduino.h>

#include "../../ui/UiText.h"

// Word wrapping with the UI fonts, shared by the canvas `text_box` command and
// `esp.ui.wrap`. Measurement is heap-free, so the app worker may call it while
// the UI task draws.
namespace AppTextLayout {

struct Line {
  uint32_t start;
  uint32_t length;
};

// Breaks `text` at spaces (or inside a word too wide for one line) and at
// newlines into at most `maxLines` lines, each no wider than `width` pixels
// and no longer than `maxLineBytes`. Lines never split a UTF-8 sequence and
// carry no leading or trailing spaces. `more` reports text left over.
uint16_t wrap(const char *text, size_t length, int16_t width, UiText::Size size,
              Line *lines, uint16_t maxLines, size_t maxLineBytes, bool &more);

}  // namespace AppTextLayout
