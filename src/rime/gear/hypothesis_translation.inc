// Included after ScriptTranslation/ShixinTypoCorrectionCandidate. Native candidates
// keep the original raw input, syllabifier, Phrase identity and edit provenance.
class ShixinTypoHypothesisTranslation : public Translation {
 public:
  void Add(size_t spelling, an<Candidate> candidate) {
    if(!finished_ && candidate && spelling < 16 && counts_[spelling] < 10)
      rows_[spelling][counts_[spelling]++] = std::move(candidate);
  }
  void Discard(size_t spelling) {
    if(finished_ || spelling>=16) return;
    rows_[spelling].fill(nullptr); counts_[spelling]=0;
  }
  void Finish() {
    if(finished_) return;
    finished_=true;
    // Only completed per-spelling batches reach here. Fixed 160 pointer moves;
    // no further lookup after exhaustion. The existing bounded correction
    // filter deduplicates text/span before publishing its display prefix.
    for(size_t rank=0;rank<10;++rank) for(size_t spelling=0;spelling<16;++spelling) {
      if(rank<counts_[spelling]) flat_[count_++]=std::move(rows_[spelling][rank]);
    }
    set_exhausted(!count_);
  }
  an<Candidate> Peek() override { return exhausted()?nullptr:flat_[position_]; }
  bool Next() override {
    if(exhausted()) return false;
    set_exhausted(++position_==count_); return !exhausted();
  }
 private:
  std::array<std::array<an<Candidate>,10>,16> rows_{};
  std::array<size_t,16> counts_{};
  std::array<an<Candidate>,160> flat_{};
  size_t count_=0,position_=0;
  bool finished_=false;
};
