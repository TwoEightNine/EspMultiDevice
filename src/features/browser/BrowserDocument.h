#pragma once

#include <Arduino.h>

#include "BrowserTypes.h"

// Layout is deliberately supplied by the UI coordinator.  Document parsing
// and pagination must remain independent of the TFT and screen classes.
struct BrowserDocumentLayout {
  int16_t viewportWidth;
  int16_t documentTop;
  int16_t documentBottom;
  int16_t sidePadding;
  int16_t textRowHeight;
  int16_t formControlHeight;
  int16_t formTextareaHeight;
  int16_t imagePlaceholderWidth;
};

// Parsing receives URL resolution as a callback rather than depending on the
// transport implementation. The callback must return a normalized HTTP(S)
// URL and leave `resolved` empty on failure.
using BrowserDocumentUrlResolver = bool (*)(void *context, const String &reference, String &resolved);

struct BrowserDocumentContext {
  uint32_t imageGeneration;
  void *resolverContext;
  BrowserDocumentUrlResolver resolveUrl;
};

// Bounded parsed-document model. All mutation is UI-task-only. Embedded-image
// pixels are borrowed from BrowserImageWorker; this component stores only the
// accepted metadata and pointer needed for layout and drawing.
class BrowserDocument {
public:
  BrowserDocument();
  void clear();
  // Initializes one bounded HTML parse. The caller owns any prior thumbnail
  // release; this method owns only document records and parse context.
  void parseHtml(const String &html, const BrowserDocumentContext &context,
                 const BrowserDocumentLayout &layout);
  void parsePlainText(const String &text, const String &title, const BrowserDocumentLayout &layout,
                      uint16_t textColor, uint16_t backgroundColor);

  // Layout and interaction behavior. These are intentionally usable without
  // display, network, filesystem, FreeRTOS, or Navigator dependencies.
  void reflow(const BrowserDocumentLayout &layout);
  uint8_t pageCount() const { return _pageCount; }
  uint16_t pageStart(uint8_t page) const;
  uint16_t pageEnd(uint8_t page) const;
  int16_t rowHeight(uint16_t rowIndex, const BrowserDocumentLayout &layout) const;
  String linkAt(uint8_t page, int16_t x, int16_t y, const BrowserDocumentLayout &layout) const;
  int8_t formAt(uint8_t page, int16_t x, int16_t y, const BrowserDocumentLayout &layout) const;
  bool imageVisibleOnPage(uint8_t page, uint16_t imageIndex) const;

  bool setFormValue(uint8_t controlIndex, const String &value);
  bool resetForm(uint8_t formIndex);
  bool toggleFormControl(uint8_t controlIndex);
  bool buildFormPayload(uint8_t formIndex, int8_t submitControlIndex, String &payload,
                        uint16_t maxBytes = 0) const;
  String formControlLabel(uint8_t controlIndex) const;
  String formControlValue(uint8_t controlIndex) const;

  // Accepts a worker-owned buffer as a borrowed UI-task rendering reference.
  bool setImageResult(uint16_t imageIndex, uint32_t generation, uint16_t *pixels,
                      uint8_t width, uint8_t height);

  // Read-only rendering access. Keeping these records fixed-size avoids a
  // second document representation while BrowserView is still Screen-owned.
  const Browser::VisualRow *row(uint16_t index) const;
  const String *link(uint8_t index) const;
  const Browser::FormData *form(uint8_t index) const;
  const Browser::FormControl *formControl(uint8_t index) const;
  const Browser::FormOption *formOption(uint8_t index) const;
  const Browser::EmbeddedImage *image(uint16_t index) const;
  uint16_t rowCount() const { return _rowCount; }
  uint8_t formCount() const { return _formCount; }
  uint8_t formControlCount() const { return _formControlCount; }
  uint8_t formOptionCount() const { return _formOptionCount; }
  uint8_t linkCount() const { return _linkCount; }
  uint16_t imageCount() const { return _embeddedImageCount; }
  uint16_t backgroundColor() const { return _documentBackgroundColor; }
  const String &title() const { return _titleBuffer; }

  // Worker-facing records are copied by value. No task receives a document
  // pointer or a mutable descriptor reference.
  bool beginImageJob(uint16_t imageIndex, uint32_t generation, bool useSessionCache,
                     Browser::ImageJob &job);
  bool returnImageJobToPending(uint16_t imageIndex, uint32_t generation);
  bool hasUnfinishedImages(uint32_t generation) const;
  bool markImageFailed(uint16_t imageIndex, uint32_t generation);
  void clearImageDescriptors();

private:
  using TextStyle = Browser::TextStyle;
  using StyleOverride = Browser::StyleOverride;
  using CssRule = Browser::CssRule;

