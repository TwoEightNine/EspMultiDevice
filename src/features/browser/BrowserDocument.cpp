#include "BrowserDocument.h"

namespace {

String trimCopy(String value) {
  value.trim();
  return value;
}

using BrowserLimits::MaxCssDeclarationLength;
using BrowserLimits::MaxCssRules;
using BrowserLimits::MaxCssSelectorLength;
using BrowserLimits::MaxLinks;
using BrowserLimits::MaxStyleDepth;
using Browser::FormControl;
using Browser::FormControlKind;
using Browser::FormData;
using Browser::FormMethod;
using Browser::FormOption;
using Browser::VisualRow;
using Browser::VisualRun;
using BrowserLimits::MaxFormControls;
using BrowserLimits::MaxFormOptions;
using BrowserLimits::MaxForms;
using BrowserLimits::MaxRows;
using BrowserLimits::MaxRunsPerRow;

String urlEncode(const String &value) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (uint16_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '*') {
      encoded += static_cast<char>(c);
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 15];
    }
  }
  return encoded;
}

int16_t imageWidth(const Browser::EmbeddedImage *image, int16_t placeholderWidth) {
  return image && image->pixels && image->width ? image->width : placeholderWidth;
}

uint8_t utf8Bytes(const String &text, uint16_t index) {
  if (index >= text.length()) return 0;
  const uint8_t first = static_cast<uint8_t>(text[index]);
  const uint8_t wanted = first < 0x80 ? 1 :
                         (first >= 0xc2 && first <= 0xdf ? 2 :
                         (first >= 0xe0 && first <= 0xef ? 3 :
                         (first >= 0xf0 && first <= 0xf4 ? 4 : 1)));
  if (index + wanted > text.length()) return 1;
  for (uint8_t offset = 1; offset < wanted; ++offset) {
    if ((static_cast<uint8_t>(text[index + offset]) & 0xc0) != 0x80) return 1;
  }
  return wanted;
}

}  // namespace

BrowserDocument::BrowserDocument()
    : _formCount(0), _formControlCount(0), _formOptionCount(0), _linkCount(0),
      _documentBackgroundColor(0xffff), _embeddedImageCount(0), _rowCount(0),
      _pageCount(0), _styleDepth(0), _cssRuleCount(0), _cssSourceOrder(0),
      _pendingSpace(false), _renderedCharacters(0) {
  _parseContext = {0, nullptr, nullptr};
  _parseLayout = {0, 0, 0, 0, 0, 0, 0, 0};
  clear();
}

void BrowserDocument::clear() {
  for (uint16_t i = 0; i < BrowserLimits::MaxRows; ++i) _rows[i] = {};
  for (uint8_t i = 0; i < BrowserLimits::MaxLinks; ++i) _linkUrls[i] = "";
  for (uint8_t i = 0; i < BrowserLimits::MaxForms; ++i) _forms[i] = {};
  for (uint8_t i = 0; i < BrowserLimits::MaxFormControls; ++i) _formControls[i] = {};
  for (uint8_t i = 0; i < BrowserLimits::MaxFormOptions; ++i) _formOptions[i] = {};
  for (uint8_t i = 0; i < BrowserLimits::MaxStyleDepth; ++i) _styleStack[i] = {};
  for (uint8_t i = 0; i < BrowserLimits::MaxCssRules; ++i) _cssRules[i] = {};
  for (uint16_t i = 0; i < BrowserLimits::MaxEmbeddedImages; ++i) _embeddedImages[i] = {};
  for (uint8_t i = 0; i < BrowserLimits::MaxPages; ++i) _pageStarts[i] = 0;
  _formCount = _formControlCount = _formOptionCount = _linkCount = 0;
  _embeddedImageCount = _rowCount = _pageCount = 0;
  _styleDepth = _cssRuleCount = 0;
  _cssSourceOrder = _renderedCharacters = 0;
  _pendingSpace = false;
  _documentBackgroundColor = TFT_WHITE;
  _currentStyle = {TFT_BLACK, TFT_WHITE, false, TFT_WHITE, false,
                   false, false, false, false, 0, false};
  _parseContext = {0, nullptr, nullptr};
  _parseLayout = {0, 0, 0, 0, 0, 0, 0, 0};
  _titleBuffer = "";
}

