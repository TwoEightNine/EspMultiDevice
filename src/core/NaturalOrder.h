#pragma once

#include <Arduino.h>

// Case-folded natural name order shared by Files and SD-app listings:
// digit runs compare by value, so "track2" sorts before "track10".
namespace NaturalOrder {
inline char folded(char value) {
  return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

inline int compare(const char *left, size_t leftLength, const char *right, size_t rightLength) {
  size_t l = 0;
  size_t r = 0;
  while (l < leftLength && r < rightLength) {
    const char lc = left[l];
    const char rc = right[r];
    if (lc >= '0' && lc <= '9' && rc >= '0' && rc <= '9') {
      const size_t lStart = l;
      const size_t rStart = r;
      while (l < leftLength && left[l] == '0') l++;
      while (r < rightLength && right[r] == '0') r++;
      const size_t lDigits = l;
      const size_t rDigits = r;
      while (l < leftLength && left[l] >= '0' && left[l] <= '9') l++;
      while (r < rightLength && right[r] >= '0' && right[r] <= '9') r++;
      const size_t lLength = l - lDigits;
      const size_t rLength = r - rDigits;
      if (lLength != rLength) return lLength < rLength ? -1 : 1;
      for (size_t i = 0; i < lLength; i++) {
        if (left[lDigits + i] != right[rDigits + i]) return left[lDigits + i] < right[rDigits + i] ? -1 : 1;
      }
      const size_t lZeros = lDigits - lStart;
      const size_t rZeros = rDigits - rStart;
      if (lZeros != rZeros) return lZeros < rZeros ? -1 : 1;
      continue;
    }
    const char lFolded = folded(lc);
    const char rFolded = folded(rc);
    if (lFolded != rFolded) return lFolded < rFolded ? -1 : 1;
    l++;
    r++;
  }
  if (l != leftLength || r != rightLength) return l == leftLength ? -1 : 1;
  const int exact = memcmp(left, right, leftLength);
  return exact < 0 ? -1 : (exact > 0 ? 1 : 0);
}

inline int compare(const String &left, const String &right) {
  return compare(left.c_str(), left.length(), right.c_str(), right.length());
}
}  // namespace NaturalOrder
