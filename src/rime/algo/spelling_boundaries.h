#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace shixin_typo {
struct Boundaries {
  uint64_t forward = 0, backward = 0;
  bool Allows(size_t begin, size_t end) const {
    return begin <= 32 && end <= 32 && (forward & (uint64_t(1) << begin)) &&
           (backward & (uint64_t(1) << end));
  }
};
// Enumerate exact prism-prefix lengths at each position. A single edited
// syllable can only join a zero-edit prefix to a zero-edit suffix. Delimiters
// are separate zero-edit transitions and cannot be crossed by the edit itself.
template <class Prefixes>
Boundaries ExactBoundaries(std::string_view input, std::string_view delimiters,
                           Prefixes prefixes) {
  if (input.size() > 32)
    return {};
  std::array<uint64_t, 33> edges{};
  for (size_t pos = 0; pos < input.size(); pos++) {
    if (delimiters.find(input[pos]) != std::string_view::npos) {
      edges[pos] = uint64_t(1) << (pos + 1);
      continue;
    }
    prefixes(pos, [&](size_t len) {
      if (len && pos + len <= input.size())
        edges[pos] |= uint64_t(1) << (pos + len);
    });
  }
  Boundaries b{1, uint64_t(1) << input.size()};
  for (size_t p = 0; p < input.size(); p++)
    if (b.forward & (uint64_t(1) << p))
      b.forward |= edges[p];
  for (size_t p = input.size(); p-- > 0;)
    if (edges[p] & b.backward)
      b.backward |= uint64_t(1) << p;
  return b;
}
} // namespace shixin_typo
