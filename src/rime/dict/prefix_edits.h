#pragma once
#include <array>
#include <cmath>
#include <string_view>
#include <utility>
#include "edit_trace.h"

namespace shixin_typo {
struct Keyboard {
  std::array<std::array<bool, 26>, 26> adjacent{};
  Keyboard() {
    std::array<double, 26> x{}, y{};
    const char *rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const double offsets[] = {0, .25, .75};
    for (int r = 0; r < 3; r++)
      for (int c = 0; rows[r][c]; c++) {
        x[rows[r][c] - 'a'] = c + offsets[r];
        y[rows[r][c] - 'a'] = r;
      }
    for (int a = 0; a < 26; a++)
      for (int b = 0; b < 26; b++)
        adjacent[a][b] = a != b && std::hypot(x[a] - x[b], y[a] - y[b]) <= 1.3;
  }
};
// Enumerate one edit of each raw prefix, never crossing punctuation. The
// callback receives an ephemeral view; copy only a bounded result if needed.
// False means budget/callback exhaustion; callers discard partial results.
template <class Emit>
bool PrefixEditsWithTrace(std::string_view raw, const Keyboard &keyboard,
                          Emit emit, size_t cap = 256) {
  std::array<char, 8> buf{};
  size_t work = 0;
  for (size_t len = 1; len <= raw.size() && len <= buf.size(); len++) {
    if (raw[len - 1] < 'a' || raw[len - 1] > 'z')
      break;
    for (size_t i = 0; i < len; i++)
      buf[i] = raw[i];
    for (size_t i = 0; i + 1 < len; i++) {
      if (buf[i] == buf[i + 1])
        continue;
      if (work++ == cap)
        return false;
      std::swap(buf[i], buf[i + 1]);
      if (!emit(std::string_view(buf.data(), len),
                EditTrace{EditTrace::Kind::Swap,
                          uint8_t(i),
                          uint8_t(i + 2),
                          8,
                          {raw[i], raw[i + 1]},
                          {buf[i], buf[i + 1]}}))
        return false;
      std::swap(buf[i], buf[i + 1]);
    }
    for (size_t i = 0; i < len; i++) {
      char original = buf[i];
      for (char c = 'a'; c <= 'z'; c++)
        if (keyboard.adjacent[original - 'a'][c - 'a']) {
          if (work++ == cap)
            return false;
          buf[i] = c;
          if (!emit(std::string_view(buf.data(), len),
                    EditTrace{EditTrace::Kind::Neighbor,
                              uint8_t(i),
                              uint8_t(i + 1),
                              10,
                              {original, 0},
                              {c, 0}}))
            return false;
        }
      buf[i] = original;
    }
  }
  return true;
}
// Compatibility adapter for the existing enumerator experiments.
template <class Emit>
bool PrefixEdits(std::string_view raw, const Keyboard &keyboard, Emit emit,
                 size_t cap = 256) {
  return PrefixEditsWithTrace(
      raw, keyboard,
      [&](std::string_view edited, const EditTrace &trace) {
        return emit(edited, int(trace.cost));
      },
      cap);
}
} // namespace shixin_typo
