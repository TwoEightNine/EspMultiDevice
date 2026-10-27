// Host contract test for the SDK 6 JSON filter and text helpers. Build and run
// with tests/lua_net_contract/run.sh; pass a downloaded 2ch catalog.json as the
// optional argument to also filter a real document.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../src/features/apps/AppText.h"
#include "../../src/features/apps/JsonPick.h"

namespace {
int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    ++failures;
    printf("FAIL: %s\n", what);
  }
}

std::string nodeText(const JsonPick &pick, const JsonPick::Node &node) {
  return std::string(pick.strings() + node.offset, node.length);
}

std::string keyText(const JsonPick &pick, const JsonPick::Node &node) {
  if (node.keyOffset == JsonPick::NoKey) return "";
  return std::string(pick.strings() + node.keyOffset, node.keyLength);
}

// Renders kept nodes back to compact JSON-like text for comparisons.
uint32_t render(const JsonPick &pick, uint32_t index, std::string &out) {
  const JsonPick::Node &node = pick.nodes()[index];
  if (node.keyOffset != JsonPick::NoKey) out += "\"" + keyText(pick, node) + "\":";
  switch (node.type) {
    case JsonPick::Type::Null: out += "null"; return index + 1;
    case JsonPick::Type::True: out += "true"; return index + 1;
    case JsonPick::Type::False: out += "false"; return index + 1;
    case JsonPick::Type::Number: out += nodeText(pick, node); return index + 1;
    case JsonPick::Type::String: out += "\"" + nodeText(pick, node) + "\""; return index + 1;
    case JsonPick::Type::Array:
    case JsonPick::Type::Object: {
      const bool object = node.type == JsonPick::Type::Object;
      out += object ? "{" : "[";
      uint32_t child = index + 1;
      for (uint32_t i = 0; i < node.count; ++i) {
        if (i) out += ",";
        child = render(pick, child, out);
      }
      check(child == node.end, "container end matches its last descendant");
      out += object ? "}" : "]";
      return node.end;
    }
  }
  return index + 1;
}

struct Result {
  bool ok;
  std::string text;
  std::string error;
  bool truncated;
};

Result run(const std::string &json, JsonPick::Options options, size_t pieceBytes = 0) {
  JsonPick pick(malloc, free);
  Result result{false, "", "", false};
  if (!pick.begin(options)) { result.error = pick.error(); return result; }
  const uint8_t *data = reinterpret_cast<const uint8_t *>(json.data());
  bool fed = true;
  if (!pieceBytes) {
    fed = pick.feed(data, json.size());
  } else {
    for (size_t at = 0; at < json.size() && fed; at += pieceBytes) {
      const size_t count = json.size() - at < pieceBytes ? json.size() - at : pieceBytes;
      fed = pick.feed(data + at, count);
    }
  }
  result.ok = fed && pick.finish();
  if (!result.ok) { result.error = pick.error() ? pick.error() : "?"; return result; }
  render(pick, 0, result.text);
  result.truncated = pick.truncated();
  return result;
}

JsonPick::Options keep(std::initializer_list<const char *> keys) {
  JsonPick::Options options;
  for (const char *key : keys) options.keep[options.keepCount++] = key;
  return options;
}

void expectText(const std::string &json, const JsonPick::Options &options,
                const std::string &expected, const char *what) {
  const Result whole = run(json, options);
  if (!whole.ok || whole.text != expected) {
    printf("  got ok=%d text=%s error=%s\n", whole.ok, whole.text.c_str(), whole.error.c_str());
  }
  check(whole.ok && whole.text == expected, what);
  // Every split point must give the same result.
  for (size_t piece = 1; piece <= 7; ++piece) {
    const Result split = run(json, options, piece);
    check(split.ok && split.text == expected, "split feeding matches whole-buffer parsing");
  }
}

void expectError(const std::string &json, const JsonPick::Options &options, const char *what) {
  const Result result = run(json, options);
  if (result.ok) printf("  unexpectedly parsed: %s\n", result.text.c_str());
  check(!result.ok, what);
}

