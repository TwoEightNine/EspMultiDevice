#pragma once

#include <Arduino.h>
#include <FS.h>

#include "../../media/FileFormats.h"

class SdCardController;

// Owns the filesystem policy and transaction state for Browser downloads.
// BrowserTransport owns HTTP framing, cancellation, and progress reporting
// and drives this store through one serialized transaction.
class BrowserDownloadStore {
public:
  explicit BrowserDownloadStore(SdCardController &sdCard);
  ~BrowserDownloadStore();

  static FileFormats::Format classify(const String &contentType, const String &urlPath);
  static String deriveSuggestedName(const String &contentDisposition, const String &urlPath,
                                    FileFormats::Format format);

  bool begin(FileFormats::Format format, const String &suggestedName, int32_t declaredSize,
             uint32_t byteLimit, String &error);
  bool write(const uint8_t *bytes, size_t length, String &error);
  bool commit(String &finalPath, String &error);
  void abort();
  bool discard(const String &path);

private:
  static constexpr uint16_t WriteChunkBytes = 512;
  static constexpr uint16_t MaxNameBytes = 96;
  static constexpr uint16_t MaxNameAttempts = 1000;

  SdCardController &_sdCard;
  fs::File _file;
  FileFormats::Format _format;
  String _finalPath;
  String _temporaryPath;
  uint32_t _byteLimit;
  uint32_t _bytesWritten;
  bool _active;

  bool validateTemporaryFile();
  void reset();
};
