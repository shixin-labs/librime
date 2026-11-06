#pragma once
// Scratch-only, little-endian sidecar. Load only during schema initialization;
// generation-manifest authentication/versioning remains a production gate.
#include "subtree_bounds.h"
#include <cmath>
#include <cstring>
#include <fstream>
namespace shixin_typo {
struct BoundIdentity {
  uint32_t checksum, syllables, entries;
  uint64_t table_bytes;
  bool operator==(const BoundIdentity &b) const {
    return checksum == b.checksum && syllables == b.syllables &&
           entries == b.entries && table_bytes == b.table_bytes;
  }
};
inline bool Write32(std::ostream &out, uint32_t value) {
  char bytes[4];
  for (int i = 0; i < 4; ++i)
    bytes[i] = char(value >> (i * 8));
  return bool(out.write(bytes, 4));
}
inline bool Read32(std::istream &in, uint32_t *value) {
  unsigned char bytes[4];
  if (!in.read(reinterpret_cast<char *>(bytes), 4))
    return false;
  *value = 0;
  for (int i = 0; i < 4; ++i)
    *value |= uint32_t(bytes[i]) << (i * 8);
  return true;
}
inline bool SaveBounds(const rime::path &path, const BoundIdentity &identity,
                       const std::vector<PrefixBound> &bounds) {
  if (std::filesystem::exists(path))
    return false; // Offline artifact: never overwrite.
  std::ofstream out(static_cast<const std::filesystem::path &>(path),
                    std::ios::binary);
  out.write("SX3BND01", 8);
  if (!Write32(out, identity.checksum) || !Write32(out, identity.syllables) ||
      !Write32(out, identity.entries) ||
      !Write32(out, uint32_t(identity.table_bytes)) ||
      !Write32(out, uint32_t(identity.table_bytes >> 32)))
    return false;
  for (const auto &bound : bounds) {
    if (!Write32(out, uint32_t(bound.best_three.size())))
      return false;
    for (float value : bound.best_three) {
      uint32_t bits;
      std::memcpy(&bits, &value, 4);
      if (!Write32(out, bits))
        return false;
    }
  }
  out.flush();
  return bool(out);
}
inline bool LoadBounds(const rime::path &path, const BoundIdentity &expected,
                       const rime::table::Index *index,
                       std::vector<PrefixBound> *bounds) {
  std::ifstream in(static_cast<const std::filesystem::path &>(path),
                   std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 28 || in.tellg() > 1024 * 1024)
    return false;
  in.seekg(0);
  char magic[8];
  if (!in.read(magic, 8) || std::memcmp(magic, "SX3BND01", 8))
    return false;
  BoundIdentity actual{};
  uint32_t low, high;
  if (!Read32(in, &actual.checksum) || !Read32(in, &actual.syllables) ||
      !Read32(in, &actual.entries) || !Read32(in, &low) || !Read32(in, &high))
    return false;
  actual.table_bytes = uint64_t(low) | (uint64_t(high) << 32);
  if (!(actual == expected) || !index || index->size != actual.syllables)
    return false;
  bounds->clear();
  bounds->resize(index->size);
  for (size_t a = 0; a < index->size; ++a) {
    auto next = index->at[a].next_level.get();
    const auto *children = next ? &next->trunk() : nullptr;
    uint32_t count;
    if (!Read32(in, &count) || count != (children ? children->size : 0))
      return false;
    auto &bound = (*bounds)[a];
    bound.children = children;
    bound.best_three.resize(count);
    for (float &value : bound.best_three) {
      uint32_t bits;
      if (!Read32(in, &bits))
        return false;
      std::memcpy(&value, &bits, 4);
      if (std::isnan(value) || value == std::numeric_limits<float>::infinity())
        return false;
      bound.best = std::max(bound.best, value);
    }
  }
  return in.peek() == std::char_traits<char>::eof();
}
} // namespace shixin_typo