void jsonTests() {
  JsonPick::Options all;
  expectText("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\"}", all,
             "{\"a\":1,\"b\":[true,false,null],\"c\":\"x\"}", "keep-all round trip");
  expectText("  [ -0.5e+3 , 0, 12 ]  ", all, "[-0.5e+3,0,12]", "numbers and whitespace");
  expectText("\"\\u0416\\u00e9\\n\\\"\"", all, "\"\xD0\x96\xC3\xA9\n\"\"", "escapes decode to UTF-8");
  expectText("\"\\ud83d\\ude00\"", all, "\"\xF0\x9F\x98\x80\"", "surrogate pair decodes");
  expectText("\xEF\xBB\xBF{}", all, "{}", "UTF-8 BOM is skipped");
  expectText("\"\xD0\x9F\xD1\x80\xD0\xB8\"", all, "\"\xD0\x9F\xD1\x80\xD0\xB8\"", "raw Cyrillic passes");

  expectText("{\"keep\":{\"drop\":[1,{\"keep\":2}],\"keep\":3},\"drop\":{\"keep\":4}}", keep({"keep"}),
             "{\"keep\":{\"keep\":3}}", "filter keeps listed keys at any depth under kept parents");
  expectText("[{\"a\":1,\"b\":2},{\"b\":3}]", keep({"b"}), "[{\"b\":2},{\"b\":3}]",
             "array elements are always kept");

  JsonPick::Options items = keep({"t"});
  items.maxItems = 2;
  Result capped = run("{\"t\":[1,2,3,4]}", items);
  check(capped.ok && capped.text == "{\"t\":[1,2]}" && capped.truncated, "max_items caps arrays");

  JsonPick::Options strings;
  strings.maxString = 5;
  Result cut = run("\"\xD0\x90\xD0\x91\xD0\x92\"", strings);
  check(cut.ok && cut.text == "\"\xD0\x90\xD0\x91\"" && cut.truncated,
        "max_string cuts on a UTF-8 boundary");

  expectError("{\"a\":1,}", all, "trailing comma in object");
  expectError("[1,]", all, "trailing comma in array");
  expectError("[01]", all, "leading zero");
  expectError("{\"a\" 1}", all, "missing colon");
  expectError("\"\\ud83d\"", all, "lone high surrogate");
  expectError("\"\\ude00\"", all, "lone low surrogate");
  expectError("\"\xC0\xAF\"", all, "overlong UTF-8");
  expectError("\"\xED\xA0\x80\"", all, "UTF-8 encoded surrogate");
  expectError("\"a\nb\"", all, "control character in string");
  expectError("{} {}", all, "two root values");
  expectError("{\"a\":[1,2", all, "truncated document");
  expectError("tru", all, "truncated literal");
  expectError("{\"skip\":[1,}", keep({"keep"}), "dropped members are still validated");

  std::string deep;
  for (int i = 0; i < 17; ++i) deep += "[";
  for (int i = 0; i < 17; ++i) deep += "]";
  expectError(deep, all, "kept depth over 16");
  std::string skippedDeep = "{\"skip\":";
  for (int i = 0; i < 40; ++i) skippedDeep += "[";
  for (int i = 0; i < 40; ++i) skippedDeep += "]";
  skippedDeep += "}";
  expectText(skippedDeep, keep({"keep"}), "{}", "dropped values may nest deeper than kept ones");

  JsonPick::Options small;
  small.maxNodes = 3;
  expectError("[1,2,3]", small, "node limit");
  JsonPick::Options tiny;
  tiny.maxStringBytes = 4;
  expectError("[\"hello\"]", tiny, "string arena limit");
}

void textTests() {
  check(AppText::urlEncode("a b/\xD0\x96~", 7) == "a%20b%2F%D0%96~", "URL encoding");
  std::string decoded;
  check(AppText::urlDecode("a+b%2Fc", 7, true, decoded) && decoded == "a b/c", "form decoding");
  check(!AppText::urlDecode("%4", 2, false, decoded), "short escape rejected");
  check(!AppText::urlDecode("%zz", 3, false, decoded), "non-hex escape rejected");

  bool truncated = false;
  const std::string post =
    "Hello&nbsp;<b>world</b><br><br><br><br>Line &amp; &#1046;&#x416; "
    "<a href=\"/pr/res/1.html\">&gt;&gt;1 (OP)</a><script>x()</script>  end";
  const std::string text = AppText::htmlToText(post.data(), post.size(), 1000, truncated);
  check(text == "Hello world\n\n\nLine & \xD0\x96\xD0\x96 >>1 (OP) end" && !truncated,
        "HTML to text");
  if (text != "Hello world\n\n\nLine & \xD0\x96\xD0\x96 >>1 (OP) end") printf("  got: %s\n", text.c_str());
  const std::string cyrillic = "\xD0\x90\xD0\x91\xD0\x92";
  const std::string clipped = AppText::htmlToText(cyrillic.data(), cyrillic.size(), 3, truncated);
  check(clipped == "\xD0\x90" && truncated, "HTML text cut on a UTF-8 boundary");
  check(AppText::utf8Prefix("\xD0\x90\xD0\x91", 4, 3) == 2, "UTF-8 prefix");
  check(AppText::validUtf8("\xF0\x9F\x98\x80", 4) && !AppText::validUtf8("\xF0\x9F\x98", 3),
        "UTF-8 validation");
}

