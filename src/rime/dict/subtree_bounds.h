#pragma once
// Experimental three-initial neighbor lane. No general pinyin/graph claims.
#include <rime/dict/table.h>
#include <algorithm>
#include <array>
#include <vector>
#include <limits>
#include <chrono>
#include "prefix_edits.h"
namespace shixin_typo {
struct PrefixBound {
  const rime::table::TrunkIndex *children = nullptr;
  std::vector<float> best_three;
  float best = -std::numeric_limits<float>::infinity();
};
struct BoundState {
  float bound;
  uint32_t a, b;
  uint8_t depth, edits;
};
struct BoundCandidate {
  const rime::table::Entry *entry = nullptr;
  std::array<rime::SyllableId, 3> code{};
};
struct BoundResult {
  std::array<BoundCandidate, 8> candidates{};
  size_t count = 0, visited = 0, probes = 0, peak = 0;
  bool complete = true;
};
class ThreeInitialSearch {
  // Fixed, reusable scratch storage. Bounds/initials are immutable, initialized
  // separately; caller serializes access. No strings or Code vectors here.
  std::array<BoundState, 4096> pending_{};
  Keyboard keyboard_;
  std::array<std::pair<uint32_t, uint32_t>, 26> ranges_{};
  size_t syllables_ = 0;
  struct Order {
    bool operator()(const BoundState &a, const BoundState &b) const {
      return a.bound < b.bound;
    }
  };
  int Edit(char raw, char canonical) const {
    if (raw == canonical)
      return 0;
    if (raw < 'a' || raw > 'z' || canonical < 'a' || canonical > 'z')
      return 2;
    return keyboard_.adjacent[raw - 'a'][canonical - 'a'] ? 1 : 2;
  }

public:
  bool Initialize(const std::vector<char> &initial) {
    ranges_.fill({0, 0});
    syllables_ = 0;
    for (size_t i = 0; i < initial.size(); ++i) {
      char c = initial[i];
      if (c < 'a' || c > 'z')
        continue;
      auto &range = ranges_[c - 'a'];
      if (range.second && range.second != i)
        return false;
      if (!range.second)
        range.first = i;
      range.second = i + 1;
    }
    syllables_ = initial.size();
    return true;
  }
  BoundResult Search(std::string_view raw,
                     const std::vector<PrefixBound> &bounds,
                     const std::vector<char> &initial, size_t probe_cap = 32768,
                     size_t frontier_cap = 4096,
                     std::chrono::steady_clock::time_point deadline =
                         std::chrono::steady_clock::time_point::max()) {
    BoundResult result;
    if (frontier_cap == 0 || frontier_cap > pending_.size() ||
        raw.size() != 3 || bounds.size() != initial.size() ||
        syllables_ != initial.size()) {
      result.complete = false;
      return result;
    }
    for (char c : raw)
      if (c < 'a' || c > 'z') {
        result.complete = false;
        return result;
      }
    size_t size = 0;
    auto probe = [&]() {
      if (result.probes == probe_cap ||
          ((result.probes & 63) == 0 &&
           deadline != std::chrono::steady_clock::time_point::max() &&
           std::chrono::steady_clock::now() >= deadline)) {
        result.complete = false;
        return false;
      }
      ++result.probes;
      return true;
    };
    auto push = [&](BoundState state) {
      if (size == frontier_cap) {
        result.complete = false;
        return false;
      }
      pending_[size++] = state;
      std::push_heap(pending_.begin(), pending_.begin() + size, Order{});
      result.peak = std::max(result.peak, size);
      return true;
    };
    // Native trunk keys are sorted syllable IDs. Validated initial ranges
    // avoid probing every unrelated child after the edit budget is spent.
    auto scan = [&](const rime::table::TrunkIndex *children, char raw_char,
                    int used, auto visit) {
      for (int letter = 0; letter < 26; ++letter) {
        int edits = used + Edit(raw_char, char('a' + letter));
        auto range = ranges_[letter];
        if (edits > 1 || range.first == range.second)
          continue;
        size_t lo = 0, hi = children->size;
        while (lo < hi) {
          if (!probe())
            return false;
          size_t mid = lo + (hi - lo) / 2;
          if (children->at[mid].key < rime::SyllableId(range.first))
            lo = mid + 1;
          else
            hi = mid;
        }
        for (size_t child = lo;
             child < children->size &&
             children->at[child].key < rime::SyllableId(range.second);
             ++child) {
          if (!probe() || !visit(child, edits))
            return false;
        }
      }
      return true;
    };
    auto keep = [&](const rime::table::Entry *entry,
                    std::array<rime::SyllableId, 3> code) {
      BoundCandidate candidate{entry, code};
      // Stable tie order without decoding text or allocating: canonical code,
      // then mapped entry address within a table. Exhaustive oracle uses this.
      auto better = [](const BoundCandidate &a, const BoundCandidate &b) {
        if (a.entry->weight != b.entry->weight)
          return a.entry->weight > b.entry->weight;
        if (a.code != b.code)
          return a.code < b.code;
        return a.entry < b.entry;
      };
      size_t pos = 0;
      while (pos < result.count && !better(candidate, result.candidates[pos]))
        ++pos;
      if (pos == result.candidates.size())
        return;
      if (result.count < result.candidates.size())
        ++result.count;
      for (size_t j = result.count - 1; j > pos; --j)
        result.candidates[j] = result.candidates[j - 1];
      result.candidates[pos] = candidate;
    };
    for (size_t a = 0; a < bounds.size(); ++a) {
      if (!probe())
        return result;
      int e = Edit(raw[0], initial[a]);
      if (e <= 1 && bounds[a].best > -std::numeric_limits<float>::infinity() &&
          !push({bounds[a].best, uint32_t(a), 0, 1, uint8_t(e)}))
        return result;
    }
    while (size) {
      std::pop_heap(pending_.begin(), pending_.begin() + size, Order{});
      auto state = pending_[--size];
      ++result.visited;
      if (result.count == 8 && state.bound < result.candidates[7].entry->weight)
        break;
      auto children = bounds[state.a].children;
      if (state.depth == 1) {
        if (!scan(children, raw[1], state.edits, [&](size_t b, int e) {
              float bound = bounds[state.a].best_three[b];
              return bound <= -std::numeric_limits<float>::infinity() ||
                     push({bound, state.a, uint32_t(b), 2, uint8_t(e)});
            }))
          return result;
      } else {
        auto leaves = &children->at[state.b].next_level.get()->trunk();
        if (!scan(leaves, raw[2], state.edits, [&](size_t c, int edits) {
              if (edits != 1)
                return true;
              auto &entries = leaves->at[c].entries;
              for (size_t i = 0; i < entries.size; ++i) {
                if (!probe())
                  return false;
                keep(&entries.at[i],
                     {rime::SyllableId(state.a), children->at[state.b].key,
                      leaves->at[c].key});
              }
              return true;
            }))
          return result;
      }
    }
    return result;
  }
};
} // namespace shixin_typo
