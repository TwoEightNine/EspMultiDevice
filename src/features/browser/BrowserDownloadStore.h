#pragma once

#include <Arduino.h>

#include "../../media/FileFormats.h"
#include "../../services/storage/DownloadStore.h"

class SdCardController;

// The Browser's `/Downloads` transaction on the shared `DownloadStore`, plus
// the Browser's own media-response classification and file naming.
// BrowserTransport owns HTTP framing, cancellation, and progress reporting
// and drives this store through one serialized transaction.
class BrowserDownloadStore : public DownloadStore {
public:
  explicit BrowserDownloadStore(SdCardController &sdCard);

  static FileFormats::Format classify(const String &contentType, const String &urlPath);
  static String deriveSuggestedName(const String &contentDisposition, const String &urlPath,
                                    FileFormats::Format format);

private:
  static constexpr uint16_t MaxNameBytes = 96;
};
