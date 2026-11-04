// Contract test for the disabled-app list that AppSettings stores in NVS.
#include "AppDisabledList.h"

#include <stdio.h>
#include <string>

namespace {
int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    ++failures;
    printf("FAIL: %s\n", what);
  }
}

std::string encoded(const AppDisabledList &list) {
  static char buffer[AppDisabledList::MaxEncodedBytes];
  return std::string(buffer, list.encode(buffer, sizeof(buffer)));
}
}  // namespace

int main() {
  AppDisabledList list;
  check(list.count() == 0 && encoded(list).empty(), "starts empty");
  check(!list.contains("counter"), "unknown id is enabled");

  check(list.setDisabled("counter", true), "disable changes");
  check(!list.setDisabled("counter", true), "disable twice is no change");
  check(list.setDisabled("dvach", true), "disable second");
  check(list.contains("counter") && list.contains("dvach"), "both disabled");
  check(encoded(list) == "counter\ndvach", "encoding joins with newline");

  check(list.setDisabled("counter", false), "enable changes");
  check(!list.setDisabled("counter", false), "enable twice is no change");
  check(encoded(list) == "dvach", "enable removes");

  check(!list.setDisabled("Bad", true), "uppercase id refused");
  check(!list.setDisabled("", true), "empty id refused");
  check(!list.setDisabled("1abc", true), "leading digit refused");
  check(!list.setDisabled("abcdefghijabcdefghijabcdefghijabc", true), "33-byte id refused");
  check(list.setDisabled("abcdefghijabcdefghijabcdefghijab", true), "32-byte id accepted");

  AppDisabledList decoded;
  const std::string blob = encoded(list);
  check(decoded.decode(blob.data(), blob.size()), "round trip decodes");
  check(encoded(decoded) == blob, "round trip keeps order");
  check(decoded.decode(nullptr, 0) && decoded.count() == 0, "empty blob is empty list");

  const char *bad[] = {"a\n\nb", "a\n", "\na", "a\na", "a\nB", "a b"};
  for (const char *value : bad) {
    AppDisabledList target;
    target.setDisabled("keep", true);
    check(!target.decode(value, strlen(value)) && target.count() == 0, value);
  }

  AppDisabledList full;
  char id[8];
  for (int i = 0; i < AppDisabledList::MaxIds; ++i) {
    snprintf(id, sizeof(id), "app%d", i);
    full.setDisabled(id, true);
  }
  check(full.count() == AppDisabledList::MaxIds, "fills to the limit");
  check(full.setDisabled("extra", true), "disable past the limit");
  check(full.count() == AppDisabledList::MaxIds, "count stays at the limit");
  check(!full.contains("app0") && full.contains("app1") && full.contains("extra"),
        "oldest dropped when full");
  const std::string fullBlob = encoded(full);
  check(fullBlob.size() <= AppDisabledList::MaxEncodedBytes, "full blob fits");
  AppDisabledList reread;
  check(reread.decode(fullBlob.data(), fullBlob.size()) && reread.count() == full.count(),
        "full blob round trips");

  std::string tooMany = fullBlob + "\nmore";
  check(!reread.decode(tooMany.data(), tooMany.size()), "65 ids refused");

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("app_disabled_list: all checks passed\n");
  return 0;
}