void BrowserDocument::parseHtml(const String &html, const BrowserDocumentContext &context,
                                const BrowserDocumentLayout &layout) {
  clear();
  _parseContext = context;
  _parseLayout = layout;
  _documentBackgroundColor = TFT_WHITE;
  _currentStyle = {TFT_BLACK, TFT_WHITE, false, TFT_WHITE, false,
                   false, false, false, false, 0, false};

  collectCssRules(html);
  _titleBuffer = "";
  bool inScript = false;
  bool inStyle = false;
  bool inTitle = false;
  bool sawScript = false;
  int8_t currentForm = -1;
  int8_t textareaControl = -1;
  int8_t selectControl = -1;
  int8_t buttonControl = -1;
  String textareaText;
  String optionAttributes;
  String optionText;
  String buttonText;
  String labelIds[MaxFormControls];
  String labelTexts[MaxFormControls];
  bool inLabel = false;
  String labelFor;
  String labelText;
  uint8_t labelCount = 0;
  uint8_t labelStartControl = 0;
  bool inOption = false;
  uint32_t position = 0;
  uint32_t nextYield = 1024;
  while (position < html.length()) {
    if (position >= nextYield) {
      yield();
      nextYield = position + 1024;
    }
    int nextTag = html.indexOf('<', position);
    String text = nextTag < 0 ? html.substring(position) : html.substring(position, nextTag);
    if (inLabel) labelText += text;
    if (inTitle) _titleBuffer += text;
    else if (textareaControl >= 0) textareaText += text;
    else if (inOption) optionText += text;
    else if (buttonControl >= 0) buttonText += text;
    else if (!inScript && !inStyle) appendText(text);
    if (nextTag < 0) break;
    int endTag = findTagEnd(html, nextTag + 1);
    if (endTag < 0) break;
    String inside = trimCopy(html.substring(nextTag + 1, endTag));
    bool closing = inside.startsWith("/");
    if (closing) inside.remove(0, 1);
    inside.trim();
    int space = inside.indexOf(' ');
    String tag = lower(space < 0 ? inside : inside.substring(0, space));
    if (tag.endsWith("/")) tag.remove(tag.length() - 1);
    String attributes = space < 0 ? "" : trimCopy(inside.substring(space + 1));
    bool selfClosing = inside.endsWith("/");
    if (tag.startsWith("!") || tag.startsWith("?")) {
      position = endTag + 1;
      continue;
    }
    if (tag == "script") {
      if (!closing) sawScript = true;
      inScript = !closing;
    }
    else if (tag == "style") inStyle = !closing;
    else if (tag == "title") inTitle = !closing;
    else if (!inScript && !inStyle && !inTitle) {
      if (tag == "label") {
        if (closing) {
          const String resolvedLabel = decodeEntities(trimCopy(labelText));
          if (resolvedLabel.length()) {
            if (labelFor.length() && labelCount < MaxFormControls) {
              labelIds[labelCount] = labelFor;
              labelTexts[labelCount++] = resolvedLabel;
            } else if (_formControlCount > labelStartControl) {
              _formControls[_formControlCount - 1].label = resolvedLabel;
            }
          }
          inLabel = false;
          labelFor = "";
          labelText = "";
          popStyle(tag);
        } else {
          inLabel = true;
          labelFor = decodeEntities(attributeValue(attributes, "for"));
          labelText = "";
          labelStartControl = _formControlCount;
          if (!selfClosing) pushStyle(tag, styleForTag(tag, attributes, _parseContext));
        }
      } else if (tag == "form") {
        if (closing) {
          currentForm = -1;
          appendBreak();
        } else {
          currentForm = addForm(attributes);
          appendBreak();
        }
      } else if (tag == "textarea") {
        if (closing) {
          if (textareaControl >= 0 && textareaControl < _formControlCount) {
            FormControl &control = _formControls[textareaControl];
            control.value = decodeEntities(textareaText);
            control.defaultValue = control.value;
          }
          textareaText = "";
          textareaControl = -1;
        } else {
          textareaText = "";
          textareaControl = addFormControl(FormControlKind::Textarea, currentForm, attributes,
                                           styleForTag(tag, attributes, _parseContext));
        }
      } else if (tag == "select") {
        if (closing) {
          if (selectControl >= 0 && selectControl < _formControlCount) {
            FormControl &control = _formControls[selectControl];
            control.defaultSelectedOption = control.selectedOption;
          }
          selectControl = -1;
        } else if (hasAttribute(attributes, "multiple")) {
          appendStub(formStubLabel("select", attributes) + ": multiple select unsupported");
        } else {
          selectControl = addFormControl(FormControlKind::Select, currentForm, attributes,
                                         styleForTag(tag, attributes, _parseContext));
        }
      } else if (tag == "option" && selectControl >= 0) {
        if (closing || selfClosing) {
          addSelectOption((uint8_t)selectControl, optionAttributes, optionText);
          optionAttributes = "";
          optionText = "";
          inOption = false;
        } else {
          optionAttributes = attributes;
          optionText = "";
          inOption = true;
        }
      } else if (tag == "input" && !closing) {
        String type = lower(trimCopy(attributeValue(attributes, "type")));
        if (!type.length()) type = "text";
        const TextStyle controlStyle = styleForTag(tag, attributes, _parseContext);
        if (type == "text" || type == "search" || type == "email" || type == "url" || type == "number") {
          addFormControl(FormControlKind::Text, currentForm, attributes, controlStyle);
        } else if (type == "checkbox") {
          addFormControl(FormControlKind::Checkbox, currentForm, attributes, controlStyle);
        } else if (type == "radio") {
          addFormControl(FormControlKind::Radio, currentForm, attributes, controlStyle);
        } else if (type == "submit") {
          String label = decodeEntities(attributeValue(attributes, "value"));
          if (!label.length()) label = "Submit";
          addFormControl(FormControlKind::Submit, currentForm, attributes, controlStyle, label);
        } else if (type == "reset") {
          String label = decodeEntities(attributeValue(attributes, "value"));
          if (!label.length()) label = "Reset";
          addFormControl(FormControlKind::Reset, currentForm, attributes, controlStyle, label);
        } else if (type != "hidden") {
          appendStub(formStubLabel("input", attributes) + ": unsupported");
        }
      } else if (tag == "button") {
        if (closing) {
          if (buttonControl >= 0 && buttonControl < _formControlCount) {
            String label = decodeEntities(trimCopy(buttonText));
            if (label.length()) _formControls[buttonControl].label = label;
          }
          buttonText = "";
          buttonControl = -1;
        } else {
          String type = lower(trimCopy(attributeValue(attributes, "type")));
          if (!type.length()) type = "submit";
          FormControlKind kind = type == "submit" ? FormControlKind::Submit :
                                 (type == "reset" ? FormControlKind::Reset : FormControlKind::Unsupported);
          if (kind == FormControlKind::Unsupported) {
            appendStub(formStubLabel("button", attributes) + ": unsupported");
          } else {
            buttonText = "";
            buttonControl = addFormControl(kind, currentForm, attributes,
                                           styleForTag(tag, attributes, _parseContext));
          }
        }
      } else if (closing) {
        if (isBlockTag(tag)) appendBreak();
        if (tag == "td" || tag == "th") _pendingSpace = true;
        popStyle(tag);
      } else if (tag == "br") {
        appendBreak();
      } else if (tag == "img") {
        String source = decodeEntities(attributeValue(attributes, "src"));
        TextStyle imageStyle = styleForTag(tag, attributes, _parseContext);
        const uint16_t imageBackground = imageStyle.hasBackground ? imageStyle.backgroundColor :
          (imageStyle.hasBlockBackground ? imageStyle.blockBackgroundColor : _documentBackgroundColor);
        int16_t imageIndex = registerImage(source, imageBackground, _parseContext);
        uint8_t imageLinkIndex = _currentStyle.linkIndex;
        // An image source already lives in its bounded EmbeddedImage entry.
        // Do not also copy every source URL into the general link table: an
        // image-heavy page otherwise keeps two independently allocated String
        // copies per image while the page body and TLS buffers are still live.
        // The tap handler falls back to image.url when this image is not
        // inside an anchor, preserving direct-image navigation.
        const bool hasDirectImageLink = imageIndex >= 0;
        appendImage("", imageStyle,
                    imageLinkIndex != 0 || hasDirectImageLink,
                    imageLinkIndex, imageStyle.fullWidthImage, imageIndex);
      } else if (tag == "audio" || tag == "video" || tag == "source") {
        String source = decodeEntities(attributeValue(attributes, "src"));
        TextStyle mediaStyle = styleForTag(tag, attributes, _parseContext);
        String normalized;
        if (source.length() && _parseContext.resolveUrl &&
            _parseContext.resolveUrl(_parseContext.resolverContext, source, normalized)) {
          mediaStyle.link = true;
          mediaStyle.linkIndex = registerLink(normalized);
        }
        appendBreak();
        appendWord(source.length() ? String(F("[")) + (tag == "source" ? String("media") : tag) + F("]")
                                  : String(F("(")) + tag + F(")"), mediaStyle);
        appendBreak();
      } else if (tag == "iframe" || tag == "object" || tag == "embed" || tag == "canvas") {
        appendStub(tag);
      } else {
        if (isBlockTag(tag)) appendBreak();
        if ((tag == "td" || tag == "th") && _rowCount) _pendingSpace = true;
        if (!selfClosing) {
          TextStyle tagStyle = styleForTag(tag, attributes, _parseContext);
          if ((tag == "html" || tag == "body") && tagStyle.hasBackground) {
            _documentBackgroundColor = tagStyle.backgroundColor;
          }
          pushStyle(tag, tagStyle);
        }
      }
    }
    position = endTag + 1;
  }
  for (uint8_t labelIndex = 0; labelIndex < labelCount; labelIndex++) {
    for (uint8_t controlIndex = 0; controlIndex < _formControlCount; controlIndex++) {
      if (_formControls[controlIndex].id == labelIds[labelIndex]) {
        _formControls[controlIndex].label = labelTexts[labelIndex];
        break;
      }
    }
  }
  _titleBuffer = decodeEntities(trimCopy(_titleBuffer));
  if (_titleBuffer.length() == 0) _titleBuffer = "Web page";
  if (_rowCount == 0) appendText(sawScript
    ? "(This page is generated by JavaScript. JavaScript is not supported.)"
    : "(This page has no static readable text.)");
  reflow(_parseLayout);

}

void BrowserDocument::parsePlainText(const String &text, const String &title,
                                     const BrowserDocumentLayout &layout,
                                     uint16_t textColor, uint16_t backgroundColor) {
  clear();
  _parseLayout = layout;
  _documentBackgroundColor = backgroundColor;
  _titleBuffer = title;
  _currentStyle = {textColor, backgroundColor, false, backgroundColor,
                   false, false, false, false, false, 0, false};
  appendText(text);
  if (_rowCount == 0) appendText("(No readable text in this response.)");
  reflow(layout);
}

bool BrowserDocument::stylesEqual(const Browser::TextStyle &left, const Browser::TextStyle &right) {
  return left.color == right.color && left.backgroundColor == right.backgroundColor &&
         left.hasBackground == right.hasBackground && left.blockBackgroundColor == right.blockBackgroundColor &&
         left.hasBlockBackground == right.hasBlockBackground && left.bold == right.bold &&
         left.italic == right.italic && left.underline == right.underline && left.link == right.link &&
         left.linkIndex == right.linkIndex && left.fullWidthImage == right.fullWidthImage && left.size == right.size;
}

int16_t BrowserDocument::textWidth(const String &text, const TextStyle &style) const {
  return BrowserText::measure(text, style.size);
}

uint16_t BrowserDocument::pageStart(uint8_t page) const {
  return page < _pageCount ? _pageStarts[page] : _rowCount;
}

uint16_t BrowserDocument::pageEnd(uint8_t page) const {
  if (page >= _pageCount) return _rowCount;
  return page + 1 < _pageCount ? _pageStarts[page + 1] : _rowCount;
}

