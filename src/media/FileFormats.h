#pragma once

#include <Arduino.h>

// Central registry for file formats understood by the application. Every
// format belongs to an image, audio, or video family when applicable. Custom cache
// formats retain that family, but are marked internal so callers do not treat
// cache files as source media.
class FileFormats {
public:
  enum class Group : uint8_t {
    Unknown,
    Image,
    Audio,
    Video
  };

  enum class Format : uint8_t {
    Unknown,
    Bmp,
    Png,
    Jpeg,
    Gif,
    Webp,
    Wav,
    Mp3,
    Mp4,
    ThreeGp,
    Em16,
    Emga,
    Empa,
    // Keep new values after the persisted source-format values above. ETH1
    // stores a FileFormats::Format byte in its version-1 header.
    Eth1
  };

  static Format fromPath(const String &path) {
    String value = path;
    int query = value.indexOf('?');
    if (query >= 0) value.remove(query);
    int dot = value.lastIndexOf('.');
    if (dot < 0) return Format::Unknown;
    String suffix = value.substring(dot);
    suffix.toLowerCase();
    if (suffix == F(".bmp")) return Format::Bmp;
    if (suffix == F(".png")) return Format::Png;
    if (suffix == F(".jpg") || suffix == F(".jpeg")) return Format::Jpeg;
    if (suffix == F(".gif")) return Format::Gif;
    if (suffix == F(".webp")) return Format::Webp;
    if (suffix == F(".wav")) return Format::Wav;
    if (suffix == F(".mp3")) return Format::Mp3;
    if (suffix == F(".mp4") || suffix == F(".m4v")) return Format::Mp4;
    if (suffix == F(".3gp") || suffix == F(".3gpp")) return Format::ThreeGp;
    if (suffix == F(".em16")) return Format::Em16;
    if (suffix == F(".emga")) return Format::Emga;
    if (suffix == F(".empa")) return Format::Empa;
    if (suffix == F(".eth1")) return Format::Eth1;
    return Format::Unknown;
  }

  static Format fromContentType(const String &contentType) {
    String type = contentType;
    int semicolon = type.indexOf(';');
    if (semicolon >= 0) type.remove(semicolon);
    type.trim();
    type.toLowerCase();
    if (type == F("image/bmp") || type == F("image/x-ms-bmp")) return Format::Bmp;
    if (type == F("image/png") || type == F("image/x-png")) return Format::Png;
    if (type == F("image/jpeg") || type == F("image/jpg") || type == F("image/pjpeg")) return Format::Jpeg;
    if (type == F("image/gif")) return Format::Gif;
    if (type == F("image/webp")) return Format::Webp;
    if (type == F("audio/wav") || type == F("audio/x-wav") || type == F("audio/wave") ||
        type == F("audio/vnd.wave") || type == F("audio/x-pn-wav")) return Format::Wav;
    if (type == F("audio/mpeg") || type == F("audio/mp3") || type == F("audio/x-mpeg")) return Format::Mp3;
    if (type == F("video/mp4") || type == F("application/mp4")) return Format::Mp4;
    if (type == F("video/3gpp") || type == F("video/3gp") || type == F("application/3gpp") ||
        type == F("application/x-3gp")) return Format::ThreeGp;
    return Format::Unknown;
  }

