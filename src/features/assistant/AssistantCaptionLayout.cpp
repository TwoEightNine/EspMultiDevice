#include "AssistantCaptionLayout.h"

#include "../../services/assistant/AssistantState.h"
#include "../../ui/UiText.h"

namespace {
String normalizedCaption(const String &text) {
  return UiText::normalizeAssistantCaption(text);
}

String singleLineTail(const String &text, uint16_t position) {
  String result = text.substring(position);
  result.replace('\r', ' ');
  result.replace('\n', ' ');
  result.trim();
  return result;
}
}  // namespace

uint16_t AssistantCaptionLayout::nextUtf8Offset(const String &text,
                                                uint16_t offset) {
  if (offset >= text.length()) return offset;
  const uint8_t first = static_cast<uint8_t>(text[offset]);
  uint8_t bytes = first < 0x80 ? 1 :
                  (first >= 0xc2 && first <= 0xdf ? 2 :
                  (first >= 0xe0 && first <= 0xef ? 3 :
                  (first >= 0xf0 && first <= 0xf4 ? 4 : 1)));
  if (offset + bytes > text.length()) bytes = 1;
  return offset + bytes;
}

bool AssistantCaptionLayout::nextLine(const String &text, uint16_t &position,
                                      String &output) {
  output = "";
  while (position < text.length() && text[position] == ' ') ++position;
  if (position >= text.length()) return false;
  const uint16_t start = position;
  uint16_t end = position;
  int32_t lastSpace = -1;
  while (end < text.length()) {
    if (text[end] == '\r') { ++end; continue; }
    if (text[end] == '\n') {
      output = text.substring(start, end);
      output.trim();
      position = end + 1;
      return true;
    }
    if (text[end] == ' ') lastSpace = end;
    const uint16_t next = nextUtf8Offset(text, end);
    if (UiText::measureAssistantCaption(text.substring(start, next)) > TextWidth) {
      uint16_t split = lastSpace >= static_cast<int32_t>(start)
          ? static_cast<uint16_t>(lastSpace) : end;
      if (split == start) split = next;
      output = text.substring(start, split);
      output.trim();
      position = lastSpace >= static_cast<int32_t>(start)
          ? static_cast<uint16_t>(lastSpace + 1) : split;
      return true;
    }
    end = next;
  }
  output = text.substring(start);
  output.trim();
  position = text.length();
  return true;
}

uint16_t AssistantCaptionLayout::countLines(const String &text) {
  const String normalized = normalizedCaption(text);
  uint16_t position = 0;
  uint16_t count = 0;
  String line;
  while (nextLine(normalized, position, line)) {
    if (count != UINT16_MAX) ++count;
  }
  return count;
}

uint8_t AssistantCaptionLayout::copyWindow(const String &text,
                                           uint16_t firstLine, String *output,
                                           uint8_t capacity) {
  if (!output || capacity == 0) return 0;
  for (uint8_t index = 0; index < capacity; ++index) output[index] = "";
  const String normalized = normalizedCaption(text);
  uint16_t position = 0;
  uint16_t lineIndex = 0;
  uint8_t copied = 0;
  String line;
  while (nextLine(normalized, position, line)) {
    if (lineIndex >= firstLine && copied < capacity) output[copied++] = line;
    if (copied == capacity) break;
    if (lineIndex != UINT16_MAX) ++lineIndex;
  }
  return copied;
}

uint8_t AssistantCaptionLayout::copyHuman(const String &text, String *output) {
  if (!output) return 0;
  output[0] = "";
  output[1] = "";
  const String normalized = normalizedCaption(text);
  uint16_t position = 0;
  if (!nextLine(normalized, position, output[0])) return 0;

  const uint16_t secondStart = position;
  if (!nextLine(normalized, position, output[1])) return 1;
  String remaining;
  if (nextLine(normalized, position, remaining)) {
    output[1] = UiText::ellipsizeAssistantCaption(
        singleLineTail(normalized, secondStart), TextWidth);
  }
  return 2;
}

uint16_t AssistantCaptionLayout::assistantPageStart(uint16_t lineCount,
                                                    uint32_t startedMs,
                                                    uint32_t nowMs) {
  if (lineCount <= AssistantLineCapacity || startedMs == 0) return 0;
  const uint32_t pages = (nowMs - startedMs) / AssistantPageIntervalMs;
  const uint32_t requested = pages * AssistantLineCapacity;
  const uint16_t lastStart = lineCount - AssistantLineCapacity;
  return requested >= lastStart ? lastStart : static_cast<uint16_t>(requested);
}

AssistantCaptionLayout::VoiceAction AssistantCaptionLayout::voiceAction(
    uint8_t phaseValue, bool micAvailable, bool resettingConversation) {
  const AssistantPhase phase = static_cast<AssistantPhase>(phaseValue);
  if (resettingConversation) return VoiceAction::Unavailable;
  if (phase == AssistantPhase::Listening) return VoiceAction::Stop;
  switch (phase) {
    case AssistantPhase::Bootstrapping:
    case AssistantPhase::NeedsActivation:
    case AssistantPhase::Connecting:
    case AssistantPhase::Thinking:
    case AssistantPhase::Speaking:
      return VoiceAction::Cancel;
    case AssistantPhase::Ready:
      return micAvailable ? VoiceAction::Microphone : VoiceAction::Unavailable;
    default:
      return VoiceAction::Unavailable;
  }
}
