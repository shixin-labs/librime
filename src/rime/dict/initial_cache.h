#pragma once
#include "subtree_bounds_io.h"
namespace shixin_typo {
struct CachedReading {
  uint32_t offset = 0;
  std::array<uint16_t, 3> code{};
  uint16_t reserved = 0;
  float weight = 0;
};
static_assert(sizeof(CachedReading) == 16);
inline bool BetterReading(const CachedReading &a, const CachedReading &b) {
  if (a.weight != b.weight)
    return a.weight > b.weight;
  if (a.code != b.code)
    return a.code < b.code;
  return a.offset < b.offset;
}
struct InitialCache {
  static constexpr size_t codes = 26 * 26 * 26;
  std::vector<uint32_t> offsets;
  std::vector<CachedReading> entries;
};
inline size_t InitialCode(std::string_view raw) {
  return (raw[0] - 'a') * 26 * 26 + (raw[1] - 'a') * 26 + raw[2] - 'a';
}
struct CacheResult {
  std::array<CachedReading, 8> entries{};
  size_t count = 0, probes = 0;
  bool complete = true;
};
class InitialCacheSearch {
  Keyboard keyboard_;

public:
  CacheResult Exact(std::string_view corrected, const InitialCache& cache,
                    size_t cap, std::chrono::steady_clock::time_point deadline) const {
    CacheResult result;
    if(corrected.size()!=3 || cache.offsets.size()!=InitialCache::codes+1) { result.complete=false; return result; }
    for(char c:corrected) if(c<'a'||c>'z') { result.complete=false; return result; }
    const size_t bucket=InitialCode(corrected);
    for(size_t i=cache.offsets[bucket]; i<cache.offsets[bucket+1] && result.count<result.entries.size(); ++i) {
      if(result.probes==cap || std::chrono::steady_clock::now()>=deadline || i>=cache.entries.size()) { result.complete=false; return result; }
      ++result.probes;
      result.entries[result.count++]=cache.entries[i];
    }
    return result;
  }
  CacheResult Search(std::string_view raw, const InitialCache &cache,
                     size_t cap = 32768,
                     std::chrono::steady_clock::time_point deadline =
                         std::chrono::steady_clock::time_point::max()) const {
    CacheResult result;
    if (cap == 0 || deadline <= std::chrono::steady_clock::now()) {
      result.complete = false;
      return result;
    }
    if (raw.size() != 3 || cache.offsets.size() != InitialCache::codes + 1) {
      result.complete = false;
      return result;
    }
    for (char c : raw)
      if (c < 'a' || c > 'z') {
        result.complete = false;
        return result;
      }
    std::array<char, 3> corrected{raw[0], raw[1], raw[2]};
    for (size_t pos = 0; pos < 3; ++pos)
      for (char key = 'a'; key <= 'z'; ++key) {
        if (!keyboard_.adjacent[raw[pos] - 'a'][key - 'a'])
          continue;
        corrected[pos] = key;
        size_t bucket = InitialCode(std::string_view(corrected.data(), 3));
        corrected[pos] = raw[pos];
        for (size_t i = cache.offsets[bucket]; i < cache.offsets[bucket + 1];
             ++i) {
          if (result.probes == cap ||
              ((result.probes & 31) == 0 &&
               deadline != std::chrono::steady_clock::time_point::max() &&
               std::chrono::steady_clock::now() >= deadline)) {
            result.complete = false;
            return result;
          }
          ++result.probes;
          auto candidate = cache.entries[i];
          size_t at = 0;
          while (at < result.count &&
                 !BetterReading(candidate, result.entries[at]))
            ++at;
          if (at == 8)
            continue;
          if (result.count < 8)
            ++result.count;
          for (size_t j = result.count - 1; j > at; --j)
            result.entries[j] = result.entries[j - 1];
          result.entries[at] = candidate;
        }
      }
    return result;
  }
};
inline bool SaveInitialCache(const rime::path &path, const BoundIdentity &id,
                             const InitialCache &cache) {
  if (std::filesystem::exists(path))
    return false;
  std::ofstream out(static_cast<const std::filesystem::path &>(path),
                    std::ios::binary);
  out.write("SX3TOP08", 8);
  if (!Write32(out, id.checksum) || !Write32(out, id.syllables) ||
      !Write32(out, id.entries) || !Write32(out, uint32_t(id.table_bytes)) ||
      !Write32(out, uint32_t(id.table_bytes >> 32)) ||
      !Write32(out, uint32_t(cache.entries.size())))
    return false;
  for (auto offset : cache.offsets)
    if (!Write32(out, offset))
      return false;
  for (const auto &entry : cache.entries) {
    uint32_t bits;
    std::memcpy(&bits, &entry.weight, 4);
    if (!Write32(out, entry.offset) ||
        !Write32(out,
                 uint32_t(entry.code[0]) | (uint32_t(entry.code[1]) << 16)) ||
        !Write32(out, entry.code[2]) || !Write32(out, bits))
      return false;
  }
  out.flush();
  return bool(out);
}
inline bool LoadInitialCache(const rime::path &path,
                             const BoundIdentity &expected,
                             const std::vector<char> &initials,
                             InitialCache *cache) {
  std::ifstream in(static_cast<const std::filesystem::path &>(path),
                   std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 32 || in.tellg() > 4 * 1024 * 1024 ||
      expected.table_bytes > UINT32_MAX ||
      expected.syllables != initials.size())
    return false;
  auto file_bytes = in.tellg();
  in.seekg(0);
  char magic[8];
  if (!in.read(magic, 8) || std::memcmp(magic, "SX3TOP08", 8))
    return false;
  BoundIdentity actual{};
  uint32_t low, high, count;
  if (!Read32(in, &actual.checksum) || !Read32(in, &actual.syllables) ||
      !Read32(in, &actual.entries) || !Read32(in, &low) || !Read32(in, &high) ||
      !Read32(in, &count))
    return false;
  actual.table_bytes = uint64_t(low) | (uint64_t(high) << 32);
  if (!(actual == expected) || count > InitialCache::codes * 8 ||
      file_bytes !=
          std::streamoff(32 + 4 * (InitialCache::codes + 1) + 16 * count))
    return false;
  cache->offsets.resize(InitialCache::codes + 1);
  cache->entries.resize(count);
  for (auto &offset : cache->offsets)
    if (!Read32(in, &offset) || offset > count)
      return false;
  if (cache->offsets.front() != 0 || cache->offsets.back() != count)
    return false;
  for (size_t bucket = 0; bucket < InitialCache::codes; ++bucket) {
    auto begin = cache->offsets[bucket], end = cache->offsets[bucket + 1];
    if (end < begin || end - begin > 8)
      return false;
    for (size_t i = begin; i < end; ++i) {
      auto &entry = cache->entries[i];
      uint32_t ab, c, bits;
      if (!Read32(in, &entry.offset) || !Read32(in, &ab) || !Read32(in, &c) ||
          !Read32(in, &bits))
        return false;
      entry.code = {uint16_t(ab), uint16_t(ab >> 16), uint16_t(c)};
      std::memcpy(&entry.weight, &bits, 4);
      if (c > UINT16_MAX || expected.table_bytes < sizeof(rime::table::Entry) ||
          entry.offset > expected.table_bytes - sizeof(rime::table::Entry) ||
          !std::isfinite(entry.weight))
        return false;
      std::array<char, 3> raw{};
      for (size_t p = 0; p < 3; ++p) {
        if (entry.code[p] >= initials.size())
          return false;
        raw[p] = initials[entry.code[p]];
        if (raw[p] < 'a' || raw[p] > 'z')
          return false;
      }
      if (InitialCode(std::string_view(raw.data(), 3)) != bucket ||
          (i > begin && BetterReading(entry, cache->entries[i - 1])))
        return false;
    }
  }
  return in.peek() == std::char_traits<char>::eof();
}
} // namespace shixin_typo