int16_t BrowserDocument::rowHeight(uint16_t rowIndex, const BrowserDocumentLayout &layout) const {
  if (rowIndex >= _rowCount) return 0;
  const Browser::VisualRow &source = _rows[rowIndex];
  if (source.formControlIndex >= 0 && source.formControlIndex < _formControlCount) {
    return _formControls[source.formControlIndex].kind == Browser::FormControlKind::Textarea
      ? layout.formTextareaHeight : layout.formControlHeight;
  }
  int16_t height = layout.textRowHeight;
  if (source.image) {
    if (source.imageIndex >= 0 && source.imageIndex < _embeddedImageCount) {
      const Browser::EmbeddedImage &image = _embeddedImages[source.imageIndex];
      if (image.pixels && image.height) height = image.height;
    }
  } else {
    for (uint8_t runIndex = 0; runIndex < source.runCount; ++runIndex) {
      const Browser::VisualRun &run = source.runs[runIndex];
      const int16_t textHeight = BrowserText::lineHeight(run.style.size) + 2;
      if (textHeight > height) height = textHeight;
      if (!run.image || run.imageIndex < 0 || run.imageIndex >= _embeddedImageCount) continue;
      const Browser::EmbeddedImage &image = _embeddedImages[run.imageIndex];
      if (image.pixels && image.height > height) height = image.height;
    }
  }
  return height > layout.textRowHeight ? height : layout.textRowHeight;
}

void BrowserDocument::reflow(const BrowserDocumentLayout &layout) {
  for (uint16_t rowIndex = 0; rowIndex < _rowCount; ++rowIndex) {
    Browser::VisualRow &row = _rows[rowIndex];
    if (row.image) continue;
    int16_t x = 0;
    for (uint8_t runIndex = 0; runIndex < row.runCount; ++runIndex) {
      Browser::VisualRun &run = row.runs[runIndex];
      if (run.image && run.imageIndex >= 0 && run.imageIndex < _embeddedImageCount) {
        run.width = imageWidth(&_embeddedImages[run.imageIndex], layout.imagePlaceholderWidth);
      }
      run.x = x;
      x += run.width;
    }
    row.contentWidth = x;
  }

  _pageCount = 0;
  if (_rowCount == 0) return;
  uint16_t first = 0;
  int16_t y = layout.documentTop;
  _pageStarts[_pageCount++] = 0;
  for (uint16_t index = 0; index < _rowCount; ++index) {
    const int16_t height = rowHeight(index, layout);
    if (y + height > layout.documentBottom && index > first) {
      if (_pageCount >= BrowserLimits::MaxPages) break;
      first = index;
      _pageStarts[_pageCount++] = index;
      y = layout.documentTop;
    }
    y += height;
  }
}

String BrowserDocument::linkAt(uint8_t page, int16_t x, int16_t y,
                               const BrowserDocumentLayout &layout) const {
  if (page >= _pageCount) return "";
  int16_t rowY = layout.documentTop;
  for (uint16_t index = pageStart(page); index < pageEnd(page); ++index) {
    const Browser::VisualRow &source = _rows[index];
    const int16_t height = rowHeight(index, layout);
    if (y >= rowY && y < rowY + height) {
      if (source.image) {
        const Browser::EmbeddedImage *image = source.imageIndex >= 0 && source.imageIndex < _embeddedImageCount
          ? &_embeddedImages[source.imageIndex] : nullptr;
        if (x < layout.sidePadding || x >= layout.sidePadding + imageWidth(image, layout.imagePlaceholderWidth)) return "";
        if (source.imageLinkIndex && source.imageLinkIndex <= _linkCount) return _linkUrls[source.imageLinkIndex - 1];
        return image ? String(image->url) : String();
      }
      for (uint8_t runIndex = 0; runIndex < source.runCount; ++runIndex) {
        const Browser::VisualRun &run = source.runs[runIndex];
        if (x < layout.sidePadding + run.x || x >= layout.sidePadding + run.x + run.width) continue;
        if (run.style.linkIndex && run.style.linkIndex <= _linkCount) return _linkUrls[run.style.linkIndex - 1];
        if (run.image && run.imageIndex >= 0 && run.imageIndex < _embeddedImageCount) return String(_embeddedImages[run.imageIndex].url);
      }
      return "";
    }
    rowY += height;
  }
  return "";
}

int8_t BrowserDocument::formAt(uint8_t page, int16_t x, int16_t y,
                               const BrowserDocumentLayout &layout) const {
  if (page >= _pageCount || x < layout.sidePadding || x >= layout.viewportWidth - layout.sidePadding) return -1;
  int16_t rowY = layout.documentTop;
  for (uint16_t index = pageStart(page); index < pageEnd(page); ++index) {
    const int16_t height = rowHeight(index, layout);
    if (y >= rowY && y < rowY + height) return _rows[index].formControlIndex;
    rowY += height;
  }
  return -1;
}

bool BrowserDocument::imageVisibleOnPage(uint8_t page, uint16_t imageIndex) const {
  if (page >= _pageCount) return false;
  for (uint16_t rowIndex = pageStart(page); rowIndex < pageEnd(page); ++rowIndex) {
    const Browser::VisualRow &row = _rows[rowIndex];
    if (row.image && row.imageIndex == static_cast<int16_t>(imageIndex)) return true;
    for (uint8_t runIndex = 0; runIndex < row.runCount; ++runIndex) {
      if (row.runs[runIndex].image && row.runs[runIndex].imageIndex == static_cast<int16_t>(imageIndex)) return true;
    }
  }
  return false;
}

bool BrowserDocument::setFormValue(uint8_t controlIndex, const String &value) {
  if (controlIndex >= _formControlCount) return false;
  _formControls[controlIndex].value = value;
  return true;
}

bool BrowserDocument::resetForm(uint8_t formIndex) {
  if (formIndex >= _formCount) return false;
  for (uint8_t index = 0; index < _formControlCount; ++index) {
    Browser::FormControl &control = _formControls[index];
    if (control.formIndex != formIndex) continue;
    control.value = control.defaultValue;
    control.checked = control.defaultChecked;
    control.selectedOption = control.defaultSelectedOption;
  }
  return true;
}

bool BrowserDocument::toggleFormControl(uint8_t controlIndex) {
  if (controlIndex >= _formControlCount) return false;
  Browser::FormControl &control = _formControls[controlIndex];
  if (control.disabled) return false;
  if (control.kind == Browser::FormControlKind::Checkbox) {
    control.checked = !control.checked;
    return true;
  }
  if (control.kind == Browser::FormControlKind::Radio) {
    if (control.checked) return false;
    for (uint8_t index = 0; index < _formControlCount; ++index) {
      Browser::FormControl &candidate = _formControls[index];
      if (candidate.kind == Browser::FormControlKind::Radio && candidate.formIndex == control.formIndex &&
          candidate.name == control.name) candidate.checked = false;
    }
    control.checked = true;
    return true;
  }
  if (control.kind == Browser::FormControlKind::Select && control.optionCount) {
    control.selectedOption = (control.selectedOption + 1) % control.optionCount;
    return true;
  }
  return false;
}

String BrowserDocument::formControlLabel(uint8_t controlIndex) const {
  if (controlIndex >= _formControlCount) return "";
  const Browser::FormControl &control = _formControls[controlIndex];
  if (control.label.length()) return control.label;
  switch (control.kind) {
    case Browser::FormControlKind::Submit: return "Submit";
    case Browser::FormControlKind::Reset: return "Reset";
    case Browser::FormControlKind::Select: return "Select";
    default: return "Input";
  }
}

String BrowserDocument::formControlValue(uint8_t controlIndex) const {
  if (controlIndex >= _formControlCount) return "";
  const Browser::FormControl &control = _formControls[controlIndex];
  if (control.kind == Browser::FormControlKind::Select && control.optionCount) {
    const uint16_t optionIndex = control.firstOption + control.selectedOption;
    if (optionIndex < _formOptionCount) return _formOptions[optionIndex].value;
  }
  return control.value;
}

bool BrowserDocument::buildFormPayload(uint8_t formIndex, int8_t submitControlIndex,
                                       String &payload, uint16_t maxBytes) const {
  payload = "";
  if (formIndex >= _formCount) return false;
  for (uint8_t index = 0; index < _formControlCount; ++index) {
    const Browser::FormControl &control = _formControls[index];
    if (control.formIndex != formIndex || control.disabled || !control.name.length()) continue;
    if (control.kind == Browser::FormControlKind::Submit && index != submitControlIndex) continue;
    if (control.kind == Browser::FormControlKind::Reset || control.kind == Browser::FormControlKind::Unsupported) continue;
    if ((control.kind == Browser::FormControlKind::Checkbox || control.kind == Browser::FormControlKind::Radio) && !control.checked) continue;
    const String pair = urlEncode(control.name) + '=' + urlEncode(formControlValue(index));
    const uint32_t nextLength = payload.length() + (payload.length() ? 1 : 0) + pair.length();
    if (maxBytes && nextLength > maxBytes) return false;
    if (payload.length()) payload += '&';
    payload += pair;
  }
  return true;
}