  static Format fromSignature(const uint8_t *bytes, size_t length) {
    if (!bytes) return Format::Unknown;
    if (length >= 4 && bytes[0] == 'E' && bytes[1] == 'M' && bytes[2] == '1' && bytes[3] == '6') return Format::Em16;
    if (length >= 4 && bytes[0] == 'E' && bytes[1] == 'M' && bytes[2] == 'G' && bytes[3] == 'A') return Format::Emga;
    if (length >= 4 && bytes[0] == 'E' && bytes[1] == 'M' && bytes[2] == 'P' && bytes[3] == 'A') return Format::Empa;
    if (length >= 4 && bytes[0] == 'E' && bytes[1] == 'T' && bytes[2] == 'H' && bytes[3] == '1') return Format::Eth1;
    if (length >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return Format::Bmp;
    if (length >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G' &&
        bytes[4] == 0x0d && bytes[5] == 0x0a && bytes[6] == 0x1a && bytes[7] == 0x0a) return Format::Png;
    if (length >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8) return Format::Jpeg;
    if (length >= 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == '8' &&
        (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a') return Format::Gif;
    if (length >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
        bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') return Format::Webp;
    if (length >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
        bytes[8] == 'W' && bytes[9] == 'A' && bytes[10] == 'V' && bytes[11] == 'E') return Format::Wav;
    if (length >= 3 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') return Format::Mp3;
    if (length >= 2 && bytes[0] == 0xff && (bytes[1] & 0xe0) == 0xe0) return Format::Mp3;
    // MP4 and 3GP are ISO base media files.  The first box is normally
    // `ftyp`; distinguish 3GPP brands so Browser can reject a response whose
    // declared type does not match its actual container.
    if (length >= 12 && bytes[4] == 'f' && bytes[5] == 't' && bytes[6] == 'y' && bytes[7] == 'p') {
      if (bytes[8] == '3' && bytes[9] == 'g' && (bytes[10] == 'p' || bytes[10] == '2')) return Format::ThreeGp;
      return Format::Mp4;
    }
    return Format::Unknown;
  }

  static Group group(Format format) {
    switch (format) {
      case Format::Bmp:
      case Format::Png:
      case Format::Jpeg:
      case Format::Gif:
      case Format::Webp:
      case Format::Em16:
      case Format::Emga:
      case Format::Eth1:
        return Group::Image;
      case Format::Wav:
      case Format::Mp3:
      case Format::Empa:
        return Group::Audio;
      case Format::Mp4:
      case Format::ThreeGp:
        return Group::Video;
      default:
        return Group::Unknown;
    }
  }

  static bool isDownloadable(Format format) {
    return !isInternal(format) && (isImage(format) || isAudio(format) || isVideo(format));
  }

  static bool isOpenable(Format format) {
    return isDownloadable(format);
  }

  static bool isImage(Format format) {
    return group(format) == Group::Image;
  }

  static bool isAudio(Format format) {
    return group(format) == Group::Audio;
  }

  static bool isVideo(Format format) {
    return group(format) == Group::Video;
  }

  static bool isInternal(Format format) {
    return format == Format::Em16 || format == Format::Emga || format == Format::Empa ||
           format == Format::Eth1;
  }

  static uint8_t internalCacheCount() { return 4; }

  static Format internalCacheAt(uint8_t index) {
    switch (index) {
      case 0: return Format::Eth1;
      case 1: return Format::Em16;
      case 2: return Format::Emga;
      case 3: return Format::Empa;
      default: return Format::Unknown;
    }
  }

  static const char *extension(Format format) {
    switch (format) {
      case Format::Bmp: return ".bmp";
      case Format::Png: return ".png";
      case Format::Jpeg: return ".jpg";
      case Format::Gif: return ".gif";
      case Format::Webp: return ".webp";
      case Format::Wav: return ".wav";
      case Format::Mp3: return ".mp3";
      case Format::Mp4: return ".mp4";
      case Format::ThreeGp: return ".3gp";
      case Format::Em16: return ".em16";
      case Format::Emga: return ".emga";
      case Format::Empa: return ".empa";
      case Format::Eth1: return ".eth1";
      default: return "";
    }
  }
};

static_assert((uint8_t)FileFormats::Format::Bmp == 1 &&
              (uint8_t)FileFormats::Format::Png == 2 &&
              (uint8_t)FileFormats::Format::Jpeg == 3 &&
              (uint8_t)FileFormats::Format::Gif == 4 &&
              (uint8_t)FileFormats::Format::Webp == 5,
              "ETH1 persists these source-format values");
static_assert((uint8_t)FileFormats::Format::Empa == 12 &&
              (uint8_t)FileFormats::Format::Eth1 == 13,
              "New cache formats must be appended");
