#pragma once

#include <Arduino.h>

// Browser resource ceilings are intentionally centralized.  They are part of
// the browser's compatibility and memory contract, not tuning knobs owned by
// an individual screen or worker.
namespace BrowserLimits {
constexpr uint8_t MaxRecentSites = 16;
constexpr uint8_t MaxCookies = 16;
constexpr uint16_t MaxRows = 192;
constexpr uint8_t MaxRunsPerRow = 16;
constexpr uint8_t MaxLinks = 64;
constexpr uint8_t MaxForms = 8;
constexpr uint8_t MaxFormControls = 32;
constexpr uint8_t MaxFormOptions = 80;
constexpr uint8_t MaxHistory = 8;
constexpr uint8_t MaxSessionPages = MaxHistory + 1;
constexpr uint8_t MaxSessionImages = 48;
constexpr uint8_t MaxStyleDepth = 16;
constexpr uint8_t MaxCssRules = 32;
constexpr uint8_t MaxCssSelectorLength = 48;
constexpr uint16_t MaxCssDeclarationLength = 256;
constexpr uint8_t MaxPages = 128;
constexpr uint16_t MaxEmbeddedImages = 48;
constexpr uint8_t ImageJobQueueLength = 4;
constexpr uint8_t LifecycleJobQueueLength = 1;
constexpr uint16_t ImageUrlMaxLength = 384;
constexpr uint32_t MaxBodyBytes = 512UL * 1024UL;
constexpr uint16_t MaxFormPostBytes = 8UL * 1024UL;
constexpr uint32_t MaxDownloadBytes = 16UL * 1024UL * 1024UL;
constexpr uint32_t MaxSessionCacheBytes = 16UL * 1024UL * 1024UL;
// Transfers are deliberately bounded.  The values below are the default
// high-throughput candidates for the N16R8 profile; benchmark alternatives
// on the physical SD card before changing them.
constexpr uint16_t TransferReadBufferBytes = 8192;
constexpr uint16_t SdWriteChunkBytes = 4096;
constexpr uint16_t CacheIoChunkBytes = 4096;
constexpr uint16_t FallbackTransferReadBufferBytes = 512;
constexpr uint16_t MaxHttpLineBytes = 2048;
constexpr uint8_t MaxResponseHeaderLines = 64;
constexpr uint32_t ConnectTimeoutMs = 10000;
constexpr uint8_t TlsRetryHandshakeTimeoutSeconds = 15;
constexpr uint32_t NetworkLockTimeoutMs = 2000;
constexpr uint8_t MaxRedirects = 5;
constexpr uint32_t MaxEmbeddedImageBytes = 384UL * 1024UL;
}  // namespace BrowserLimits