  void collectCssRules(const String &html);
  void skipCssSpaceAndComments(const String &source, uint32_t &position, uint32_t end) const;
  bool cssSlice(const String &source, uint32_t start, uint32_t end, uint16_t limit, String &result) const;
  void parseCssRules(const String &source, uint32_t start, uint32_t end);
  bool addCssRule(const String &selector, const StyleOverride &style, uint16_t sourceOrder);
  bool parseCssDeclarations(const String &declarations, StyleOverride &style) const;
  bool parseBackground(const String &value, uint16_t &color, bool &enabled) const;
  bool matchesCssRule(const CssRule &rule, const String &tag, const String &attributes) const;
  void applyStyleOverride(TextStyle &style, const StyleOverride &override, bool imageTag) const;
  void applyCssRules(TextStyle &style, const String &tag, const String &attributes) const;
  String attributeValue(const String &attributes, const char *name) const;
  String lower(const String &value) const;
  uint16_t parseColor(const String &value, uint16_t fallback) const;
  bool parseCssColor(const String &value, uint16_t &color) const;
  void applyBackground(TextStyle &style, const String &value) const;
  String decodeEntities(const String &value) const;
  bool isBlockTag(const String &tag) const;
  void appendText(const String &text);
  void appendBreak();
  void appendImage(const String &label, const TextStyle &style, bool linked, uint8_t linkIndex,
                   bool fullWidth, int16_t imageIndex = -1);
  void appendInlineImage(const String &label, const TextStyle &style, int16_t imageIndex);
  void appendStub(const String &label);
  void appendFormControl(uint8_t index, const TextStyle &style);
  int16_t registerImage(const String &source, uint16_t backgroundColor,
                        const BrowserDocumentContext &context);
  Browser::TextStyle styleForTag(const String &tag, const String &attributes,
                                 const BrowserDocumentContext &context);
  int8_t addForm(const String &attributes);
  int8_t addFormControl(Browser::FormControlKind kind, int8_t formIndex, const String &attributes,
                        const TextStyle &style, const String &fallbackLabel = String());
  void addSelectOption(uint8_t controlIndex, const String &attributes, const String &text);
  bool hasAttribute(const String &attributes, const char *name) const;
  String formStubLabel(const String &tag, const String &attributes) const;
  void appendWord(const String &word, const TextStyle &style);
  void appendRun(const String &text, const TextStyle &style);
  void beginTextRow();
  int16_t textWidth(const String &text, const TextStyle &style) const;
  uint8_t registerLink(const String &url);
  int findTagEnd(const String &html, int start) const;
  void pushStyle(const String &tag, const Browser::TextStyle &style);
  void popStyle(const String &tag);
  String _linkUrls[BrowserLimits::MaxLinks];
  Browser::FormData _forms[BrowserLimits::MaxForms];
  Browser::FormControl _formControls[BrowserLimits::MaxFormControls];
  Browser::FormOption _formOptions[BrowserLimits::MaxFormOptions];
  uint8_t _formCount;
  uint8_t _formControlCount;
  uint8_t _formOptionCount;
  uint8_t _linkCount;
  uint16_t _documentBackgroundColor;
  String _titleBuffer;
  Browser::VisualRow _rows[BrowserLimits::MaxRows];
  Browser::EmbeddedImage _embeddedImages[BrowserLimits::MaxEmbeddedImages];
  uint16_t _embeddedImageCount;
  uint16_t _rowCount;
  uint16_t _pageStarts[BrowserLimits::MaxPages];
  uint8_t _pageCount;
  Browser::TextStyle _currentStyle;
  Browser::StyleFrame _styleStack[BrowserLimits::MaxStyleDepth];
  uint8_t _styleDepth;
  Browser::CssRule _cssRules[BrowserLimits::MaxCssRules];
  uint8_t _cssRuleCount;
  uint16_t _cssSourceOrder;
  bool _pendingSpace;
  uint16_t _renderedCharacters;
  BrowserDocumentLayout _parseLayout;
  BrowserDocumentContext _parseContext;

  static bool stylesEqual(const Browser::TextStyle &left, const Browser::TextStyle &right);
};
