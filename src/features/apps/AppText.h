#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// Byte-exact text helpers shared by the Lua SDK. They do no allocation beyond
// the returned string and have no Arduino dependency, so the host contract
// test compiles them as they are.
namespace AppText {

// Percent-encodes every byte outside the RFC 3986 unreserved set.
std::string urlEncode(const char *text, size_t length);
// Decodes %XX escapes; with `plusAsSpace`, also `+`. False on a malformed
// escape.
bool urlDecode(const char *text, size_t length, bool plusAsSpace, std::string &out);

// Plain text of an HTML fragment such as a forum post: tags are dropped,
// `<br>` and block ends become line breaks, entities are decoded, runs of
// spaces collapse, and at most two consecutive empty lines remain. The result
// is cut on a UTF-8 boundary at `maxBytes` and `truncated` reports a cut.
std::string htmlToText(const char *html, size_t length, size_t maxBytes, bool &truncated);

// Length of the longest prefix of `text` no longer than `maxBytes` that ends
// on a UTF-8 code point boundary.
size_t utf8Prefix(const char *text, size_t length, size_t maxBytes);
bool validUtf8(const char *text, size_t length);
void appendUtf8(std::string &out, uint32_t codepoint);

}  // namespace AppText