// A catalog shaped like 2ch's: adverts, board metadata, threads with HTML
// comments, files, and Cyrillic subjects.
std::string syntheticCatalog(int threads) {
  std::string json = "{\"advert_top_image\":\"/banners/x.png\",\"board\":{\"id\":\"pr\","
                     "\"name\":\"\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB3\xD1\x80\xD0\xB0\xD0\xBC\xD0\xBC"
                     "\xD0\xB8\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5\","
                     "\"max_pages\":10,\"enable_posting\":true},\"threads\":[";
  for (int i = 0; i < threads; ++i) {
    if (i) json += ",";
    json += "{\"num\":" + std::to_string(3700000 + i) +
            ",\"subject\":\"\xD0\xA2\xD1\x80\xD0\xB5\xD0\xB4 \\u2116" + std::to_string(i) + "\","
            "\"comment\":\"" + std::string(400, 'x') + "<br>\\u041f\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\","
            "\"posts_count\":" + std::to_string(i * 3) + ",\"files_count\":1,"
            "\"files\":[{\"name\":\"a.jpg\",\"thumbnail\":\"/pr/thumb/1.jpg\",\"width\":800}],"
            "\"lasthit\":1793024255,\"sticky\":" + std::to_string(i == 0 ? 1 : 0) +
            ",\"closed\":0,\"name\":\"\xD0\x90\xD0\xBD\xD0\xBE\xD0\xBD\xD0\xB8\xD0\xBC\"}";
  }
  json += "]}";
  return json;
}

JsonPick::Options catalogOptions() {
  JsonPick::Options options = keep({"board", "name", "threads", "num", "subject", "comment",
                                    "posts_count", "files_count", "lasthit", "sticky", "closed"});
  options.maxString = 200;
  options.maxItems = 250;
  return options;
}

void catalogTests(const char *path) {
  const std::string synthetic = syntheticCatalog(204);
  const Result result = run(synthetic, catalogOptions(), 1460);
  check(result.ok, "synthetic catalog parses");
  check(result.text.find("\"files\"") == std::string::npos, "dropped members are absent");
  check(result.text.find("\"subject\":\"\xD0\xA2\xD1\x80\xD0\xB5\xD0\xB4 \xE2\x84\x96" "0\"") != std::string::npos,
        "Cyrillic subject with an escape survives");
  check(result.truncated, "long comments report truncation");
  if (!path) return;
  FILE *file = fopen(path, "rb");
  if (!file) { printf("cannot open %s\n", path); ++failures; return; }
  std::string real;
  char buffer[65536];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) real.append(buffer, count);
  fclose(file);
  JsonPick pick(malloc, free);
  pick.begin(catalogOptions());
  bool ok = true;
  for (size_t at = 0; at < real.size() && ok; at += 1460) {
    const size_t piece = real.size() - at < 1460 ? real.size() - at : 1460;
    ok = pick.feed(reinterpret_cast<const uint8_t *>(real.data()) + at, piece);
  }
  ok = ok && pick.finish();
  printf("real catalog: %zu bytes -> %u nodes, %u string bytes, ok=%d error=%s\n",
         real.size(), pick.nodeCount(), pick.stringBytes(), ok, pick.error() ? pick.error() : "-");
  check(ok, "real catalog parses within limits");
}
}

int main(int argc, char **argv) {
  jsonTests();
  textTests();
  catalogTests(argc > 1 ? argv[1] : nullptr);
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("lua_net_contract: all checks passed\n");
  return 0;
}