bool BrowserDocument::setImageResult(uint16_t imageIndex, uint32_t generation, uint16_t *pixels,
                                     uint8_t width, uint8_t height) {
  if (imageIndex >= _embeddedImageCount) return false;
  Browser::EmbeddedImage &image = _embeddedImages[imageIndex];
  if (image.generation != generation || image.state != 1 || !pixels || !width || !height) return false;
  image.pixels = pixels;
  image.width = width;
  image.height = height;
  image.state = 2;
  return true;
}

int16_t BrowserDocument::registerImage(const String &source, uint16_t backgroundColor,
                                       const BrowserDocumentContext &context) {
  if (source.length() == 0 || _embeddedImageCount >= BrowserLimits::MaxEmbeddedImages ||
      source.length() >= BrowserLimits::ImageUrlMaxLength || !context.resolveUrl) return -1;
  String url;
  if (!context.resolveUrl(context.resolverContext, source, url) || url.length() == 0 ||
      url.length() >= BrowserLimits::ImageUrlMaxLength) return -1;

  // Keep one descriptor per URL/background pair. Transparent previews are
  // decoded against the descriptor's page background, so a different
  // background intentionally requires a distinct thumbnail.
  for (uint16_t index = 0; index < _embeddedImageCount; ++index) {
    const Browser::EmbeddedImage &image = _embeddedImages[index];
    if (image.generation == context.imageGeneration && image.backgroundColor == backgroundColor &&
        strcmp(image.url, url.c_str()) == 0) return index;
  }

  const uint16_t index = _embeddedImageCount++;
  Browser::EmbeddedImage &image = _embeddedImages[index];
  image = {};
  image.backgroundColor = backgroundColor;
  image.generation = context.imageGeneration;
  url.toCharArray(image.url, sizeof(image.url));
  return index;
}

const Browser::VisualRow *BrowserDocument::row(uint16_t index) const {
  return index < _rowCount ? &_rows[index] : nullptr;
}

const String *BrowserDocument::link(uint8_t index) const {
  return index < _linkCount ? &_linkUrls[index] : nullptr;
}

const Browser::FormData *BrowserDocument::form(uint8_t index) const {
  return index < _formCount ? &_forms[index] : nullptr;
}

const Browser::FormControl *BrowserDocument::formControl(uint8_t index) const {
  return index < _formControlCount ? &_formControls[index] : nullptr;
}

const Browser::FormOption *BrowserDocument::formOption(uint8_t index) const {
  return index < _formOptionCount ? &_formOptions[index] : nullptr;
}

const Browser::EmbeddedImage *BrowserDocument::image(uint16_t index) const {
  return index < _embeddedImageCount ? &_embeddedImages[index] : nullptr;
}

bool BrowserDocument::beginImageJob(uint16_t imageIndex, uint32_t generation,
                                    bool useSessionCache, Browser::ImageJob &job) {
  if (imageIndex >= _embeddedImageCount) return false;
  Browser::EmbeddedImage &image = _embeddedImages[imageIndex];
  if (image.generation != generation || image.state != 0 || image.url[0] == '\0') return false;
  job = {};
  job.imageIndex = imageIndex;
  job.generation = image.generation;
  job.backgroundColor = image.backgroundColor;
  job.useSessionCache = useSessionCache;
  memcpy(job.url, image.url, sizeof(job.url));
  image.state = 1;
  return true;
}

bool BrowserDocument::returnImageJobToPending(uint16_t imageIndex, uint32_t generation) {
  if (imageIndex >= _embeddedImageCount) return false;
  Browser::EmbeddedImage &image = _embeddedImages[imageIndex];
  if (image.generation != generation || image.state != 1 || image.pixels) return false;
  image.state = 0;
  return true;
}

bool BrowserDocument::hasUnfinishedImages(uint32_t generation) const {
  for (uint16_t index = 0; index < _embeddedImageCount; ++index) {
    const Browser::EmbeddedImage &image = _embeddedImages[index];
    if (image.generation == generation && image.state < 2) return true;
  }
  return false;
}

bool BrowserDocument::markImageFailed(uint16_t imageIndex, uint32_t generation) {
  if (imageIndex >= _embeddedImageCount) return false;
  Browser::EmbeddedImage &image = _embeddedImages[imageIndex];
  if (image.generation != generation || image.state != 1) return false;
  image.state = 3;
  return true;
}

void BrowserDocument::clearImageDescriptors() {
  for (uint16_t index = 0; index < _embeddedImageCount; ++index) _embeddedImages[index] = {};
  _embeddedImageCount = 0;
}

void BrowserDocument::collectCssRules(const String &html) {
  bool inStyle = false;
  bool inScript = false;
  uint32_t styleStart = 0;
  uint32_t position = 0;
  uint32_t nextYield = 1024;
  while (position < html.length() && _cssRuleCount < MaxCssRules) {
    if (position >= nextYield) {
      yield();
      nextYield = position + 1024;
    }
    int nextTag = html.indexOf('<', position);
    if (nextTag < 0) break;
    int endTag = findTagEnd(html, nextTag + 1);
    if (endTag < 0) break;
    String inside = trimCopy(html.substring(nextTag + 1, endTag));
    bool closing = inside.startsWith("/");
    if (closing) inside.remove(0, 1);
    inside.trim();
    int space = inside.indexOf(' ');
    String tag = lower(space < 0 ? inside : inside.substring(0, space));
    if (tag.endsWith("/")) tag.remove(tag.length() - 1);
    if (tag == "script") {
      inScript = !closing;
    } else if (tag == "style" && !inScript) {
      if (!closing && !inStyle) {
        inStyle = true;
        styleStart = endTag + 1;
      } else if (closing && inStyle) {
        parseCssRules(html, styleStart, nextTag);
        inStyle = false;
      }
    }
    position = endTag + 1;
  }
}

void BrowserDocument::skipCssSpaceAndComments(const String &source, uint32_t &position, uint32_t end) const {
  while (position < end) {
    if (isspace((unsigned char)source[position])) {
      position++;
      continue;
    }
    if (position + 1 < end && source[position] == '/' && source[position + 1] == '*') {
      position += 2;
      while (position + 1 < end && !(source[position] == '*' && source[position + 1] == '/')) position++;
      position = position + 1 < end ? position + 2 : end;
      continue;
    }
    break;
  }
}

bool BrowserDocument::cssSlice(const String &source, uint32_t start, uint32_t end, uint16_t limit,
                             String &result) const {
  result = "";
  for (uint32_t position = start; position < end; position++) {
    if (position + 1 < end && source[position] == '/' && source[position + 1] == '*') {
      position += 2;
      while (position + 1 < end && !(source[position] == '*' && source[position + 1] == '/')) position++;
      if (position + 1 < end) position++;
      continue;
    }
    if (result.length() >= limit) return false;
    result += source[position];
  }
  result.trim();
  return true;
}

void BrowserDocument::parseCssRules(const String &source, uint32_t start, uint32_t end) {
  uint32_t position = start;
  while (position < end && _cssRuleCount < MaxCssRules) {
    skipCssSpaceAndComments(source, position, end);
    const uint32_t selectorStart = position;
    while (position < end) {
      if (position + 1 < end && source[position] == '/' && source[position + 1] == '*') {
        position += 2;
        while (position + 1 < end && !(source[position] == '*' && source[position + 1] == '/')) position++;
        position = position + 1 < end ? position + 2 : end;
        continue;
      }
      if (source[position] == '{' || source[position] == '}') break;
      position++;
    }
    if (position >= end) break;
    if (source[position] != '{') {
      position++;
      continue;
    }
    const uint32_t selectorEnd = position++;
    const uint32_t declarationsStart = position;
    while (position < end) {
      if (position + 1 < end && source[position] == '/' && source[position + 1] == '*') {
        position += 2;
        while (position + 1 < end && !(source[position] == '*' && source[position + 1] == '/')) position++;
        position = position + 1 < end ? position + 2 : end;
        continue;
      }
      if (source[position] == '}') break;
      position++;
    }
    if (position >= end) break;
    const uint32_t declarationsEnd = position++;
    String selectors;
    String declarations;
    if (!cssSlice(source, selectorStart, selectorEnd, MaxCssSelectorLength, selectors) ||
        !cssSlice(source, declarationsStart, declarationsEnd, MaxCssDeclarationLength, declarations)) continue;
    StyleOverride style = {};
    if (!parseCssDeclarations(declarations, style)) continue;
    const uint16_t sourceOrder = _cssSourceOrder++;
    int selectorStartIndex = 0;
    while (selectorStartIndex < (int)selectors.length() && _cssRuleCount < MaxCssRules) {
      int selectorEndIndex = selectors.indexOf(',', selectorStartIndex);
      String selector = trimCopy(selectors.substring(selectorStartIndex,
        selectorEndIndex < 0 ? selectors.length() : selectorEndIndex));
      addCssRule(selector, style, sourceOrder);
      if (selectorEndIndex < 0) break;
      selectorStartIndex = selectorEndIndex + 1;
    }
  }
}

