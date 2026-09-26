#pragma once

#include <Arduino.h>

#include "XiaozhiProtocol.h"

class SdCardController;

// Worker-owned, normal-storage-only tools exposed to the authenticated
// assistant session. This class never navigates or paints; the session turns
// an accepted playback request into a UI event after returning the RPC result.
class AssistantDeviceTools {
public:
  static constexpr uint8_t MaxMatches = 5;
  static constexpr size_t MaxQueryBytes = 96;
  static constexpr size_t MaxPathBytes = 256;
  static constexpr uint32_t SelectionLifetimeMs = 120000;
  static constexpr uint32_t SearchDeadlineMs = 8000;

  explicit AssistantDeviceTools(SdCardController &sdCard);

  XiaozhiProtocol::DeviceToolResult searchAudio(const char *query);
  XiaozhiProtocol::DeviceToolResult playAudio(uint8_t selectionId, String &path);
  XiaozhiProtocol::DeviceToolResult search(const char *query,
                                          XiaozhiProtocol::DeviceCommandKind kind,
                                          bool (*cancelled)(void *) = nullptr,
                                          void *cancelContext = nullptr);
  XiaozhiProtocol::DeviceToolResult selected(uint8_t selectionId,
                                            XiaozhiProtocol::DeviceCommandKind kind,
                                            String &path);
  void clearSelections();

private:
  struct Selection {
    String path;
    uint16_t score;
    bool used;
    bool directory;
  };

  SdCardController &_sdCard;
  Selection _selections[MaxMatches];
  uint32_t _selectionExpiresAt;
  uint32_t _scanDeadlineMs;
  bool _scanBusy;
  bool (*_scanCancelled)(void *);
  void *_scanCancelContext;
  XiaozhiProtocol::DeviceCommandKind _searchKind;

  bool scanDirectory(const String &path, uint8_t depth, const String &query,
                     XiaozhiProtocol::DeviceToolResult &result);
  void considerPath(const String &path, bool directory, const String &query,
                    XiaozhiProtocol::DeviceToolResult &result);
  void insertMatch(const String &path, bool directory, uint16_t score,
                   XiaozhiProtocol::DeviceToolResult &result);
  bool selectionValid(uint8_t selectionId) const;
  static bool hiddenOrPrivate(const String &path);
  static String baseName(const String &path);
  static String normalizedName(const String &value);
  static uint16_t scoreMatch(const String &name, const String &query);
  static void copyDisplayName(const String &path, char *output, size_t outputBytes);
  static bool before(const Selection &left, const Selection &right);
};
