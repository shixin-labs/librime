#pragma once
// Scratch-only policy. Immutable small spelling model; fixed query workspace.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <rime/common.h>
#include "prefix_edits.h"

namespace shixin_typo {
struct SpellingHypothesis {
  std::array<char, 32> code{};
  size_t length = 0;
  double score = 0;
  EditTrace edit{};
  bool abbreviated = false;
  std::string_view spelling() const { return {code.data(), length}; }
};
struct HypothesisPlan {
  std::array<SpellingHypothesis, 16> hypotheses{};
  size_t count = 0;
  bool complete = false, abbreviated = false;
};
class HypothesisPolicy {
  struct Form {
    std::array<char, 6> code{};
    size_t length = 0;
    double score = 0;
    bool abbreviated = false;
    std::string_view spelling() const { return {code.data(), length}; }
  };
  std::array<Form, 512> forms_{};
  size_t count_ = 0;
  Keyboard keyboard_;
  const Form* Find(std::string_view code) const {
    auto end = forms_.begin() + count_;
    auto it = std::lower_bound(forms_.begin(), end, code,
        [](const Form& f, std::string_view s) { return f.spelling() < s; });
    return it != end && it->spelling() == code ? &*it : nullptr;
  }
 public:
  static bool ThreeInitials(std::string_view code) {
    if(code.size()!=3) return false;
    for(char c:code) if(c<'a'||c>'z'||std::string_view("aeiouv").find(c)!=std::string_view::npos) return false;
    return true;
  }
  // Offline generated, sorted TSV; only called during translator initialization.
  // Bounded byte reader rejects oversized/truncated/trailing or unsorted records.
  bool Load(const std::string& path) {
    count_ = 0;
    std::ifstream file(static_cast<const std::filesystem::path&>(rime::path(path)));
    if (!file) return false;
    char line[96];
    while (file.getline(line, sizeof(line))) {
      std::string row(line, size_t(file.gcount()) - (file.eof() ? 0 : 1));
      if (!row.empty() && row.back() == '\r') row.pop_back();
      if (row.find('\0') != row.npos) { count_=0; return false; }
      auto a = row.find('\t'), b = row.find('\t', a == row.npos ? 0 : a+1);
      if (a == row.npos || b == row.npos || a == 0 || a > 6 || count_ == forms_.size()) { count_=0; return false; }
      for (size_t i=0;i<a;++i) if (row[i]<'a'||row[i]>'z') { count_=0; return false; }
      char* tail = nullptr;
      double score = std::strtod(row.c_str()+a+1,&tail);
      if (b == a+1 || tail != row.c_str()+b || !std::isfinite(score) || score > 0 ||
          (row.substr(b+1)!="0" && row.substr(b+1)!="1") ||
          (count_ && forms_[count_-1].spelling() >= std::string_view(row.data(),a))) { count_=0; return false; }
      Form f; f.length=a; f.score=score; f.abbreviated=row[b+1]=='1';
      std::copy_n(row.data(),a,f.code.data()); forms_[count_++]=f;
    }
    if (!file.eof()) { count_=0; return false; }
    return count_ != 0;
  }
  template<class Budget>
  HypothesisPlan Plan(std::string_view raw, Budget& budget) const {
    HypothesisPlan result;
    if (!count_ || raw.size()<3 || raw.size()>32) return result;
    for (size_t i=0;i<raw.size();++i) {
      const char c=raw[i];
      if (c=='\'') {
        if (!i || i+1==raw.size() || raw[i-1]=='\'') return result;
      } else if (c<'a'||c>'z') return result;
    }
    struct Score { double value; bool abbreviated; };
    auto score = [&](std::string_view code) {
      std::array<Score,33> dp;
      for(auto& s:dp) s={-std::numeric_limits<double>::infinity(),false};
      dp[0]={0,false};
      for(size_t i=0;i<code.size();++i) {
        if (!std::isfinite(dp[i].value)) continue;
        if (code[i]=='\'') { dp[i+1]=dp[i]; continue; }
        for(size_t j=i+1;j<=code.size() && j<=i+6;++j) {
          if (!budget.SpendShixinTypoWork()) return Score{-std::numeric_limits<double>::infinity(),false};
          if (const auto* form=Find(code.substr(i,j-i))) {
            Score next{dp[i].value+form->score,dp[i].abbreviated||form->abbreviated};
            if(next.value>dp[j].value || (next.value==dp[j].value && !next.abbreviated)) dp[j]=next;
          }
        }
      }
      return dp[code.size()];
    };
    auto better=[](const SpellingHypothesis& a,const SpellingHypothesis& b) {
      if(a.score!=b.score) return a.score>b.score;
      if(a.edit.cost!=b.edit.cost) return a.edit.cost<b.edit.cost;
      return a.spelling()<b.spelling();
    };
    std::array<char,32> edited{}; std::copy(raw.begin(),raw.end(),edited.begin());
    auto add=[&](EditTrace trace) {
      const auto s=score({edited.data(),raw.size()});
      if(!std::isfinite(s.value)) return;
      SpellingHypothesis h; h.code=edited; h.length=raw.size(); h.edit=trace;
      h.score=s.value-double(trace.cost)/4; h.abbreviated=s.abbreviated;
      // Preserve a clear three-initial input form ahead of ambiguous mixed
      // parses, but retain the latter as lower-ranked hypotheses.
      if(ThreeInitials(raw) && !ThreeInitials(h.spelling())) h.score-=2;
      size_t at=0; while(at<result.count && !better(h,result.hypotheses[at])) ++at;
      if(at==16) return;
      for(size_t i=std::min(result.count,size_t(15));i>at;--i) result.hypotheses[i]=result.hypotheses[i-1];
      result.hypotheses[at]=h; result.count=std::min(result.count+1,size_t(16));
    };
    for(size_t i=0;i+1<raw.size() && !budget.shixin_typo_exhausted;++i) {
      if(raw[i]==raw[i+1] || raw[i]=='\'' || raw[i+1]=='\'') continue;
      std::swap(edited[i],edited[i+1]);
      add({EditTrace::Kind::Swap,uint8_t(i),uint8_t(i+2),8,{raw[i],raw[i+1]},{edited[i],edited[i+1]}});
      std::swap(edited[i],edited[i+1]);
    }
    for(size_t i=0;i<raw.size() && !budget.shixin_typo_exhausted;++i) {
      if(raw[i]=='\'') continue;
      for(char c='a';c<='z' && !budget.shixin_typo_exhausted;++c) if(keyboard_.adjacent[raw[i]-'a'][c-'a']) {
        edited[i]=c;
        add({EditTrace::Kind::Neighbor,uint8_t(i),uint8_t(i+1),10,{raw[i],0},{c,0}});
      }
      edited[i]=raw[i];
    }
    const auto original=score(raw);
    if(!budget.CheckShixinTypoDeadline()) return {};
    result.complete=true;
    // Intent is unknown. Use the best corrected spelling's parse, or the raw
    // parse if no correction survives. This is a heuristic, not a mode detector.
    result.abbreviated=result.count ? result.hypotheses[0].abbreviated : original.abbreviated;
    if(!result.abbreviated) result.count=std::min(result.count,size_t(8));
    return result;
  }
};
} // namespace shixin_typo