bool BrowserDocument::addCssRule(const String &rawSelector, const StyleOverride &style, uint16_t sourceOrder) {
  if (_cssRuleCount >= MaxCssRules) return false;
  String selector = trimCopy(rawSelector);
  if (!selector.length()) return false;
  uint8_t selectorType = 0;
  uint8_t specificity = 1;
  if (selector[0] == '.') {
    selectorType = 1;
    specificity = 10;
    selector.remove(0, 1);
  } else if (selector[0] == '#') {
    selectorType = 2;
    specificity = 100;
    selector.remove(0, 1);
  } else {
    selector = lower(selector);
  }
  if (!selector.length() || selector.length() > MaxCssSelectorLength) return false;
  for (uint16_t index = 0; index < selector.length(); index++) {
    char value = selector[index];
    if (!isalnum((unsigned char)value) && value != '-' && value != '_') return false;
  }
  _cssRules[_cssRuleCount] = {selector, style, sourceOrder, specificity, selectorType};
  _cssRuleCount++;
  return true;
}

bool BrowserDocument::parseCssDeclarations(const String &declarations, StyleOverride &style) const {
  bool found = false;
  int start = 0;
  while (start < (int)declarations.length()) {
    int end = declarations.indexOf(';', start);
    String declaration = trimCopy(declarations.substring(start, end < 0 ? declarations.length() : end));
    int colon = declaration.indexOf(':');
    if (colon > 0) {
      String property = lower(trimCopy(declaration.substring(0, colon)));
      String value = lower(trimCopy(declaration.substring(colon + 1)));
      value.replace("!important", "");
      value.trim();
      if (property == "color") {
        uint16_t color;
        if (parseCssColor(value, color)) {
          style.hasColor = true;
          style.color = color;
          found = true;
        }
      } else if (property == "background" || property == "background-color") {
        uint16_t color;
        bool enabled;
        if (parseBackground(value, color, enabled)) {
          style.hasBackground = true;
          style.backgroundColor = color;
          style.backgroundEnabled = enabled;
          found = true;
        }
      } else if (property == "font-weight") {
        bool digits = value.length() > 0;
        for (uint16_t index = 0; index < value.length(); index++) {
          if (!isdigit((unsigned char)value[index])) digits = false;
        }
        if (value == "normal" || value.indexOf("bold") >= 0 || digits) {
          style.hasBold = true;
          style.bold = value.indexOf("bold") >= 0 || (digits && value.toInt() >= 600);
          found = true;
        }
      } else if (property == "font-style") {
        if (value == "normal" || value.indexOf("italic") >= 0 || value.indexOf("oblique") >= 0) {
          style.hasItalic = true;
          style.italic = value.indexOf("italic") >= 0 || value.indexOf("oblique") >= 0;
          found = true;
        }
      } else if (property == "font-size") {
        BrowserText::Size size = BrowserText::Size::Normal;
        bool valid = value == "small" || value == "medium" || value == "normal" ||
                     value == "large" || value == "x-large" || value.endsWith("px");
        if (valid) {
          if (value == "small") size = BrowserText::Size::Small;
          else if (value == "large" || value == "x-large") size = BrowserText::Size::Large;
          else if (value.endsWith("px")) {
            const int pixels = value.substring(0, value.length() - 2).toInt();
            size = pixels <= 15 ? BrowserText::Size::Small :
                   (pixels >= 22 ? BrowserText::Size::Large : BrowserText::Size::Normal);
          }
          style.hasSize = true;
          style.size = size;
          found = true;
        }
      } else if (property == "text-decoration") {
        if (value.length()) {
          style.hasUnderline = true;
          style.underline = value.indexOf("underline") >= 0;
          found = true;
        }
      } else if (property == "display" && value == "block") {
        style.hasFullWidthImage = true;
        style.fullWidthImage = true;
        found = true;
      } else if (property == "width" && value == "100%") {
        style.hasFullWidthImage = true;
        style.fullWidthImage = true;
        found = true;
      }
    }
    if (end < 0) break;
    start = end + 1;
  }
  return found;
}

bool BrowserDocument::parseBackground(const String &rawValue, uint16_t &color, bool &enabled) const {
  String value = lower(trimCopy(rawValue));
  value.replace("!important", "");
  value.trim();
  if (value == "transparent") {
    enabled = false;
    return true;
  }
  if (parseCssColor(value, color)) {
    enabled = true;
    return true;
  }
  int start = 0;
  while (start < (int)value.length()) {
    while (start < (int)value.length() && isspace((unsigned char)value[start])) start++;
    int end = start;
    while (end < (int)value.length() && !isspace((unsigned char)value[end])) end++;
    if (parseCssColor(value.substring(start, end), color)) {
      enabled = true;
      return true;
    }
    start = end + 1;
  }
  return false;
}

bool BrowserDocument::matchesCssRule(const CssRule &rule, const String &tag, const String &attributes) const {
  if (rule.selectorType == 0) return rule.selector == tag;
  String value = attributeValue(attributes, rule.selectorType == 1 ? "class" : "id");
  if (rule.selectorType == 2) return value == rule.selector;
  int start = 0;
  while (start < (int)value.length()) {
    while (start < (int)value.length() && isspace((unsigned char)value[start])) start++;
    int end = start;
    while (end < (int)value.length() && !isspace((unsigned char)value[end])) end++;
    if (value.substring(start, end) == rule.selector) return true;
    start = end + 1;
  }
  return false;
}

void BrowserDocument::applyStyleOverride(TextStyle &style, const StyleOverride &override, bool imageTag) const {
  if (override.hasColor) style.color = override.color;
  if (override.hasBackground) {
    style.hasBackground = override.backgroundEnabled;
    if (override.backgroundEnabled) style.backgroundColor = override.backgroundColor;
  }
  if (override.hasBold) style.bold = override.bold;
  if (override.hasItalic) style.italic = override.italic;
  if (override.hasSize) style.size = override.size;
  if (override.hasUnderline) style.underline = override.underline;
  if (imageTag && override.hasFullWidthImage) style.fullWidthImage = override.fullWidthImage;
}

void BrowserDocument::applyCssRules(TextStyle &style, const String &tag, const String &attributes) const {
  StyleOverride resolved = {};
  int16_t colorSpecificity = -1, backgroundSpecificity = -1, boldSpecificity = -1;
  int16_t italicSpecificity = -1, underlineSpecificity = -1, sizeSpecificity = -1, imageSpecificity = -1;
  uint16_t colorOrder = 0, backgroundOrder = 0, boldOrder = 0;
  uint16_t italicOrder = 0, underlineOrder = 0, sizeOrder = 0, imageOrder = 0;
  for (uint8_t index = 0; index < _cssRuleCount; index++) {
    const CssRule &rule = _cssRules[index];
    if (!matchesCssRule(rule, tag, attributes)) continue;
    const StyleOverride &candidate = rule.style;
    if (candidate.hasColor && (rule.specificity > colorSpecificity ||
        (rule.specificity == colorSpecificity && rule.sourceOrder >= colorOrder))) {
      resolved.hasColor = true; resolved.color = candidate.color;
      colorSpecificity = rule.specificity; colorOrder = rule.sourceOrder;
    }
    if (candidate.hasBackground && (rule.specificity > backgroundSpecificity ||
        (rule.specificity == backgroundSpecificity && rule.sourceOrder >= backgroundOrder))) {
      resolved.hasBackground = true; resolved.backgroundEnabled = candidate.backgroundEnabled;
      resolved.backgroundColor = candidate.backgroundColor;
      backgroundSpecificity = rule.specificity; backgroundOrder = rule.sourceOrder;
    }
    if (candidate.hasBold && (rule.specificity > boldSpecificity ||
        (rule.specificity == boldSpecificity && rule.sourceOrder >= boldOrder))) {
      resolved.hasBold = true; resolved.bold = candidate.bold;
      boldSpecificity = rule.specificity; boldOrder = rule.sourceOrder;
    }
    if (candidate.hasItalic && (rule.specificity > italicSpecificity ||
        (rule.specificity == italicSpecificity && rule.sourceOrder >= italicOrder))) {
      resolved.hasItalic = true; resolved.italic = candidate.italic;
      italicSpecificity = rule.specificity; italicOrder = rule.sourceOrder;
    }
    if (candidate.hasSize && (rule.specificity > sizeSpecificity ||
        (rule.specificity == sizeSpecificity && rule.sourceOrder >= sizeOrder))) {
      resolved.hasSize = true; resolved.size = candidate.size;
      sizeSpecificity = rule.specificity; sizeOrder = rule.sourceOrder;
    }
    if (candidate.hasUnderline && (rule.specificity > underlineSpecificity ||
        (rule.specificity == underlineSpecificity && rule.sourceOrder >= underlineOrder))) {
      resolved.hasUnderline = true; resolved.underline = candidate.underline;
      underlineSpecificity = rule.specificity; underlineOrder = rule.sourceOrder;
    }
    if (candidate.hasFullWidthImage && (rule.specificity > imageSpecificity ||
        (rule.specificity == imageSpecificity && rule.sourceOrder >= imageOrder))) {
      resolved.hasFullWidthImage = true; resolved.fullWidthImage = candidate.fullWidthImage;
      imageSpecificity = rule.specificity; imageOrder = rule.sourceOrder;
    }
  }
  applyStyleOverride(style, resolved, tag == "img");
}

