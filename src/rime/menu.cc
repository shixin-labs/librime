//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-29 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <iterator>
#include <rime/filter.h>
#include <rime/menu.h>
#include <rime/translation.h>

namespace rime {

namespace {
// The ordinary filter history is populated by Menu::Prepare as usual. Never
// prefetch its first page into a private buffer: uniquifier depends on history.
class CorrectionsAfterPage : public Translation {
 public:
  CorrectionsAfterPage(an<Translation> ordinary, an<Translation> corrections,
                       CandidateList* history, size_t page_size)
      : ordinary_(std::move(ordinary)), corrections_(std::move(corrections)),
        history_(history), head_left_(page_size) { Refresh(); }
  an<Candidate> Peek() override {
    return exhausted() ? nullptr : (in_corrections_ ? corrections_ : ordinary_)->Peek();
  }
  bool Next() override {
    if (exhausted()) return false;
    if (in_corrections_) {
      corrections_->Next();
      ++correction_count_;
    } else {
      ordinary_->Next();
      if (head_left_) --head_left_;
    }
    Refresh();
    return !exhausted();
  }
 private:
  void Refresh() {
    in_corrections_ = false;
    // Short ordinary menus stay unchanged, including their last-page status.
    if (head_left_) {
      set_exhausted(ordinary_->exhausted());
      return;
    }
    while (corrections_ && !corrections_->exhausted() && correction_count_ < 8) {
      auto candidate = corrections_->Peek();
      bool duplicate = !candidate;
      if (candidate) {
        for (const auto& previous : *history_) {
          if (previous->text() == candidate->text() &&
              previous->start() == candidate->start() && previous->end() == candidate->end()) {
            duplicate = true;
            break;
          }
        }
      }
      if (!duplicate) {
        in_corrections_ = true;
        set_exhausted(false);
        return;
      }
      corrections_->Next();
      ++correction_count_;
    }
    corrections_.reset();
    set_exhausted(ordinary_->exhausted());
  }
  an<Translation> ordinary_, corrections_;
  CandidateList* history_;
  size_t head_left_, correction_count_ = 0;
  bool in_corrections_ = false;
};
}  // namespace

void Menu::AddCorrections(an<Translation> corrections, size_t page_size) {
  if (!corrections || corrections->exhausted() || !candidates_.empty() ||
      page_size == 0 || page_size > 10) return;
  result_ = New<CorrectionsAfterPage>(result_, std::move(corrections), &candidates_, page_size);
}

Menu::Menu() : merged_(new MergedTranslation(candidates_)), result_(merged_) {}

void Menu::AddTranslation(an<Translation> translation) {
  *merged_ += translation;
  DLOG(INFO) << merged_->size() << " translations added.";
}

void Menu::AddFilter(Filter* filter) {
  result_ = filter->Apply(result_, &candidates_);
}

size_t Menu::Prepare(size_t requested) {
  DLOG(INFO) << "preparing " << requested << " candidates.";
  while (candidates_.size() < requested && !result_->exhausted()) {
    if (auto cand = result_->Peek()) {
      candidates_.push_back(cand);
    }
    result_->Next();
  }
  return candidates_.size();
}

Page* Menu::CreatePage(size_t page_size, size_t page_no) {
  size_t start_pos = page_size * page_no;
  size_t end_pos = start_pos + page_size;
  if (end_pos > candidates_.size()) {
    if (result_->exhausted())
      end_pos = candidates_.size();
    else
      end_pos = Prepare(end_pos);
    if (start_pos >= end_pos)
      return NULL;
    end_pos = (std::min)(start_pos + page_size, end_pos);
  }
  Page* page = new Page;
  if (!page)
    return NULL;
  page->page_size = page_size;
  page->page_no = page_no;
  page->is_last_page = result_->exhausted() && (end_pos == candidates_.size());
  std::copy(candidates_.begin() + start_pos, candidates_.begin() + end_pos,
            std::back_inserter(page->candidates));
  return page;
}

an<Candidate> Menu::GetCandidateAt(size_t index) {
  if (index >= candidates_.size() && index >= Prepare(index + 1)) {
    return nullptr;
  }
  return candidates_[index];
}

bool Menu::empty() const {
  return candidates_.empty() && result_->exhausted();
}

}  // namespace rime
