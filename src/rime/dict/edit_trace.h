#pragma once
#include <cstdint>
namespace shixin_typo {
// Scratch provenance: byte offsets are relative to the searched prefix until
// the graph offsets them into raw composition coordinates. Costs are heuristic
// units (8/10), not calibrated probabilities.
struct EditTrace {
  enum class Kind : uint8_t { None, Swap, Neighbor };
  Kind kind = Kind::None;
  uint8_t begin = 0, end = 0, cost = 0;
  char from[2]{}, to[2]{};
};
} // namespace shixin_typo