uint8_t BrowserDocument::registerLink(const String &url) {
  for (uint8_t index = 0; index < _linkCount; index++) {
    if (_linkUrls[index] == url) return index + 1;
  }
  if (_linkCount >= MaxLinks) return 0;
  _linkUrls[_linkCount] = url;
  _linkCount++;
  return _linkCount;
}

String BrowserDocument::attributeValue(const String &attributes, const char *name) const {
  String wanted = lower(String(name));
  uint16_t index = 0;
  while (index < attributes.length()) {
    while (index < attributes.length() && (isspace((unsigned char)attributes[index]) || attributes[index] == '/')) index++;
    uint16_t nameStart = index;
    while (index < attributes.length() && !isspace((unsigned char)attributes[index]) &&
           attributes[index] != '=' && attributes[index] != '/') index++;
    if (nameStart == index) {
      index++;
      continue;
    }
    String attributeName = lower(attributes.substring(nameStart, index));
    while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
    if (index >= attributes.length() || attributes[index] != '=') {
      continue;
    }
    index++;
    while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
    String value;
    if (index < attributes.length() && (attributes[index] == '\'' || attributes[index] == '\"')) {
      char quote = attributes[index++];
      uint16_t valueStart = index;
      while (index < attributes.length() && attributes[index] != quote) index++;
      value = attributes.substring(valueStart, index);
      if (index < attributes.length()) index++;
    } else {
      uint16_t valueStart = index;
      while (index < attributes.length() && !isspace((unsigned char)attributes[index])) index++;
      value = attributes.substring(valueStart, index);
    }
    if (attributeName == wanted) return value;
  }
  return "";
}

uint16_t BrowserDocument::parseColor(const String &rawValue, uint16_t fallback) const {
  String value = lower(trimCopy(rawValue));
  if (value.startsWith("#")) {
    String digits = value.substring(1);
    for (uint16_t i = 0; i < digits.length(); i++) {
      if (!isxdigit((unsigned char)digits[i])) return fallback;
    }
    char *end = nullptr;
    if (digits.length() == 3) {
      uint8_t red = strtoul(digits.substring(0, 1).c_str(), &end, 16) * 17;
      uint8_t green = strtoul(digits.substring(1, 2).c_str(), &end, 16) * 17;
      uint8_t blue = strtoul(digits.substring(2, 3).c_str(), &end, 16) * 17;
      return ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
    }
    if (digits.length() == 6) {
      uint8_t red = strtoul(digits.substring(0, 2).c_str(), &end, 16);
      uint8_t green = strtoul(digits.substring(2, 4).c_str(), &end, 16);
      uint8_t blue = strtoul(digits.substring(4, 6).c_str(), &end, 16);
      return ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
    }
  }
  if (value == "black") return TFT_BLACK;
  if (value == "white") return TFT_WHITE;
  if (value == "red") return TFT_RED;
  if (value == "green") return TFT_GREEN;
  if (value == "blue") return TFT_BLUE;
  if (value == "yellow") return TFT_YELLOW;
  if (value == "cyan" || value == "aqua") return TFT_CYAN;
  if (value == "magenta" || value == "fuchsia") return TFT_MAGENTA;
  if (value == "gray" || value == "grey") return TFT_LIGHTGREY;
  if (value == "darkgray" || value == "darkgrey") return TFT_DARKGREY;
  if (value == "orange") return TFT_ORANGE;
  if (value == "purple") return 0x8010;
  return fallback;
}

bool BrowserDocument::parseCssColor(const String &rawValue, uint16_t &color) const {
  String value = lower(trimCopy(rawValue));
  if (value.startsWith("#")) {
    String digits = value.substring(1);
    if (digits.length() != 3 && digits.length() != 6) return false;
    for (uint16_t i = 0; i < digits.length(); i++) {
      if (!isxdigit((unsigned char)digits[i])) return false;
    }
    color = parseColor(value, TFT_BLACK);
    return true;
  }
  if (value.startsWith("rgb(") && value.endsWith(")")) {
    int red = -1;
    int green = -1;
    int blue = -1;
    if (sscanf(value.c_str(), "rgb(%d,%d,%d)", &red, &green, &blue) == 3 &&
        red >= 0 && red <= 255 && green >= 0 && green <= 255 && blue >= 0 && blue <= 255) {
      color = ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
      return true;
    }
  }
  if (value == "black" || value == "white" || value == "red" || value == "green" ||
      value == "blue" || value == "yellow" || value == "cyan" || value == "aqua" ||
      value == "magenta" || value == "fuchsia" || value == "gray" || value == "grey" ||
      value == "darkgray" || value == "darkgrey" || value == "orange" || value == "purple") {
    color = parseColor(value, TFT_BLACK);
    return true;
  }
  return false;
}

void BrowserDocument::applyBackground(TextStyle &style, const String &rawValue) const {
  uint16_t color = TFT_BLACK;
  bool enabled = false;
  if (!parseBackground(rawValue, color, enabled)) return;
  style.hasBackground = enabled;
  if (enabled) style.backgroundColor = color;
}

int BrowserDocument::findTagEnd(const String &html, int start) const {
  char quote = '\0';
  for (int index = start; index < (int)html.length(); index++) {
    char value = html[index];
    if (quote) {
      if (value == quote) quote = '\0';
    } else if (value == '\'' || value == '\"') {
      quote = value;
    } else if (value == '>') {
      return index;
    }
  }
  return -1;
}

void BrowserDocument::pushStyle(const String &tag, const Browser::TextStyle &style) {
  if (_styleDepth >= MaxStyleDepth) return;
  _styleStack[_styleDepth].tag = tag;
  _styleStack[_styleDepth].style = style;
  _styleDepth++;
  _currentStyle = style;
}

void BrowserDocument::popStyle(const String &tag) {
  for (int8_t index = _styleDepth - 1; index >= 0; --index) {
    if (_styleStack[index].tag != tag) continue;
    _styleDepth = index;
    _currentStyle = index > 0 ? _styleStack[index - 1].style :
      Browser::TextStyle{TFT_BLACK, TFT_WHITE, false, TFT_WHITE, false,
                         false, false, false, false, 0, false};
    return;
  }
}

String BrowserDocument::lower(const String &value) const {
  String result = value;
  result.toLowerCase();
  return result;
}

bool BrowserDocument::isBlockTag(const String &tag) const {
  return tag == "body" || tag == "p" || tag == "div" || tag == "section" || tag == "article" ||
         tag == "header" || tag == "footer" || tag == "main" || tag == "nav" || tag == "h1" ||
         tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" || tag == "ul" ||
         tag == "ol" || tag == "li" || tag == "table" || tag == "tr" || tag == "td" || tag == "th" ||
         tag == "blockquote" || tag == "hr";
}
void BrowserDocument::appendText(const String &text) {
  String decoded = decodeEntities(text);
  String word;
  for (uint16_t i = 0; i <= decoded.length(); i++) {
    char value = i < decoded.length() ? decoded[i] : ' ';
    if (value != ' ' && value != '\r' && value != '\n' && value != '\t') {
      word += value;
      continue;
    }
    if (word.length()) {
      appendWord(word, _currentStyle);
      word = "";
    }
    if (value == '\n' || value == '\r') appendBreak();
    else _pendingSpace = true;
  }
}

