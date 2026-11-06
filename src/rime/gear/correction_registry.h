#pragma once
// Scratch-only single-engine-thread registry. Not a wire format or public API.
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <rime/context.h>
#include <rime/gear/translator_commons.h>

namespace shixin_typo {
struct CorrectionScope {
  uint64_t generation = 0, session = 0, revision = 0;
  bool operator==(const CorrectionScope &b) const {
    return generation == b.generation && session == b.session &&
           revision == b.revision;
  }
  bool valid() const { return generation && session && revision; }
};
enum class EditKind : uint8_t { Swap, Neighbor, Missing, Extra };
struct CorrectionEdit {
  EditKind kind = EditKind::Neighbor;
  uint8_t begin = 0, end = 0;
  float cost = 0;
};
struct CorrectionChoice {
  rime::an<rime::Candidate> candidate;
  CorrectionEdit edit;
};
// Build registry metadata only from a native recovery trace. This validates raw
// alignment; keyboard adjacency and the dictionary path are the generator's
// responsibility. Costs retain native heuristic units, without reapplying them.
inline bool ChoiceFromNativeTrace(const rime::Context &context,
                                  rime::an<rime::Candidate> candidate,
                                  CorrectionChoice *choice) {
  if (!candidate || !choice)
    return false;
  const auto *trace = candidate->shixin_typo_edit_trace();
  if (!trace || !trace->cost || trace->begin < candidate->start() ||
      trace->end > candidate->end() || trace->end > context.input().size() ||
      trace->end <= trace->begin)
    return false;
  const size_t width = trace->end - trace->begin;
  EditKind kind;
  if (trace->kind == EditTrace::Kind::Swap) {
    if (width != 2 || trace->from[0] != trace->to[1] ||
        trace->from[1] != trace->to[0] || trace->from[0] == trace->from[1])
      return false;
    kind = EditKind::Swap;
  } else if (trace->kind == EditTrace::Kind::Neighbor) {
    if (width != 1 || trace->from[0] == trace->to[0])
      return false;
    kind = EditKind::Neighbor;
  } else
    return false;
  for (size_t i = 0; i < width; ++i)
    if (context.input()[trace->begin + i] != trace->from[i] ||
        trace->from[i] < 'a' || trace->from[i] > 'z' || trace->to[i] < 'a' ||
        trace->to[i] > 'z')
      return false;
  *choice = {std::move(candidate),
             {kind, trace->begin, trace->end, float(trace->cost)}};
  return true;
}
struct CorrectionToken {
  CorrectionScope scope;
  uint64_t publication = 0;
  uint8_t slot = 0;
};
struct CorrectionView {
  std::string_view text;
  size_t begin = 0, end = 0;
  CorrectionEdit edit;
  CorrectionToken token;
};
class CorrectionRegistry {
public:
  static constexpr size_t kCapacity = 8, kRawBytes = 32, kTextBytes = 256,
                          kCodeIds = 32;
  // Seed supports overflow testing; runtime construction starts at zero.
  explicit CorrectionRegistry(uint64_t seed = 0) : publication_(seed) {}
  CorrectionRegistry(const CorrectionRegistry &) = delete;
  CorrectionRegistry &operator=(const CorrectionRegistry &) = delete;
  void Clear() {
    for (auto &slot : slots_) {
      slot.choice.candidate.reset();
      slot.genuine.reset();
    }
    count_ = 0;
    owner_ = nullptr;
    scope_ = {};
    raw_size_ = 0;
    active_ = false;
    sealed_ = false;
  }
  bool Publish(rime::Context &context, CorrectionScope scope,
               const CorrectionChoice *choices, size_t count) {
    // A displayed snapshot is immutable, including a displayed empty list.
    if (active_ && sealed_ && scope == scope_)
      return false;
    Clear();
    if (disabled_ || publication_ == std::numeric_limits<uint64_t>::max()) {
      disabled_ = true;
      return false;
    }
    ++publication_;
    if (!scope.valid() || count > kCapacity || (count && !choices) ||
        context.input().size() > kRawBytes || context.composition().empty())
      return false;
    const auto &segment = context.composition().back();
    if (segment.status >= rime::Segment::kSelected ||
        segment.start >= segment.end || segment.end > context.input().size())
      return false;
    for (size_t i = 0; i < count; ++i) {
      const auto &c = choices[i].candidate;
      const auto &e = choices[i].edit;
      if (!c || c->start() != segment.start || c->end() <= c->start() ||
          c->end() > segment.end || e.begin < c->start() || e.end > c->end() ||
          e.end < e.begin || !std::isfinite(e.cost) || e.cost < 0)
        return false;
      const size_t width = e.end - e.begin;
      switch (e.kind) {
      case EditKind::Swap:
        if (width != 2)
          return false;
        break;
      case EditKind::Neighbor:
      case EditKind::Extra:
        if (width != 1)
          return false;
        break;
      case EditKind::Missing:
        if (width != 0)
          return false;
        break;
      default:
        return false;
      }
      auto genuine =
          rime::As<rime::Phrase>(rime::Candidate::GetGenuineCandidate(c));
      if (!genuine || genuine->code().empty() ||
          genuine->code().size() > kCodeIds || c->text().empty() ||
          c->text().size() > kTextBytes)
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
      auto &slot = slots_[i];
      slot.choice = choices[i];
      slot.genuine = rime::As<rime::Phrase>(
          rime::Candidate::GetGenuineCandidate(slot.choice.candidate));
      slot.begin = slot.choice.candidate->start();
      slot.end = slot.choice.candidate->end();
      slot.text_size = slot.choice.candidate->text().size();
      slot.code_size = slot.genuine->code().size();
      std::memcpy(slot.text.data(), slot.choice.candidate->text().data(),
                  slot.text_size);
      std::copy(slot.genuine->code().begin(), slot.genuine->code().end(),
                slot.code.begin());
    }
    owner_ = &context;
    scope_ = scope;
    count_ = count;
    begin_ = segment.start;
    end_ = segment.end;
    raw_size_ = context.input().size();
    std::memcpy(raw_.data(), context.input().data(), raw_size_);
    active_ = true;
    return true;
  }
  void Seal() {
    if (active_)
      sealed_ = true;
  }
  size_t size() const { return count_; }
  bool Read(const rime::Context &context, CorrectionScope current, size_t index,
            CorrectionView *view) const {
    if (!view || index >= count_ || !MatchesContext(context, current) ||
        !MatchesCandidate(slots_[index]))
      return false;
    const auto &slot = slots_[index];
    *view = {std::string_view(slot.text.data(), slot.text_size),
             slot.begin,
             slot.end,
             slot.choice.edit,
             {scope_, publication_, static_cast<uint8_t>(index)}};
    return true;
  }
  bool Select(rime::Context &context, CorrectionScope current,
              const CorrectionToken &token) {
    // current is authoritative engine state, never copied from the client
    // token.
    if (!MatchesContext(context, current) || !(token.scope == scope_) ||
        token.publication != publication_ || token.slot >= count_ ||
        !MatchesCandidate(slots_[token.slot]))
      return false;
    auto candidate = slots_[token.slot].choice.candidate;
    Clear(); // Invalidate before synchronous native notification callbacks.
    return context.SelectExplicitCandidate(std::move(candidate));
  }

private:
  struct Slot {
    CorrectionChoice choice;
    rime::an<rime::Phrase> genuine;
    size_t begin = 0, end = 0;
    std::array<char, kTextBytes> text{};
    std::array<rime::SyllableId, kCodeIds> code{};
    uint16_t text_size = 0;
    uint8_t code_size = 0;
  };
  bool MatchesContext(const rime::Context &context,
                      CorrectionScope current) const {
    if (!active_ || owner_ != &context || !(current == scope_) ||
        context.input().size() != raw_size_ ||
        std::memcmp(context.input().data(), raw_.data(), raw_size_) ||
        context.composition().empty())
      return false;
    const auto &segment = context.composition().back();
    return segment.status < rime::Segment::kSelected &&
           segment.start == begin_ && segment.end == end_;
  }
  bool MatchesCandidate(const Slot &slot) const {
    const auto &candidate = slot.choice.candidate;
    if (!candidate)
      return false;
    auto genuine =
        rime::As<rime::Phrase>(rime::Candidate::GetGenuineCandidate(candidate));
    return genuine == slot.genuine && candidate->start() == slot.begin &&
           candidate->end() == slot.end &&
           candidate->text().size() == slot.text_size &&
           !std::memcmp(candidate->text().data(), slot.text.data(),
                        slot.text_size) &&
           genuine->code().size() == slot.code_size &&
           std::equal(genuine->code().begin(), genuine->code().end(),
                      slot.code.begin());
  }
  std::array<Slot, kCapacity> slots_{};
  std::array<char, kRawBytes> raw_{};
  rime::Context *owner_ = nullptr;
  CorrectionScope scope_{};
  uint64_t publication_ = 0;
  size_t count_ = 0, raw_size_ = 0, begin_ = 0, end_ = 0;
  bool active_ = false, sealed_ = false, disabled_ = false;
};
static_assert(sizeof(CorrectionRegistry) <= 4096,
              "registry scratch storage grew");
} // namespace shixin_typo
