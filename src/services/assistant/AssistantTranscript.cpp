#include "AssistantTranscript.h"

#include "XiaozhiProtocol.h"

#include <esp_heap_caps.h>
#include <string.h>

AssistantTranscript::AssistantTranscript()
    : _turns(nullptr), _count(0), _draftActive(false), _revision(0) {}

AssistantTranscript::~AssistantTranscript() { end(); }

bool AssistantTranscript::begin() {
  if (_turns) return true;
  _turns = static_cast<AssistantTranscriptTurn *>(heap_caps_calloc(
      Capacity + 1, sizeof(AssistantTranscriptTurn),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_turns) return false;
  _count = 0;
  _draftActive = false;
  ++_revision;
  return true;
}

void AssistantTranscript::end() {
  if (!_turns) return;
  memset(_turns, 0, sizeof(AssistantTranscriptTurn) * (Capacity + 1));
  free(_turns);
  _turns = nullptr;
  _count = 0;
  _draftActive = false;
  ++_revision;
}

void AssistantTranscript::clear() {
  if (_turns) memset(_turns, 0, sizeof(AssistantTranscriptTurn) * (Capacity + 1));
  _count = 0;
  _draftActive = false;
  ++_revision;
}

void AssistantTranscript::beginDraft() {
  if (!_turns || _draftActive) return;
  clearTurn(draft());
  _draftActive = true;
  ++_revision;
}

void AssistantTranscript::setUser(const char *text) {
  if (!_turns) return;
  if (!_draftActive) beginDraft();
  copyUtf8(text, draft().user, sizeof(draft().user));
  ++_revision;
}

void AssistantTranscript::appendAssistant(const char *text) {
  if (!_turns) return;
  if (!_draftActive) beginDraft();
  appendUtf8(text, draft().assistant, sizeof(draft().assistant));
  ++_revision;
}

bool AssistantTranscript::completeDraft() {
  if (!_turns || !_draftActive || !draft().user[0] || !draft().assistant[0]) {
    discardDraft();
    return false;
  }
  if (_count == Capacity) {
    memmove(&_turns[0], &_turns[1],
            sizeof(AssistantTranscriptTurn) * (Capacity - 1));
    _count = Capacity - 1;
  }
  _turns[_count++] = draft();
  clearTurn(draft());
  _draftActive = false;
  ++_revision;
  return true;
}

void AssistantTranscript::discardDraft() {
  if (!_turns || !_draftActive) return;
  clearTurn(draft());
  _draftActive = false;
  ++_revision;
}

uint8_t AssistantTranscript::count() const { return _count; }
uint32_t AssistantTranscript::revision() const { return _revision; }

bool AssistantTranscript::copyTurn(uint8_t index,
                                   AssistantTranscriptTurn &output) const {
  clearTurn(output);
  if (!_turns || index >= _count) return false;
  output = _turns[index];
  return true;
}

bool AssistantTranscript::copyDraft(AssistantTranscriptTurn &output) const {
  clearTurn(output);
  if (!_turns || !_draftActive) return false;
  output = draft();
  return true;
}

AssistantTranscriptTurn &AssistantTranscript::draft() { return _turns[Capacity]; }
const AssistantTranscriptTurn &AssistantTranscript::draft() const {
  return _turns[Capacity];
}

void AssistantTranscript::clearTurn(AssistantTranscriptTurn &turn) {
  memset(&turn, 0, sizeof(turn));
}

void AssistantTranscript::copyUtf8(const char *input, char *output,
                                   size_t capacity) {
  XiaozhiProtocol::copyUtf8Bounded(input, output, capacity);
}

void AssistantTranscript::appendUtf8(const char *input, char *output,
                                     size_t capacity) {
  if (!input || !input[0] || !output || capacity < 2) return;
  const size_t used = strnlen(output, capacity);
  if (used >= capacity - 1) return;
  size_t offset = used;
  if (offset && output[offset - 1] != ' ' && output[offset - 1] != '\n') {
    output[offset++] = ' ';
    output[offset] = 0;
  }
  if (offset >= capacity - 1) return;
  XiaozhiProtocol::copyUtf8Bounded(input, output + offset, capacity - offset);
}
