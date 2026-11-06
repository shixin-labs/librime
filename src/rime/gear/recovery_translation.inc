// Scratch-only independent recovery stream. Included inside namespace rime
// after ScriptTranslation is defined. No ordinary Translation is retained or
// advanced here; exclusions are a bounded snapshot supplied by the caller.
class ShixinTypoCorrectionCandidate : public ShadowCandidate {
public:
  explicit ShixinTypoCorrectionCandidate(an<Candidate> c,
                                    shixin_typo::EditTrace trace)
      : ShadowCandidate(c, c->type(), c->text(), "纠错 → " + c->comment()),
        trace_(trace) {}
  bool is_shixin_typo_correction() const override { return true; }
  const shixin_typo::EditTrace *shixin_typo_edit_trace() const override {
    return &trace_;
  }

private:
  shixin_typo::EditTrace trace_;
};
class ShixinTypoRecoveryTranslation : public Translation {
public:
  ShixinTypoRecoveryTranslation(an<ScriptTranslation> recovery,
                           an<ScriptTranslation> broad, size_t full_span,
                           size_t first_full_limit = 4,
                           const std::array<an<Candidate>, 10> &exclusions = {})
      : recovery_(std::move(recovery)), broad_(std::move(broad)),
        exclusions_(exclusions), full_span_(full_span),
        first_full_limit_(first_full_limit) {
    Prepare();
  }
  an<Candidate> Peek() override { return current_; }
  bool Next() override {
    if (exhausted())
      return false;
    accepted_[extras_] = current_;
    ++per_span_[current_->end() - current_->start()];
    ++phase_span_[current_->end() - current_->start()];
    recovery_->Next();
    ++extras_;
    Prepare();
    return !exhausted();
  }

private:
  bool Duplicate(const an<Candidate> &c) const {
    for (const auto &prior : exclusions_)
      if (prior && prior->text() == c->text() && prior->start() == c->start() &&
          prior->end() == c->end())
        return true;
    for (const auto &prior : accepted_)
      if (prior && prior->text() == c->text() && prior->start() == c->start() &&
          prior->end() == c->end())
        return true;
    return false;
  }
  void Prepare() {
    current_.reset();
    while (scanned_ < 64 && extras_ < 8) {
      if (!recovery_ || recovery_->exhausted() || phase_scanned_ >= 32 ||
          (broad_ && extras_ >= 6)) {
        recovery_ = std::move(broad_);
        phase_scanned_ = 0;
        phase_span_.fill(0);
        second_phase_ = true;
        if (!recovery_)
          break;
      }
      auto c = recovery_->Peek();
      ++scanned_;
      ++phase_scanned_;
      auto phrase = As<Phrase>(c);
      shixin_typo::EditTrace trace;
      if (c && c->end() > c->start()) {
        size_t span = c->end() - c->start();
        if (phrase && !Duplicate(c) && span < per_span_.size() &&
            per_span_[span] < (span == full_span_ ? 6 : 2) &&
            phase_span_[span] < (span == full_span_
                                     ? (second_phase_ ? 4 : first_full_limit_)
                                     : 2) &&
            recovery_->ShixinTypoCorrectionTrace(*phrase, &trace)) {
          current_ = New<ShixinTypoCorrectionCandidate>(c, trace);
          break;
        }
      }
      recovery_->Next();
    }
    set_exhausted(!current_);
  }
  an<ScriptTranslation> recovery_, broad_;
  an<Candidate> current_;
  std::array<an<Candidate>, 10> exclusions_{};
  std::array<an<Candidate>, 8> accepted_{};
  std::array<uint8_t, 33> per_span_{}, phase_span_{};
  size_t full_span_, first_full_limit_, phase_scanned_ = 0, scanned_ = 0,
                                        extras_ = 0;
  bool second_phase_ = false;
};