void BrowserDocument::appendBreak() {
  _pendingSpace = false;
  if (_rowCount && !_rows[_rowCount - 1].image && _rows[_rowCount - 1].runCount) {
    _rows[_rowCount - 1].contentWidth = _parseLayout.viewportWidth;
  }
}

void BrowserDocument::appendImage(const String &label, const TextStyle &sourceStyle, bool linked,
                                uint8_t linkIndex, bool fullWidth, int16_t imageIndex) {
  // Image labels from the web are untrusted and can be unexpectedly large.
  // Keep placeholders allocation-free: the image record already owns the URL
  // needed for jobs and direct taps, while the UI uses a fixed label.
  (void)label;
  TextStyle style = sourceStyle;
  style.link = linked;
  style.linkIndex = linkIndex;
  if (!fullWidth) {
    appendInlineImage(label, style, imageIndex);
    return;
  }
  appendBreak();
  if (_rowCount >= MaxRows) return;
  VisualRow &row = _rows[_rowCount++];
  row.image = true;
  row.formControlIndex = -1;
  row.imageLink = linked;
  row.imageLinkIndex = linkIndex;
  row.imageIndex = imageIndex;
  row.style = style;
  row.contentWidth = 0;
  row.runCount = 0;
}

void BrowserDocument::appendInlineImage(const String &label, const TextStyle &style, int16_t imageIndex) {
  (void)label;
  const int16_t maxWidth = _parseLayout.viewportWidth - _parseLayout.sidePadding * 2;
  const bool hasImageSlot = imageIndex >= 0 && imageIndex < _embeddedImageCount;
  const bool hasThumbnail = hasImageSlot && _embeddedImages[imageIndex].pixels;
  int16_t width = hasThumbnail ? _embeddedImages[imageIndex].width
                                : _parseLayout.imagePlaceholderWidth;
  beginTextRow();
  if (_rowCount == 0) return;

  VisualRow *row = &_rows[_rowCount - 1];
  if (_pendingSpace && row->contentWidth > 0) {
    const int16_t spaceWidth = textWidth(" ", style);
    if (row->contentWidth + spaceWidth + width > maxWidth || row->runCount + 2 > MaxRunsPerRow) {
      row->contentWidth = _parseLayout.viewportWidth;
      beginTextRow();
      if (_rowCount == 0) return;
      row = &_rows[_rowCount - 1];
    } else {
      appendRun(" ", style);
      row = &_rows[_rowCount - 1];
    }
  }
  _pendingSpace = false;

  if ((row->contentWidth > 0 && row->contentWidth + width > maxWidth) ||
      row->runCount >= MaxRunsPerRow) {
    row->contentWidth = _parseLayout.viewportWidth;
    beginTextRow();
    if (_rowCount == 0) return;
    row = &_rows[_rowCount - 1];
  }
  VisualRun &run = row->runs[row->runCount++];
  run.image = true;
  run.style = style;
  run.imageIndex = imageIndex;
  run.x = row->contentWidth;
  run.width = width;
  row->contentWidth += width;
}

void BrowserDocument::appendStub(const String &label) {
  appendBreak();
  appendText(String("(") + label + ")");
  appendBreak();
}

void BrowserDocument::appendFormControl(uint8_t index, const TextStyle &style) {
  appendBreak();
  if (index >= _formControlCount || _rowCount >= MaxRows) return;
  VisualRow &row = _rows[_rowCount++];
  row = {};
  row.image = false;
  row.formControlIndex = index;
  row.imageIndex = -1;
  row.style = style;
  row.contentWidth = _parseLayout.viewportWidth;
  _pendingSpace = false;
}

int8_t BrowserDocument::addForm(const String &attributes) {
  if (_formCount >= MaxForms) return -1;
  FormData &form = _forms[_formCount];
  form = {};
  form.action = decodeEntities(attributeValue(attributes, "action"));
  String method = lower(trimCopy(attributeValue(attributes, "method")));
  form.method = method.length() == 0 || method == "get" ? FormMethod::Get :
                (method == "post" ? FormMethod::Post : FormMethod::Unsupported);
  String enctype = lower(trimCopy(attributeValue(attributes, "enctype")));
  const int semicolon = enctype.indexOf(';');
  if (semicolon >= 0) enctype.remove(semicolon);
  enctype.trim();
  form.urlEncoded = enctype.length() == 0 || enctype == "application/x-www-form-urlencoded";
  return _formCount++;
}

int8_t BrowserDocument::addFormControl(FormControlKind kind, int8_t formIndex, const String &attributes,
                                     const TextStyle &style, const String &fallbackLabel) {
  if (_formControlCount >= MaxFormControls) {
    appendStub("form control limit reached");
    return -1;
  }
  FormControl &control = _formControls[_formControlCount];
  control = {};
  control.kind = kind;
  control.formIndex = formIndex;
  control.label = decodeEntities(fallbackLabel);
  control.name = decodeEntities(attributeValue(attributes, "name"));
  control.id = decodeEntities(attributeValue(attributes, "id"));
  if (!control.label.length()) control.label = decodeEntities(attributeValue(attributes, "aria-label"));
  if (!control.label.length()) {
    control.label = decodeEntities(attributeValue(attributes, "placeholder"));
    control.labelIsPlaceholder = control.label.length() > 0;
  }
  if (!control.label.length()) control.label = control.name.length() ? control.name : control.id;
  control.value = decodeEntities(attributeValue(attributes, "value"));
  if ((kind == FormControlKind::Checkbox || kind == FormControlKind::Radio) && !control.value.length()) control.value = "on";
  control.defaultValue = control.value;
  control.checked = hasAttribute(attributes, "checked");
  control.defaultChecked = control.checked;
  control.disabled = hasAttribute(attributes, "disabled");
  control.firstOption = _formOptionCount;
  control.optionCount = 0;
  control.selectedOption = 0;
  control.defaultSelectedOption = 0;
  const uint8_t index = _formControlCount++;
  appendFormControl(index, style);
  return index;
}

void BrowserDocument::addSelectOption(uint8_t controlIndex, const String &attributes, const String &text) {
  if (controlIndex >= _formControlCount || _formOptionCount >= MaxFormOptions) return;
  FormControl &control = _formControls[controlIndex];
  if (control.kind != FormControlKind::Select || control.optionCount >= 255) return;
  FormOption &option = _formOptions[_formOptionCount++];
  option = {};
  option.controlIndex = controlIndex;
  option.label = decodeEntities(trimCopy(text));
  option.value = decodeEntities(attributeValue(attributes, "value"));
  if (!option.value.length()) option.value = option.label;
  const uint8_t optionIndex = control.optionCount++;
  if (hasAttribute(attributes, "selected")) {
    control.selectedOption = optionIndex;
    control.defaultSelectedOption = optionIndex;
  }
}

bool BrowserDocument::hasAttribute(const String &attributes, const char *name) const {
  String wanted = lower(String(name));
  uint16_t index = 0;
  while (index < attributes.length()) {
    while (index < attributes.length() && (isspace((unsigned char)attributes[index]) || attributes[index] == '/')) index++;
    const uint16_t start = index;
    while (index < attributes.length() && !isspace((unsigned char)attributes[index]) &&
           attributes[index] != '=' && attributes[index] != '/') index++;
    if (start == index) { index++; continue; }
    const String attributeName = lower(attributes.substring(start, index));
    while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
    if (attributeName == wanted) return true;
    if (index < attributes.length() && attributes[index] == '=') {
      index++;
      while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
      if (index < attributes.length() && (attributes[index] == '\'' || attributes[index] == '\"')) {
        const char quote = attributes[index++];
        while (index < attributes.length() && attributes[index] != quote) index++;
        if (index < attributes.length()) index++;
      } else {
        while (index < attributes.length() && !isspace((unsigned char)attributes[index])) index++;
      }
    }
  }
  return false;
}

String BrowserDocument::formStubLabel(const String &tag, const String &attributes) const {
  String name = decodeEntities(attributeValue(attributes, "name"));
  if (!name.length()) name = decodeEntities(attributeValue(attributes, "id"));
  if (!name.length()) name = decodeEntities(attributeValue(attributes, "aria-label"));
  if (!name.length() && tag == "form") name = decodeEntities(attributeValue(attributes, "action"));
  if (!name.length() && tag == "input") name = decodeEntities(attributeValue(attributes, "type"));
  name.trim();
  return name.length() ? tag + ": " + name : tag;
}

