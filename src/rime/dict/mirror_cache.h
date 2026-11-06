#pragma once
// Experimental compact text/native-entry mirror. Loading requires an already
// validated generation; this structural parser does not authenticate artifacts.
#include "initial_cache.h"
namespace shixin_typo {
struct TextSpan {
  uint32_t offset = 0, length = 0;
};
struct MirrorCache {
  std::vector<rime::table::Entry> native;
  std::vector<TextSpan> spans;
  std::vector<char> text;
  std::string_view Text(size_t i) const {
    const auto &span = spans[i];
    return {text.data() + span.offset, span.length};
  }
  size_t Find(const CachedReading &reading, const InitialCache &cache,
              const std::vector<char> &initials) const {
    std::array<char, 3> code{};
    for (size_t i = 0; i < 3; ++i)
      code[i] = initials[reading.code[i]];
    const auto bucket = InitialCode({code.data(), 3});
    for (size_t i = cache.offsets[bucket]; i < cache.offsets[bucket + 1]; ++i)
      if (cache.entries[i].offset == reading.offset &&
          cache.entries[i].code == reading.code)
        return i;
    return native.size();
  }
};
inline bool ValidMirrorUTF8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    uint32_t c = uint8_t(s[i++]);
    if (c == 0)
      return false;
    if (c < 0x80)
      continue;
    unsigned n = 0;
    uint32_t minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      n = 1;
      c &= 31;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      c &= 15;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      c &= 7;
      minimum = 0x10000;
    } else
      return false;
    if (n > s.size() - i)
      return false;
    while (n--) {
      uint32_t b = uint8_t(s[i++]);
      if ((b & 0xc0) != 0x80)
        return false;
      c = (c << 6) | (b & 63);
    }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return false;
  }
  return !s.empty();
}
inline bool SaveMirror(const rime::path &path, const BoundIdentity &id,
                       const InitialCache &cache, const MirrorCache &mirror) {
  if (std::filesystem::exists(path) ||
      mirror.native.size() != cache.entries.size() ||
      mirror.spans.size() != cache.entries.size())
    return false;
  std::ofstream out(static_cast<const std::filesystem::path &>(path),
                    std::ios::binary);
  out.write("SX3TXT01", 8);
  for (uint32_t value :
       {id.checksum, id.syllables, id.entries, uint32_t(id.table_bytes),
        uint32_t(id.table_bytes >> 32), uint32_t(cache.entries.size()),
        uint32_t(mirror.text.size())})
    if (!Write32(out, value))
      return false;
  for (size_t i = 0; i < cache.entries.size(); ++i)
    for (uint32_t value :
         {cache.entries[i].offset, uint32_t(mirror.native[i].text.value),
          mirror.spans[i].offset, mirror.spans[i].length})
      if (!Write32(out, value))
        return false;
  out.write(mirror.text.data(), mirror.text.size());
  out.flush();
  return bool(out);
}
inline bool LoadMirror(const rime::path &path, const BoundIdentity &expected,
                       const InitialCache &cache, MirrorCache *output) {
  std::ifstream in(static_cast<const std::filesystem::path &>(path),
                   std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 36 || in.tellg() > 8 * 1024 * 1024)
    return false;
  const auto bytes = in.tellg();
  in.seekg(0);
  char magic[8];
  if (!in.read(magic, 8) || std::memcmp(magic, "SX3TXT01", 8))
    return false;
  BoundIdentity actual{};
  uint32_t low, high, count, pool;
  if (!Read32(in, &actual.checksum) || !Read32(in, &actual.syllables) ||
      !Read32(in, &actual.entries) || !Read32(in, &low) || !Read32(in, &high) ||
      !Read32(in, &count) || !Read32(in, &pool))
    return false;
  actual.table_bytes = uint64_t(low) | (uint64_t(high) << 32);
  if (!(actual == expected) || count != cache.entries.size() ||
      count > InitialCache::codes * 8 || pool > 4 * 1024 * 1024 ||
      uint64_t(bytes) != 36 + uint64_t(count) * 16 + pool)
    return false;
  MirrorCache result;
  result.native.resize(count);
  result.spans.resize(count);
  result.text.resize(pool);
  for (size_t i = 0; i < count; ++i) {
    uint32_t offset, text_id;
    auto &span = result.spans[i];
    if (!Read32(in, &offset) || !Read32(in, &text_id) ||
        !Read32(in, &span.offset) || !Read32(in, &span.length) ||
        offset != cache.entries[i].offset || text_id > INT32_MAX ||
        span.length == 0 || span.length > 4096 || span.offset >= pool ||
        span.length >= pool - span.offset)
      return false;
    result.native[i].text.value = int32_t(text_id);
    result.native[i].weight = cache.entries[i].weight;
  }
  if (!in.read(result.text.data(), pool))
    return false;
  for (size_t i = 0; i < count; ++i) {
    const auto &span = result.spans[i];
    if (result.text[span.offset + span.length] != 0 ||
        !ValidMirrorUTF8(result.Text(i)))
      return false;
  }
  if (in.peek() != std::char_traits<char>::eof())
    return false;
  *output = std::move(result);
  return true;
}
} // namespace shixin_typo
