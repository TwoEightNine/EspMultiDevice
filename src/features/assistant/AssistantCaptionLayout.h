#pragma once

#include <Arduino.h>

// Heap-light caption layout for AssistantScreen's two fixed background-owned
// bubbles. It keeps every UTF-8 boundary intact and uses the same CJK-aware
// measurement as the screen renderer.
class AssistantCaptionLayout {
public:
  enum class VoiceAction : uint8_t { Microphone, Stop, Cancel, Unavailable };

  static constexpr int16_t BackgroundTop = 48;
  static constexpr int16_t HumanX = 22;
  static constexpr int16_t HumanY = BackgroundTop + 21;
  static constexpr int16_t HumanWidth = 276;
  static constexpr int16_t HumanHeight = 48;
  static constexpr int16_t AssistantX = 22;
  static constexpr int16_t AssistantY = BackgroundTop + 100;
  static constexpr int16_t AssistantWidth = 276;
  static constexpr int16_t AssistantHeight = 72;
  static constexpr int16_t VoiceControlX = 130;
  static constexpr int16_t VoiceControlY = 396;
  static constexpr int16_t VoiceControlSize = 60;
  static constexpr int16_t TextWidth = 260;
  static constexpr uint8_t HumanLineCapacity = 2;
  static constexpr uint8_t AssistantLineCapacity = 3;
  static constexpr uint32_t AssistantPageIntervalMs = 2000;

  static uint16_t countLines(const String &text);
  static uint8_t copyWindow(const String &text, uint16_t firstLine,
                            String *output, uint8_t capacity);
  static uint8_t copyHuman(const String &text, String *output);
  static uint16_t assistantPageStart(uint16_t lineCount, uint32_t startedMs,
                                     uint32_t nowMs);
  static VoiceAction voiceAction(uint8_t phase, bool micAvailable,
                                 bool resettingConversation);

private:
  static bool nextLine(const String &text, uint16_t &position, String &output);
  static uint16_t nextUtf8Offset(const String &text, uint16_t offset);
};
