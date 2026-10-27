#include "BrowserDownloadStore.h"

#include <ctype.h>
#include <stdlib.h>

#include "BrowserLimits.h"

namespace {
constexpr const char *DownloadDirectory = "/Downloads";

String trimCopy(const String &value) {
  String result = value;
  result.trim();
  return result;
}

String lowerCopy(const String &value) {
  String result = value;
  result.toLowerCase();
  return result;
}
}  // namespace

BrowserDownloadStore::BrowserDownloadStore(SdCardController &sdCard)
    : DownloadStore(sdCard, DownloadDirectory, BrowserLimits::MaxDownloadBytes) {}

FileFormats::Format BrowserDownloadStore::classify(const String &contentType,
                                                   const String &urlPath) {
  String type = lowerCopy(trimCopy(contentType));
  const int semicolon = type.indexOf(';');
  if (semicolon >= 0) type.remove(semicolon);
  type.trim();

  FileFormats::Format format = FileFormats::fromContentType(type);
  if (FileFormats::isDownloadable(format)) return format;
  if (type.length() == 0 || type == F("application/octet-stream")) {
    format = FileFormats::fromPath(urlPath);
    if (FileFormats::isDownloadable(format)) return format;
  }
  return FileFormats::Format::Unknown;
}

String BrowserDownloadStore::deriveSuggestedName(const String &contentDisposition,
                                                  const String &urlPath,
                                                  FileFormats::Format format) {
  String candidate;
  const String lowerDisposition = lowerCopy(contentDisposition);
  int position = lowerDisposition.indexOf(F("filename*="));
  if (position >= 0) {
    candidate = trimCopy(contentDisposition.substring(position + 10));
    const int separator = candidate.indexOf(';');
    if (separator >= 0) candidate.remove(separator);
    if (candidate.length() >= 2 && candidate[0] == '"' && candidate[candidate.length() - 1] == '"') {
      candidate = candidate.substring(1, candidate.length() - 1);
    }
    const int charsetEnd = candidate.indexOf('\'');
    const int languageEnd = charsetEnd < 0 ? -1 : candidate.indexOf('\'', charsetEnd + 1);
    if (languageEnd >= 0) candidate = candidate.substring(languageEnd + 1);
  }
  if (!candidate.length()) {
    position = lowerDisposition.indexOf(F("filename="));
    if (position >= 0) {
      candidate = trimCopy(contentDisposition.substring(position + 9));
      const int separator = candidate.indexOf(';');
      if (separator >= 0) candidate.remove(separator);
      if (candidate.length() >= 2 && candidate[0] == '"' && candidate[candidate.length() - 1] == '"') {
        candidate = candidate.substring(1, candidate.length() - 1);
      }
    }
  }
  if (!candidate.length()) {
    candidate = urlPath;
    const int query = candidate.indexOf('?');
    if (query >= 0) candidate.remove(query);
    const int slash = candidate.lastIndexOf('/');
    if (slash >= 0) candidate = candidate.substring(slash + 1);
  }

  String decoded;
  for (uint16_t index = 0; index < candidate.length(); ++index) {
    if (candidate[index] == '%' && index + 2 < candidate.length() &&
        isxdigit(static_cast<unsigned char>(candidate[index + 1])) &&
        isxdigit(static_cast<unsigned char>(candidate[index + 2]))) {
      char encoded[] = {candidate[index + 1], candidate[index + 2], '\0'};
      decoded += static_cast<char>(strtoul(encoded, nullptr, 16));
      index += 2;
    } else {
      decoded += candidate[index];
    }
  }

  String safe;
  for (uint16_t index = 0; index < decoded.length() && safe.length() < MaxNameBytes; ++index) {
    const uint8_t character = static_cast<uint8_t>(decoded[index]);
    if (character < 0x20 || character == 0x7f || character == '/' || character == '\\' ||
        character == ':' || character == '*' || character == '?' || character == '"' ||
        character == '<' || character == '>' || character == '|') continue;
    safe += static_cast<char>(character);
  }
  safe.trim();
  while (safe.endsWith(F("."))) safe.remove(safe.length() - 1);
  if (!safe.length() || safe == F(".") || safe == F("..")) return "";

  if (FileFormats::fromPath(safe) != format) {
    const int dot = safe.lastIndexOf('.');
    if (dot > 0) safe.remove(dot);
    safe += FileFormats::extension(format);
  }
  return safe;
}
