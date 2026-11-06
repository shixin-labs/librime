//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-08 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <utility>
#include <atomic>
#include <limits>
#include <rime/gear/correction_registry.h>
#include <rime/gear/correction_filters.h>
#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/menu.h>
#include <rime/segmentation.h>

namespace rime {

namespace {
uint64_t NewShixinTypoIncarnation() {
  static std::atomic<uint64_t> next{1};
  auto value = next.load(std::memory_order_relaxed);
  while (value != std::numeric_limits<uint64_t>::max()) {
    if (next.compare_exchange_weak(value, value+1, std::memory_order_relaxed)) return value;
  }
  return 0;
}
}
Context::Context() : shixin_typo_incarnation_(NewShixinTypoIncarnation()) {}
Context::~Context() { shixin_typo_registry_.reset(); }

bool Context::ConfigureShixinTypoCorrections(uint64_t generation) {
  if (!generation || !shixin_typo_incarnation_ || shixin_typo_revision_ == std::numeric_limits<uint64_t>::max()) return false;
  if (!shixin_typo_registry_) shixin_typo_registry_.reset(new shixin_typo::CorrectionRegistry);
  if (generation != shixin_typo_generation_) {
    shixin_typo_generation_ = generation;
    InvalidateShixinTypoCorrections();
  }
  return true;
}
void Context::InvalidateShixinTypoCorrections() {
  if (!shixin_typo_registry_) return;
  shixin_typo_registry_->Clear();
  if (shixin_typo_revision_ != std::numeric_limits<uint64_t>::max()) ++shixin_typo_revision_;
}
bool Context::PublishShixinTypoCorrections() {
  if (!shixin_typo_registry_ || !shixin_typo_generation_ || !shixin_typo_revision_ ||
      shixin_typo_revision_ == std::numeric_limits<uint64_t>::max() || composition_.empty()) return false;
  auto snapshot = As<ShixinTypoCorrectionFilterTranslation>(composition_.back().shixin_typo_corrections);
  if (!snapshot) return false;
  std::array<shixin_typo::CorrectionChoice, 8> choices;
  const auto count = snapshot->candidate_count();
  if (count > choices.size()) return false;
  for (size_t i=0; i<count; ++i)
    if (!shixin_typo::ChoiceFromNativeTrace(*this, snapshot->candidate_at(i), &choices[i])) {
      shixin_typo_registry_->Clear(); return false;
    }
  if (!shixin_typo_registry_->Publish(*this, {shixin_typo_generation_, shixin_typo_incarnation_, shixin_typo_revision_}, choices.data(), count)) return false;
  shixin_typo_registry_->Seal();
  return true;
}
size_t Context::ShixinTypoCorrectionSize() const {
  return shixin_typo_registry_ ? shixin_typo_registry_->size() : 0;
}
bool Context::ReadShixinTypoCorrection(size_t index, shixin_typo::CorrectionView* view) const {
  return shixin_typo_registry_ && shixin_typo_registry_->Read(*this,
      {shixin_typo_generation_, shixin_typo_incarnation_, shixin_typo_revision_}, index, view);
}
bool Context::SelectShixinTypoCorrection(const shixin_typo::CorrectionToken& token) {
  return shixin_typo_registry_ && shixin_typo_registry_->Select(*this,
      {shixin_typo_generation_, shixin_typo_incarnation_, shixin_typo_revision_}, token);
}

const string kSelectedBeforeEditing = "selected_before_editing";

bool Context::Commit() {
  if (!IsComposing())
    return false;
  // notify the engine and interesting components
  InvalidateShixinTypoCorrections();
  commit_notifier_(this);
  // start over
  Clear();
  return true;
}

string Context::GetCommitText() const {
  if (get_option("dumb"))
    return string();
  return composition_.GetCommitText();
}

string Context::GetScriptText() const {
  return composition_.GetScriptText();
}

static const string kCaretSymbol("\xe2\x80\xb8");  // U+2038 ‸ CARET

string Context::GetSoftCursor() const {
  return get_option("soft_cursor") ? kCaretSymbol : string();
}

Preedit Context::GetPreedit() const {
  return composition_.GetPreedit(input_, caret_pos_, GetSoftCursor());
}

bool Context::IsComposing() const {
  return !input_.empty() || !composition_.empty();
}

bool Context::HasMenu() const {
  if (composition_.empty())
    return false;
  const auto& menu(composition_.back().menu);
  return menu && !menu->empty();
}

an<Candidate> Context::GetSelectedCandidate() const {
  if (composition_.empty())
    return nullptr;
  return composition_.back().GetSelectedCandidate();
}

bool Context::PushInput(char ch) {
  if (caret_pos_ >= input_.length()) {
    input_.push_back(ch);
    caret_pos_ = input_.length();
  } else {
    input_.insert(caret_pos_, 1, ch);
    ++caret_pos_;
  }
  InvalidateShixinTypoCorrections();
  update_notifier_(this);
  return true;
}

bool Context::PushInput(const string& str) {
  if (caret_pos_ >= input_.length()) {
    input_ += str;
    caret_pos_ = input_.length();
  } else {
    input_.insert(caret_pos_, str);
    caret_pos_ += str.length();
  }
  InvalidateShixinTypoCorrections();
  update_notifier_(this);
  return true;
}

bool Context::PopInput(size_t len) {
  if (caret_pos_ < len)
    return false;
  caret_pos_ -= len;
  input_.erase(caret_pos_, len);
  InvalidateShixinTypoCorrections();
  update_notifier_(this);
  return true;
}

bool Context::DeleteInput(size_t len) {
  if (caret_pos_ + len > input_.length())
    return false;
  input_.erase(caret_pos_, len);
  InvalidateShixinTypoCorrections();
  update_notifier_(this);
  return true;
}

void Context::Clear() {
  input_.clear();
  caret_pos_ = 0;
  composition_.clear();
  InvalidateShixinTypoCorrections();
  update_notifier_(this);
}

void Context::AbortComposition() {
  Clear();
  InvalidateShixinTypoCorrections();
  abort_notifier_(this);
}

bool Context::Select(size_t index) {
  if (composition_.empty())
    return false;
  Segment& seg(composition_.back());
  if (auto cand = seg.GetCandidateAt(index)) {
    seg.selected_candidate_override.reset();
    seg.selected_index = index;
    seg.status = Segment::kSelected;
    DLOG(INFO) << "Selected: '" << cand->text() << "', index = " << index;
    InvalidateShixinTypoCorrections();
    select_notifier_(this);
    return true;
  }
  return false;
}

bool Context::SelectExplicitCandidate(an<Candidate> candidate) {
  if (!candidate || composition_.empty()) return false;
  Segment& seg(composition_.back());
  if (seg.status >= Segment::kSelected || candidate->start() != seg.start ||
      candidate->end() <= candidate->start() || candidate->end() > seg.end ||
      seg.end > input_.size()) return false;
  seg.selected_candidate_override = std::move(candidate);
  seg.status = Segment::kSelected;
  InvalidateShixinTypoCorrections();
  select_notifier_(this);
  return true;
}

bool Context::Highlight(size_t index) {
  if (composition_.empty() || !composition_.back().menu)
    return false;
  Segment& seg(composition_.back());
  size_t candidate_count = seg.menu->Prepare(index + 1);
  size_t new_index =
      candidate_count > 0 ? (std::min)(candidate_count - 1, index) : 0;
  size_t previous_index = seg.selected_index;
  if (previous_index == new_index && !seg.selected_candidate_override) {
    DLOG(INFO) << "selection has not changed, currently at " << new_index;
    return false;
  }
  seg.selected_candidate_override.reset();
  seg.selected_index = new_index;
  InvalidateShixinTypoCorrections();
  update_notifier_(this);
  DLOG(INFO) << "selection changed from: " << previous_index
             << " to: " << new_index;
  return true;
}

bool Context::DeleteCandidate(size_t index) {
  if (composition_.empty())
    return false;
  Segment& seg(composition_.back());
  auto candidate = seg.GetCandidateAt(index);
  if (!candidate) return false;
  seg.selected_candidate_override.reset();
  seg.selected_index = index;
  DLOG(INFO) << "Deleting candidate: " << seg.GetSelectedCandidate()->text();
  InvalidateShixinTypoCorrections();
  delete_notifier_(this);
  return true;  // CAVEAT: this doesn't mean anything is deleted for sure
}

bool Context::DeleteCurrentSelection() {
  if (composition_.empty())
    return false;
  Segment& seg(composition_.back());
  if (seg.selected_candidate_override) {
    InvalidateShixinTypoCorrections();
    delete_notifier_(this);
    return true;
  }
  return DeleteCandidate(seg.selected_index);
}

bool Context::ConfirmCurrentSelection() {
  if (composition_.empty())
    return false;
  Segment& seg(composition_.back());
  seg.status = Segment::kSelected;
  if (auto cand = seg.GetSelectedCandidate()) {
    DLOG(INFO) << "Confirmed: '" << cand->text()
               << "', selected_index = " << seg.selected_index;
  } else {
    if (seg.end == seg.start) {
      // fluid_editor will confirm the whole sentence
      return false;
    }
    // confirm raw input
  }
  InvalidateShixinTypoCorrections();
  select_notifier_(this);
  return true;
}

void Context::BeginEditing() {
  for (auto it = composition_.rbegin(); it != composition_.rend(); ++it) {
    if (it->status > Segment::kSelected) {
      return;
    }
    if (it->status == Segment::kSelected) {
      it->tags.insert(kSelectedBeforeEditing);
      return;
    }
  }
}

bool Context::ConfirmPreviousSelection() {
  BeginEditing();
  return false;
}

bool Context::ReopenPreviousSegment() {
  if (composition_.Trim()) {
    if (!composition_.empty() &&
        composition_.back().status >= Segment::kSelected) {
      composition_.back().Reopen(caret_pos());
    }
    InvalidateShixinTypoCorrections();
    update_notifier_(this);
    return true;
  }
  return false;
}

bool Context::ClearPreviousSegment() {
  if (composition_.empty())
    return false;
  size_t where = composition_.back().start;
  if (where >= input_.length())
    return false;
  set_input(input_.substr(0, where));
  return true;
}

bool Context::ReopenPreviousSelection() {
  for (auto it = composition_.rbegin(); it != composition_.rend(); ++it) {
    if (it->status > Segment::kSelected)
      return false;
    if (it->status == Segment::kSelected) {
      // do not reopen the previous selection after editing input.
      if (it->tags.count(kSelectedBeforeEditing) != 0) {
        return false;
      }
      while (it != composition_.rbegin()) {
        composition_.pop_back();
      }
      it->Reopen(caret_pos());
      InvalidateShixinTypoCorrections();
      update_notifier_(this);
      return true;
    }
  }
  return false;
}

bool Context::ClearNonConfirmedComposition() {
  bool reverted = false;
  while (!composition_.empty() &&
         composition_.back().status < Segment::kSelected) {
    composition_.pop_back();
    reverted = true;
  }
  if (reverted) {
    InvalidateShixinTypoCorrections();
    composition_.Forward();
    DLOG(INFO) << "composition: " << composition_.GetDebugText();
  }
  return reverted;
}

bool Context::RefreshNonConfirmedComposition() {
  if (ClearNonConfirmedComposition()) {
    InvalidateShixinTypoCorrections();
    update_notifier_(this);
    return true;
  }
  return false;
}

void Context::set_caret_pos(size_t caret_pos) {
  if (caret_pos > input_.length())
    caret_pos_ = input_.length();
  else
    caret_pos_ = caret_pos;
  InvalidateShixinTypoCorrections();
  update_notifier_(this);
}

void Context::set_composition(Composition&& comp) {
  InvalidateShixinTypoCorrections();
  composition_ = std::move(comp);
}

void Context::set_input(const string& value) {
  input_ = value;
  caret_pos_ = input_.length();
  InvalidateShixinTypoCorrections();
  update_notifier_(this);
}

void Context::set_option(const string& name, bool value) {
  options_[name] = value;
  DLOG(INFO) << "Context::set_option " << name << " = " << value;
  InvalidateShixinTypoCorrections();
  option_update_notifier_(this, name);
}

bool Context::get_option(const string& name) const {
  auto it = options_.find(name);
  if (it != options_.end())
    return it->second;
  else
    return false;
}

void Context::set_property(const string& name, const string& value) {
  properties_[name] = value;
  InvalidateShixinTypoCorrections();
  property_update_notifier_(this, name);
}

string Context::get_property(const string& name) const {
  auto it = properties_.find(name);
  if (it != properties_.end())
    return it->second;
  else
    return string();
}

void Context::ClearTransientOptions() {
  InvalidateShixinTypoCorrections();
  DLOG(INFO) << "Context::ClearTransientOptions";
  auto opt = options_.lower_bound("_");
  while (opt != options_.end() && !opt->first.empty() && opt->first[0] == '_') {
    DLOG(INFO) << "cleared opption: " << opt->first;
    options_.erase(opt++);
  }
  auto prop = properties_.lower_bound("_");
  while (prop != properties_.end() && !prop->first.empty() &&
         prop->first[0] == '_') {
    properties_.erase(prop++);
  }
}

}  // namespace rime
