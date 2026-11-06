// Legacy mixed-menu control. Recovery extraction lives in the independent
// stream; this adapter remains only for comparison with earlier captures.
#include "recovery_translation.inc"
class ShixinTypoProtectedTranslation : public Translation {
public:
  ShixinTypoProtectedTranslation(an<Translation> ordinary,
                            an<ScriptTranslation> recovery,
                            an<ScriptTranslation> broad, size_t page,
                            size_t full_span, size_t first_full_limit = 4)
      : ordinary_(ordinary), recovery_(recovery), broad_(broad), page_(page),
        full_span_(full_span), first_full_limit_(first_full_limit) {
    Prepare();
  }
  an<Candidate> Peek() override { return current_; }
  bool Next() override {
    if (exhausted())
      return false;
    if (from_recovery_)
      corrections_->Next();
    else {
      ordinary_->Next();
      ++ordinary_count_;
    }
    Prepare();
    return !exhausted();
  }

private:
  void Prepare() {
    current_.reset();
    from_recovery_ = false;
    if (ordinary_ && !ordinary_->exhausted() && ordinary_count_ < page_) {
      current_ = ordinary_->Peek();
      if (head_count_ < head_.size())
        head_[head_count_++] = current_;
    } else {
      if (!corrections_) {
        corrections_ = New<ShixinTypoRecoveryTranslation>(
            std::move(recovery_), std::move(broad_), full_span_,
            first_full_limit_, head_);
      }
      if (!corrections_->exhausted()) {
        current_ = corrections_->Peek();
        from_recovery_ = true;
      } else if (ordinary_ && !ordinary_->exhausted())
        current_ = ordinary_->Peek();
    }
    set_exhausted(!current_);
  }
  an<Translation> ordinary_;
  an<ScriptTranslation> recovery_, broad_;
  an<ShixinTypoRecoveryTranslation> corrections_;
  an<Candidate> current_;
  std::array<an<Candidate>, 10> head_{};
  size_t page_, full_span_, first_full_limit_, ordinary_count_ = 0,
                                               head_count_ = 0;
  bool from_recovery_ = false;
};

// Envelope handed to the engine: ordinary enumeration and a one-shot transfer
// of the independent raw recovery stream. No recovery enters Compare/Peek/Next.
class ShixinTypoSeparateTranslation : public Translation {
public:
  ShixinTypoSeparateTranslation(an<Translation> ordinary,
                           an<Translation> corrections)
      : ordinary_(std::move(ordinary)), corrections_(std::move(corrections)) {
    set_exhausted(!ordinary_ || ordinary_->exhausted());
  }
  an<Candidate> Peek() override {
    return exhausted() ? nullptr : ordinary_->Peek();
  }
  bool Next() override {
    if (exhausted())
      return false;
    ordinary_->Next();
    set_exhausted(ordinary_->exhausted());
    return !exhausted();
  }
  an<Translation> TakeShixinTypoCorrections() override {
    return std::move(corrections_);
  }

private:
  an<Translation> ordinary_, corrections_;
};