void BrowserDocument::appendWord(const String &word, const TextStyle &style) {
  if (_renderedCharacters >= 16384) return;
  String remaining = word;
  const uint16_t renderBudget = 16384 - _renderedCharacters;
  if (remaining.length() > renderBudget) {
    uint16_t safeEnd = renderBudget;
    while (safeEnd && ((uint8_t)remaining[safeEnd] & 0xc0) == 0x80) safeEnd--;
    remaining.remove(safeEnd);
  }
  const int16_t maxWidth = _parseLayout.viewportWidth - _parseLayout.sidePadding * 2;
  while (remaining.length() && _rowCount < MaxRows) {
    beginTextRow();
    VisualRow &row = _rows[_rowCount - 1];
    if (_pendingSpace && row.contentWidth > 0) {
      int16_t spaceWidth = textWidth(" ", style);
      if (row.contentWidth + spaceWidth >= maxWidth) {
        row.contentWidth = _parseLayout.viewportWidth;
        beginTextRow();
      } else {
        appendRun(" ", style);
      }
    }
    _pendingSpace = false;
    VisualRow &target = _rows[_rowCount - 1];
    int16_t available = maxWidth - target.contentWidth;
    int16_t wholeWidth = textWidth(remaining, style);
    const VisualRun *mergedRun = target.runCount && !target.runs[target.runCount - 1].image &&
                                 stylesEqual(target.runs[target.runCount - 1].style, style)
      ? &target.runs[target.runCount - 1] : nullptr;
    if (mergedRun) {
      wholeWidth = textWidth(mergedRun->text + remaining, style) - mergedRun->width;
    }
    if (wholeWidth <= available) {
      appendRun(remaining, style);
      _renderedCharacters += remaining.length();
      return;
    }
    String part;
    while (part.length() < remaining.length()) {
      const uint8_t characterBytes = utf8Bytes(remaining, part.length());
      String candidate = part + remaining.substring(part.length(), part.length() + characterBytes);
      int16_t candidateWidth = mergedRun
        ? textWidth(mergedRun->text + candidate, style) - mergedRun->width
        : textWidth(candidate, style);
      if (candidateWidth > available) break;
      part += remaining.substring(part.length(), part.length() + characterBytes);
    }
    if (part.length() == 0) {
      if (target.contentWidth > 0) {
        target.contentWidth = _parseLayout.viewportWidth;
        continue;
      }
      part = remaining.substring(0, utf8Bytes(remaining, 0));
    }
    appendRun(part, style);
    _renderedCharacters += part.length();
    remaining.remove(0, part.length());
    if (remaining.length()) _rows[_rowCount - 1].contentWidth = _parseLayout.viewportWidth;
  }
}

void BrowserDocument::appendRun(const String &text, const TextStyle &style) {
  if (text.length() == 0 || _rowCount == 0) return;
  VisualRow &row = _rows[_rowCount - 1];
  if (row.runCount && !row.runs[row.runCount - 1].image &&
      stylesEqual(row.runs[row.runCount - 1].style, style)) {
    VisualRun &run = row.runs[row.runCount - 1];
    const int16_t previousWidth = run.width;
    run.text += text;
    // Free-font textWidth() uses the glyph's visible bounds for the final
    // character, while text within a drawString() advances by xAdvance.
    // Summing separate fragments therefore underestimates a merged run and
    // makes the next differently styled run paint over its end.
    run.width = textWidth(run.text, style);
    row.contentWidth += run.width - previousWidth;
  } else if (row.runCount < MaxRunsPerRow) {
    const int16_t width = textWidth(text, style);
    VisualRun &run = row.runs[row.runCount++];
    run.image = false;
    run.text = text;
    run.style = style;
    run.imageIndex = -1;
    run.x = row.contentWidth;
    run.width = width;
    row.contentWidth += width;
  } else {
    row.contentWidth = _parseLayout.viewportWidth;
    beginTextRow();
    appendRun(text, style);
    return;
  }
}

void BrowserDocument::beginTextRow() {
  const int16_t maxWidth = _parseLayout.viewportWidth - _parseLayout.sidePadding * 2;
  if (_rowCount && !_rows[_rowCount - 1].image && _rows[_rowCount - 1].contentWidth < maxWidth) return;
  if (_rowCount >= MaxRows) return;
  VisualRow &row = _rows[_rowCount++];
  row.image = false;
  row.formControlIndex = -1;
  row.imageLabel = "";
  row.imageLink = false;
  row.imageLinkIndex = 0;
  row.imageIndex = -1;
  row.style = _currentStyle;
  row.contentWidth = 0;
  row.runCount = 0;
}



String BrowserDocument::decodeEntities(const String &value) const {
  String result = value;
  result.replace("&nbsp;", " ");
  result.replace("&amp;", "&");
  result.replace("&lt;", "<");
  result.replace("&gt;", ">");
  result.replace("&quot;", "\"");
  result.replace("&#39;", "'");
  result.replace("&apos;", "'");
  String decoded;
  for (uint32_t index = 0; index < result.length();) {
    if (result[index] == '&' && index + 3 < result.length() && result[index + 1] == '#') {
      const int end = result.indexOf(';', index + 2);
      if (end > static_cast<int>(index) + 2) {
        String number = result.substring(index + 2, end);
        const bool hexadecimal = number.startsWith("x") || number.startsWith("X");
        if (hexadecimal) number.remove(0, 1);
        char *numberEnd = nullptr;
        uint32_t codepoint = strtoul(number.c_str(), &numberEnd, hexadecimal ? 16 : 10);
        if (number.length() && numberEnd && *numberEnd == '\0' && codepoint <= 0x10ffffUL) {
          if (codepoint >= 0xd800 && codepoint <= 0xdfff) codepoint = 0xfffd;
          if (codepoint < 0x80) decoded += static_cast<char>(codepoint);
          else if (codepoint < 0x800) {
            decoded += static_cast<char>(0xc0 | (codepoint >> 6));
            decoded += static_cast<char>(0x80 | (codepoint & 0x3f));
          } else if (codepoint < 0x10000) {
            decoded += static_cast<char>(0xe0 | (codepoint >> 12));
            decoded += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
            decoded += static_cast<char>(0x80 | (codepoint & 0x3f));
          } else {
            decoded += static_cast<char>(0xf0 | (codepoint >> 18));
            decoded += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
            decoded += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
            decoded += static_cast<char>(0x80 | (codepoint & 0x3f));
          }
          index = end + 1;
          continue;
        }
      }
    }
    decoded += result[index++];
  }
  return decoded;
}

Browser::TextStyle BrowserDocument::styleForTag(const String &tag, const String &attributes,
                                                const BrowserDocumentContext &context) {
  Browser::TextStyle style = _currentStyle;
  const bool block = isBlockTag(tag);
  if (tag == "small") style.size = BrowserText::Size::Small;
  if (tag == "big" || tag == "h1" || tag == "h2") style.size = BrowserText::Size::Large;
  if (tag == "img") style.fullWidthImage = false;
  if (tag == "b" || tag == "strong") style.bold = true;
  if (tag == "i" || tag == "em") style.italic = true;
  if (tag == "a") {
    style.underline = true;
    style.link = true;
    style.linkIndex = 0;
    String href = decodeEntities(attributeValue(attributes, "href"));
    href.trim();
    String normalized;
    if (href.length() && !href.startsWith("#") && context.resolveUrl &&
        context.resolveUrl(context.resolverContext, href, normalized)) style.linkIndex = registerLink(normalized);
  }
  const String color = attributeValue(attributes, "color");
  if (color.length()) style.color = parseColor(color, style.color);
  const String background = attributeValue(attributes, "bgcolor");
  if (background.length()) applyBackground(style, background);
  if (tag == "img" && lower(trimCopy(attributeValue(attributes, "width"))) == "100%") style.fullWidthImage = true;
  applyCssRules(style, tag, attributes);
  StyleOverride inlineOverride = {};
  parseCssDeclarations(attributeValue(attributes, "style"), inlineOverride);
  applyStyleOverride(style, inlineOverride, tag == "img");
  if (block && style.hasBackground) {
    style.blockBackgroundColor = style.backgroundColor;
    style.hasBlockBackground = true;
  }
  return style;
}
