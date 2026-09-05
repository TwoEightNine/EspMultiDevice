#pragma once

#include <Arduino.h>

#include "BrowserLimits.h"
#include "BrowserText.h"

namespace Browser {

struct Rect { int16_t x; int16_t y; int16_t w; int16_t h; };

struct Url { String scheme; String host; String path; uint16_t port; };

enum class HttpMethod : uint8_t { Get, Post };

struct HttpRequest { HttpMethod method; String url; String body; };
struct RecentSite { String url; String title; };

struct LoadOptions {
  String userAgent;
  bool useSessionCache = false;
  uint32_t bodyLimit = 0;
  uint32_t downloadLimit = 0;
};

enum class PageLoadOutcome : uint8_t { Text, Download, Error };
enum class PageLoadErrorKind : uint8_t {
  None, InvalidRequest, Transport, TextEncoding, UnsupportedContent, Cancelled
};

struct PageLoadResult {
  PageLoadOutcome outcome = PageLoadOutcome::Error;
  PageLoadErrorKind errorKind = PageLoadErrorKind::None;
  String finalUrl;
  String contentType;
  String textBody;
  String downloadPath;
  String error;
  bool fromSessionCache = false;
  bool cancelled = false;

  bool succeeded() const {
    return outcome != PageLoadOutcome::Error && !cancelled && error.length() == 0;
  }
};

struct PageLoadProgress {
  String phase;
  uint32_t bytesReceived = 0;
  int32_t totalBytes = -1;
};

class CancellationCheck {
public:
  virtual ~CancellationCheck() = default;
  virtual bool cancelled() const = 0;
};

struct Cookie {
  String name;
  String value;
  String domain;
  String path;
  bool secure;
  uint64_t expiresAt;
};

struct TextStyle {
  uint16_t color;
  uint16_t backgroundColor;
  bool hasBackground;
  uint16_t blockBackgroundColor;
  bool hasBlockBackground;
  bool bold;
  bool italic;
  bool underline;
  bool link;
  uint8_t linkIndex;
  bool fullWidthImage;
  BrowserText::Size size = BrowserText::Size::Normal;
};

struct StyleOverride {
  bool hasColor;
  uint16_t color;
  bool hasBackground;
  bool backgroundEnabled;
  uint16_t backgroundColor;
  bool hasBold;
  bool bold;
  bool hasItalic;
  bool italic;
  bool hasUnderline;
  bool underline;
  bool hasFullWidthImage;
  bool fullWidthImage;
  bool hasSize;
  BrowserText::Size size;
};

struct CssRule {
  String selector;
  StyleOverride style;
  uint16_t sourceOrder;
  uint8_t specificity;
  uint8_t selectorType;
};

struct VisualRun {
  bool image;
  String text;
  TextStyle style;
  int16_t imageIndex;
  int16_t x;
  int16_t width;
};

struct VisualRow {
  bool image;
  int8_t formControlIndex;
  String imageLabel;
  bool imageLink;
  uint8_t imageLinkIndex;
  int16_t imageIndex;
  TextStyle style;
  int16_t contentWidth;
  uint8_t runCount;
  VisualRun runs[BrowserLimits::MaxRunsPerRow];
};

enum class FormControlKind : uint8_t { Text, Textarea, Checkbox, Radio, Select, Submit, Reset, Unsupported };
enum class FormMethod : uint8_t { Get, Post, Unsupported };
struct FormData { String action; FormMethod method; bool urlEncoded; };
struct FormControl {
  FormControlKind kind;
  int8_t formIndex;
  String label;
  bool labelIsPlaceholder;
  String name;
  String id;
  String value;
  String defaultValue;
  bool checked;
  bool defaultChecked;
  bool disabled;
  uint8_t firstOption;
  uint8_t optionCount;
  uint8_t selectedOption;
  uint8_t defaultSelectedOption;
};
struct FormOption { uint8_t controlIndex; String label; String value; };

struct EmbeddedImage {
  uint16_t *pixels;
  uint8_t width;
  uint8_t height;
  uint16_t backgroundColor;
  char url[BrowserLimits::ImageUrlMaxLength];
  uint32_t generation;
  uint8_t state;
};
struct ImageJob {
  uint16_t imageIndex;
  uint32_t generation;
  uint16_t backgroundColor;
  bool useSessionCache;
  char url[BrowserLimits::ImageUrlMaxLength];
};
struct ImageResult { uint16_t imageIndex; uint32_t generation; uint16_t *pixels; uint8_t width; uint8_t height; };

struct SessionPageCacheEntry { String url; String contentType; String path; uint32_t bytes; uint32_t lastUse; bool used; };
struct SessionImageCacheEntry { String url; String path; uint32_t bytes; uint32_t lastUse; bool used; };
struct LifecycleJob { uint32_t generation; };
struct StyleFrame { String tag; TextStyle style; };

enum class Action : uint8_t {
  None, Close, BackOrCancel, Home, OpenUrlInput, OpenUrl, ToggleFavorite,
  ToggleUserAgent, PreviousPage, NextPage, EditForm, SubmitForm, ResetForm
};

struct ActionResult { Action action; int16_t index; String url; };
}  // namespace Browser
