#pragma once

#include <Arduino.h>

#include "AssistantState.h"

struct AssistantTranscriptTurn {
  static constexpr size_t UserBytes = AssistantSnapshot::CaptionBytes;
  static constexpr size_t AssistantBytes = AssistantSnapshot::CaptionBytes * 2;

  char user[UserBytes + 1];
  char assistant[AssistantBytes + 1];
};

class AssistantTranscript {
public:
  static constexpr uint8_t Capacity = 5;

  AssistantTranscript();
  ~AssistantTranscript();

  bool begin();
  void end();
  void clear();
  void beginDraft();
  void setUser(const char *text);
  void appendAssistant(const char *text);
  bool completeDraft();
  void discardDraft();

  uint8_t count() const;
  uint32_t revision() const;
  bool copyTurn(uint8_t index, AssistantTranscriptTurn &output) const;
  bool copyDraft(AssistantTranscriptTurn &output) const;

private:
  AssistantTranscriptTurn *_turns;
  uint8_t _count;
  bool _draftActive;
  uint32_t _revision;

  AssistantTranscriptTurn &draft();
  const AssistantTranscriptTurn &draft() const;
  static void clearTurn(AssistantTranscriptTurn &turn);
  static void copyUtf8(const char *input, char *output, size_t capacity);
  static void appendUtf8(const char *input, char *output, size_t capacity);
};
