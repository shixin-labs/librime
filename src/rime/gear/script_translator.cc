//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Script translator
//
// 2011-07-10 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <array>
#include <stack>
#include <cmath>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/language.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/deployer.h>
#include <rime/translation.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/corrector.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/poet.h>
#include <rime/gear/script_translator.h>
#include "search_diagnostics.h"
#include <rime/gear/translator_commons.h>

// static const char* quote_left = "\xef\xbc\x88";
// static const char* quote_right = "\xef\xbc\x89";

namespace rime {
namespace {
thread_local bool shixin_typo_diagnostics_enabled = false;
thread_local ShixinTypoSearchDiagnostics shixin_typo_diagnostics;
}
void EnableShixinTypoSearchDiagnostics(bool enabled) {
  shixin_typo_diagnostics_enabled = enabled;
  shixin_typo_diagnostics = {};
}
ShixinTypoSearchDiagnostics ReadShixinTypoSearchDiagnostics() { return shixin_typo_diagnostics; }

namespace {

struct SyllabifyTask {
  const Code& code;
  const SyllableGraph& graph;
  size_t target_pos;
  function<void(SyllabifyTask* task,
                size_t depth,
                size_t current_pos,
                size_t next_pos)>
      push;
  function<void(SyllabifyTask* task, size_t depth)> pop;
};

static bool syllabify_dfs(SyllabifyTask* task,
                          size_t depth,
                          size_t current_pos, size_t* budget = nullptr) {
  if (budget) { if (!*budget) return false; --*budget; }
  if (depth == task->code.size()) {
    return current_pos == task->target_pos;
  }
  SyllableId syllable_id = task->code.at(depth);
  auto z = task->graph.edges.find(current_pos);
  if (z == task->graph.edges.end())
    return false;
  // favor longer spellings
  for (const auto& y : boost::adaptors::reverse(z->second)) {
    size_t end_vertex_pos = y.first;
    if (end_vertex_pos > task->target_pos)
      continue;
    auto x = y.second.find(syllable_id);
    if (x != y.second.end()) {
      task->push(task, depth, current_pos, end_vertex_pos);
      if (syllabify_dfs(task, depth + 1, end_vertex_pos, budget))
        return true;
      task->pop(task, depth);
    }
  }
  return false;
}

}  // anonymous namespace

class ScriptSyllabifier : public PhraseSyllabifier {
 public:
  ScriptSyllabifier(ScriptTranslator* translator,
                    Corrector* corrector,
                    const string& input,
                    size_t start)
      : translator_(translator),
        input_(input),
        start_(start),
        syllabifier_(translator->delimiters(),
                     translator->enable_completion(),
                     translator->strict_spelling()) {
    if (corrector) {
      syllabifier_.EnableCorrection(corrector);
    }
  }

  virtual Spans Syllabify(const Phrase* phrase);
  size_t BuildSyllableGraph(Prism& prism);
  string GetPreeditString(const Phrase& cand) const;
  string GetOriginalSpelling(const Phrase& cand) const;
  int ShixinTypoCorrectionCount(const Code& code, size_t code_length, shixin_typo::EditTrace* trace = nullptr) const;
  bool IsCorrection(const Code& code, size_t code_length) const;

  const SyllableGraph& syllable_graph() const { return syllable_graph_; }
  void ShixinTypoCarryBudget(const SyllableGraph& source) {
    syllable_graph_.CarryShixinTypoBudgetFrom(source);
  }

 protected:
  ScriptTranslator* translator_;
  string input_;
  size_t start_;
  Syllabifier syllabifier_;
  SyllableGraph syllable_graph_;
};

class ScriptTranslation : public Translation {
 public:
  ScriptTranslation(ScriptTranslator* translator,
                    Corrector* corrector,
                    Poet* poet,
                    const string& input,
                    size_t start,
                    size_t end_of_input,
                    int max_sentences,
                    double sentence_cutoff_threshold)
      : translator_(translator),
        poet_(poet),
        start_(start),
        end_of_input_(end_of_input),
        syllabifier_(
            New<ScriptSyllabifier>(translator, corrector, input, start)),
        enable_correction_(corrector),
        max_sentences_(max_sentences),
        sentence_cutoff_threshold_(sentence_cutoff_threshold) {
    set_exhausted(true);
  }
  bool Evaluate(Dictionary* dict, UserDictionary* user_dict);
  virtual bool Next();
  virtual an<Candidate> Peek();

 public:
  void ShixinTypoCarryBudget(const ScriptTranslation& source) {
    syllabifier_->ShixinTypoCarryBudget(source.syllabifier_->syllable_graph());
  }
  void ShixinTypoCarryBudget(const SyllableGraph& budget) { syllabifier_->ShixinTypoCarryBudget(budget); }
  void ShixinTypoCopyBudgetTo(SyllableGraph& budget) const { budget.CarryShixinTypoBudgetFrom(syllabifier_->syllable_graph()); }
  bool ShixinTypoBudgetAvailable() const {
    const auto& graph = syllabifier_->syllable_graph();
    return !graph.shixin_typo_exhausted && graph.shixin_typo_work_left &&
        graph.shixin_typo_nodes_left && graph.shixin_typo_chunks_left &&
        std::chrono::steady_clock::now() < graph.shixin_typo_deadline;
  }
  void ShixinTypoRecordStage(size_t index, bool found, bool available) const {
    if (!shixin_typo_diagnostics_enabled || index >= 2) return;
    const auto& graph = syllabifier_->syllable_graph();
    auto& stage = shixin_typo_diagnostics.stages[index];
    stage = {true, found, available, graph.shixin_typo_graph_complete,
             graph.shixin_typo_stop_reason, graph.shixin_typo_phase, graph.shixin_typo_work_left,
             graph.shixin_typo_nodes_left, graph.shixin_typo_chunks_left};
    // The final availability check can observe expiry without mutating graph.
    if (!available && stage.stop == ShixinTypoStopReason::None) {
      stage.stop = !stage.work_left ? ShixinTypoStopReason::Work
                 : !stage.nodes_left ? ShixinTypoStopReason::Nodes
                 : !stage.chunks_left ? ShixinTypoStopReason::Chunks
                 : std::chrono::steady_clock::now() >= graph.shixin_typo_deadline
                     ? ShixinTypoStopReason::Deadline : ShixinTypoStopReason::Other;
    }
  }
  size_t ShixinTypoPreferredAnchor(const string& raw, Table* table) const {
    const auto& graph = syllabifier_->syllable_graph();
    const size_t end = graph.input_length;
    if (end > 32) return 0;
    std::array<int,33> cost, anchor;
    cost.fill(100000); anchor.fill(-1); cost[end]=0;
    for (size_t p=end; p-->0;) {
      auto it=graph.edges.find(p);
      if (it==graph.edges.end()) continue;
      for (const auto& edge:it->second) {
        size_t next=edge.first;
        if (next>end || next<=p || cost[next]==100000) continue;
        // Graph edges consume delimiters, but those bytes are not part of a
        // canonical syllable and must not make an exact prefix look mistyped.
        size_t spelling_begin = p, spelling_end = next;
        const auto& delimiters = translator_->delimiters();
        while (spelling_begin < spelling_end && delimiters.find(raw[spelling_begin]) != string::npos) ++spelling_begin;
        while (spelling_end > spelling_begin && delimiters.find(raw[spelling_end-1]) != string::npos) --spelling_end;
        bool normal=false;
        for (const auto& spelling:edge.second)
          if (spelling.second.type==kNormalSpelling && !spelling.second.is_correction &&
              table->GetSyllableById(spelling.first)==raw.substr(spelling_begin,spelling_end-spelling_begin)) normal=true;
        int candidate=cost[next]+(normal?0:int(spelling_end-spelling_begin));
        if (candidate<cost[p]) {
          cost[p]=candidate;
          anchor[p]=normal?anchor[next]:int(spelling_begin);
        }
      }
    }
    return anchor[0]>=0?size_t(anchor[0]):0;
  }
  bool ShixinTypoCorrectionTrace(const Phrase& phrase, shixin_typo::EditTrace* trace) const {
    if (!trace || phrase.end() < start_ || phrase.end() > 32) return false;
    *trace = {};
    if (syllabifier_->ShixinTypoCorrectionCount(phrase.code(), phrase.end()-start_, trace) != 1 ||
        trace->kind == shixin_typo::EditTrace::Kind::None) return false;
    trace->begin += start_;
    trace->end += start_;
    return true;
  }
  int ShixinTypoCorrectionCount(const Phrase& phrase) const {
    return syllabifier_->ShixinTypoCorrectionCount(phrase.code(), phrase.end()-start_);
  }
 protected:
  bool CheckEmpty();
  bool IsNormalSpelling() const;
  bool PrepareCandidate();
  template <class QueryResult>
  void EnrollEntries(map<int, DictEntryList>& entries_by_end_pos,
                     const an<QueryResult>& query_result);
  WordGraph PrepareForMakingSentence(Dictionary* dict,
                                     UserDictionary* user_dict);
  an<Sentence> MakeSentence(Dictionary* dict, UserDictionary* user_dict);
  deque<an<Sentence>> MakeSentences(Dictionary* dict,
                                    UserDictionary* user_dict);

  ScriptTranslator* translator_;
  Poet* poet_;
  size_t start_;
  size_t end_of_input_;
  an<ScriptSyllabifier> syllabifier_;

  an<DictEntryCollector> phrase_;
  an<UserDictEntryCollector> user_phrase_;
  deque<an<Sentence>> sentences_;

  an<Phrase> candidate_ = nullptr;
  size_t candidate_index_ = 0;
  enum CandidateSource {
    kUninitialized,
    kUserPhrase,
    kSysPhrase,
    kSentence,
  };
  CandidateSource candidate_source_ = kUninitialized;

  DictEntryCollector::reverse_iterator phrase_iter_;
  UserDictEntryCollector::reverse_iterator user_phrase_iter_;

  const size_t max_corrections_ = 4;
  size_t correction_count_ = 0;
  int max_sentences_ = 1;
  double sentence_cutoff_threshold_ = 0.1;

  bool enable_correction_;
};

#include "protected_translation.inc"
#include "hypothesis_translation.inc"

// ScriptTranslator implementation

ScriptTranslator::ScriptTranslator(const Ticket& ticket)
    : Translator(ticket), Memory(ticket), TranslatorOptions(ticket) {
  if (!engine_)
    return;
  string shixin_typo_initial_cache_path, shixin_typo_mirror_cache_path, shixin_typo_canonical_path;
  auto resource_path=[](const string& name) {
    if(name.empty()) return name;
    path p(name);
    return (p.is_absolute()?p:Service::instance().deployer().staging_dir/p).u8string();
  };
  bool shixin_typo_warm_original = false, shixin_typo_warm_strings = false, shixin_typo_warm_indexes = false, shixin_typo_warm_entries = false;
  if (Config* config = engine_->schema()->config()) {
    config->GetInt(name_space_ + "/spelling_hints", &spelling_hints_);
    config->GetInt(name_space_ + "/max_word_length", &max_word_length_);
    config->GetInt(name_space_ + "/core_word_length", &core_word_length_);
    config->GetBool(name_space_ + "/always_show_comments",
                    &always_show_comments_);
    config->GetBool(name_space_ + "/enable_correction", &enable_correction_);
    config->GetBool(name_space_ + "/shixin_typo_protected", &shixin_typo_protect_);
    config->GetBool(name_space_ + "/shixin_typo_separate", &shixin_typo_separate_);
    string hypothesis_model;
    if (config->GetString(name_space_ + "/shixin_typo_hypothesis_model", &hypothesis_model)) {
      shixin_typo_hypotheses_.reset(new shixin_typo::HypothesisPolicy);
      if (!shixin_typo_hypotheses_->Load(resource_path(hypothesis_model))) {
        shixin_typo_hypotheses_.reset(); shixin_typo_mirror_unavailable_ = true;
      }
    }
    if (shixin_typo_protect_) {
      config->GetString(name_space_ + "/shixin_typo_initial_cache_file", &shixin_typo_initial_cache_path);
config->GetString(name_space_ + "/shixin_typo_mirror_cache_file", &shixin_typo_mirror_cache_path);
      config->GetString(name_space_ + "/shixin_typo_canonical_table_file", &shixin_typo_canonical_path);
      config->GetBool(name_space_ + "/shixin_typo_warm_original", &shixin_typo_warm_original);
      config->GetBool(name_space_ + "/shixin_typo_warm_strings", &shixin_typo_warm_strings);
      config->GetBool(name_space_ + "/shixin_typo_warm_indexes", &shixin_typo_warm_indexes);
      config->GetBool(name_space_ + "/shixin_typo_warm_short_entries", &shixin_typo_warm_entries);
      config->GetInt(name_space_ + "/shixin_typo_bound_probe_cap", &shixin_typo_probe_cap_);
      shixin_typo_probe_cap_=std::max(0,std::min(32768,shixin_typo_probe_cap_));
      config->GetInt(name_space_ + "/shixin_typo_work_cap", &shixin_typo_work_cap_);
      shixin_typo_work_cap_=std::max(0,std::min(32768,shixin_typo_work_cap_));
      constexpr const char* keys[] = {"date","time","week","datetime","timestamp","datezh","dateen"};
      for (size_t i=0;i<shixin_typo_reserved_codes_.size();i++)
        config->GetString(string("date_translator/")+keys[i],&shixin_typo_reserved_codes_[i]);
    }
    if (!config->GetBool(name_space_ + "/enable_word_completion",
                         &enable_word_completion_)) {
      enable_word_completion_ = enable_completion_;
    }
    config->GetInt(name_space_ + "/max_homophones", &max_homophones_);
    poet_.reset(new Poet(language(), config));
  }
  if (shixin_typo_protect_ && dict_ && dict_->loaded())
    for (const auto& table : dict_->tables()) {
  if (shixin_typo_warm_original && !table->WarmShixinTypoPages())
    shixin_typo_mirror_unavailable_ = true;
  if (shixin_typo_warm_strings && !table->WarmShixinTypoStrings())
    shixin_typo_mirror_unavailable_ = true;
  if ((shixin_typo_warm_indexes || shixin_typo_warm_entries) && !table->WarmShixinTypoIndexes(shixin_typo_warm_entries))
    shixin_typo_mirror_unavailable_ = true;
  if (!table->PrepareShixinTypoInitialCache(resource_path(shixin_typo_initial_cache_path)))
    shixin_typo_mirror_unavailable_ = true;
  if (!shixin_typo_canonical_path.empty() && !table->PrepareShixinTypoCanonicalTable(shixin_typo_canonical_path))
    shixin_typo_mirror_unavailable_ = true;
  if (!shixin_typo_mirror_cache_path.empty() && !table->PrepareShixinTypoMirrorCache(resource_path(shixin_typo_mirror_cache_path)))
    shixin_typo_mirror_unavailable_ = true;
}
  if (enable_correction_) {
    if (auto* corrector = Corrector::Require("corrector")) {
      corrector_.reset(corrector->Create(ticket));
    }
  }
}

an<Translation> ScriptTranslator::Query(const string& input,
                                        const Segment& segment) {
  if (!dict_ || !dict_->loaded())
    return nullptr;
  if (!segment.HasAnyTagIn(tags_))
    return nullptr;
  DLOG(INFO) << "input = '" << input << "', [" << segment.start << ", "
             << segment.end << ")";

  FinishSession();

  bool enable_user_dict =
      user_dict_ && user_dict_->loaded() && !IsUserDictDisabledFor(input);

  size_t end_of_input = engine_->context()->input().length();
  if (shixin_typo_protect_) {
    if (shixin_typo_diagnostics_enabled) { shixin_typo_diagnostics = {}; shixin_typo_diagnostics.input_bytes = input.size(); }
    auto ordinary = New<ScriptTranslation>(this, nullptr, poet_.get(), input,
        segment.start, end_of_input, max_sentences_, sentence_cutoff_threshold_);
    bool has_ordinary = ordinary->Evaluate(dict_.get(), enable_user_dict ? user_dict_.get() : nullptr);
    an<ScriptTranslation> recovery, broad;
    bool reserved = false;
    for (const auto& tag : segment.tags)
      if (tag == "punct" || tag == "number" || tag == "unicode" ||
          tag == "calculator" || tag == "radical_lookup" || tag == "email" ||
          tag == "url" || tag == "underscore" || tag == "gregorian_to_lunar")
        reserved = true;
    for (const auto& code : shixin_typo_reserved_codes_)
      if (!code.empty() && input == code) reserved = true;
    if (shixin_typo_hypotheses_ && shixin_typo_separate_) {
      auto pool=New<ShixinTypoHypothesisTranslation>();
      SyllableGraph budget;
      budget.shixin_typo_bounded=true;
      budget.shixin_typo_work_left=shixin_typo_work_cap_;
#ifdef SHIXIN_TYPO_SANITIZER_TEST
      budget.shixin_typo_deadline=std::chrono::steady_clock::now()+std::chrono::minutes(1);
#else
      budget.shixin_typo_deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(900);
#endif
      if (!reserved && !shixin_typo_mirror_unavailable_ && corrector_ && corrector_->ShixinTypoBounded()) {
        auto plan=shixin_typo_hypotheses_->Plan(input,budget);
        corrector_->ShixinTypoSpecific(false);
        corrector_->ShixinTypoAbbreviated(false);
        if (plan.complete) for(size_t h=0;h<plan.count && budget.CheckShixinTypoDeadline();++h) {
          corrector_->ShixinTypoTarget(&plan.hypotheses[h].edit);
          corrector_->ShixinTypoAbbreviated(shixin_typo::HypothesisPolicy::ThreeInitials(input) &&
              shixin_typo::HypothesisPolicy::ThreeInitials(plan.hypotheses[h].spelling()));
          auto lookup=New<ScriptTranslation>(this,corrector_.get(),poet_.get(),input,
              segment.start,end_of_input,max_sentences_,sentence_cutoff_threshold_);
          lookup->ShixinTypoCarryBudget(budget);
          bool found=lookup->Evaluate(dict_.get(),enable_user_dict?user_dict_.get():nullptr);
          for(size_t i=0;found && i<10 && !lookup->exhausted() && lookup->ShixinTypoBudgetAvailable();++i) {
            auto c=lookup->Peek();
            auto phrase=As<Phrase>(c);
            shixin_typo::EditTrace trace;
            if (phrase && lookup->ShixinTypoCorrectionTrace(*phrase,&trace))
              pool->Add(h,New<ShixinTypoCorrectionCandidate>(c,trace));
            // Never advance into an eleventh native entry.
            if(i+1<10) lookup->Next();
          }
          lookup->ShixinTypoRecordStage(std::min(h,size_t(1)),found,lookup->ShixinTypoBudgetAvailable());
          lookup->ShixinTypoCopyBudgetTo(budget);
          if(!budget.CheckShixinTypoDeadline()) { pool->Discard(h); break; }
        }
        corrector_->ShixinTypoTarget(nullptr);
        corrector_->ShixinTypoSpecific(false);
        corrector_->ShixinTypoAbbreviated(false);
      }
      pool->Finish();
      return New<ShixinTypoSeparateTranslation>(has_ordinary?New<DistinctTranslation>(ordinary):nullptr,pool);
    }
    if (!reserved && !shixin_typo_mirror_unavailable_ && input.size() >= 3 && input.size() <= 32 && corrector_ && corrector_->ShixinTypoBounded()) {
      const auto preferred_anchor=ordinary->ShixinTypoPreferredAnchor(input,dict_->primary_table().get());
      if (shixin_typo_diagnostics_enabled) shixin_typo_diagnostics.preferred_anchor = preferred_anchor;
      const bool cache_first=input.size()==3;
      corrector_->ShixinTypoAnchor(cache_first?size_t(-1):preferred_anchor);
      corrector_->ShixinTypoSpecific(!cache_first);
      corrector_->ShixinTypoAbbreviated(cache_first);
      recovery = New<ScriptTranslation>(this, corrector_.get(), poet_.get(), input,
          segment.start, end_of_input, max_sentences_, sentence_cutoff_threshold_);
      bool has_recovery = recovery->Evaluate(dict_.get(), enable_user_dict ? user_dict_.get() : nullptr);
      const bool recovery_available = recovery->ShixinTypoBudgetAvailable();
      recovery->ShixinTypoRecordStage(0, has_recovery, recovery_available);
      if (recovery_available) {
        corrector_->ShixinTypoSpecific(cache_first);
        corrector_->ShixinTypoAbbreviated(false);
        // A fully valid alternate segmentation can hide the typo position.
        // The second stage explores other exact-prefix boundaries under the
        // same carried composition budget, rather than repeating one anchor.
        corrector_->ShixinTypoAnchor(size_t(-1));
        broad = New<ScriptTranslation>(this, corrector_.get(), poet_.get(), input,
            segment.start, end_of_input, max_sentences_, sentence_cutoff_threshold_);
        broad->ShixinTypoCarryBudget(*recovery);
        const bool has_broad=broad->Evaluate(dict_.get(), enable_user_dict ? user_dict_.get() : nullptr);
        const bool incomplete=!broad->ShixinTypoBudgetAvailable();
        broad->ShixinTypoRecordStage(1, has_broad, !incomplete);
        // The first correction stage completed before this optional second
        // stage began. Its validated results remain eligible on later expiry.
        if(!has_broad || incomplete) broad.reset();
      } else {
        recovery.reset();
      }
      corrector_->ShixinTypoSpecific(false);
      corrector_->ShixinTypoAbbreviated(false);
      if (!has_recovery) recovery.reset();
    }
    if (!has_ordinary && !recovery && !broad) return nullptr;
    if (shixin_typo_separate_)
      return New<ShixinTypoSeparateTranslation>(
          has_ordinary ? New<DistinctTranslation>(ordinary) : nullptr,
          New<ShixinTypoRecoveryTranslation>(recovery, broad, input.size(), input.size()==3 ? 2 : 4));
    int page = 5;
    engine_->schema()->config()->GetInt("menu/page_size", &page);
    page = std::max(1, std::min(10, page));
    return New<ShixinTypoProtectedTranslation>(
        has_ordinary ? New<DistinctTranslation>(ordinary) : nullptr, recovery, broad, page, input.size(), input.size()==3 ? 2 : 4);
  }
  // the translator should survive translations it creates
  auto result = New<ScriptTranslation>(
      this, corrector_.get(), poet_.get(), input, segment.start, end_of_input,
      max_sentences_, sentence_cutoff_threshold_);
  if (!result || !result->Evaluate(
                     dict_.get(), enable_user_dict ? user_dict_.get() : NULL)) {
    return nullptr;
  }
  auto deduped = New<DistinctTranslation>(result);
  if (contextual_suggestions_) {
    return poet_->ContextualWeighted(deduped, input, segment.start, this);
  }
  return deduped;
}

int ScriptTranslator::core_word_length() const {
  if (max_word_length_ <= 0) {
    return core_word_length_;
  }
  return (std::min)(core_word_length_, max_word_length_);
}

static bool exceed_upperlimit(int length, int upper_limit) {
  return upper_limit > 0 && length > upper_limit;
}

bool ScriptTranslator::SaveCommitEntry(CommitEntry& commit_entry) {
  if (exceed_upperlimit(commit_entry.Length(), max_word_length())) {
    UpdateElements(commit_entry);
  } else {
    commit_entry.Save();
  }
  return true;
}

bool ScriptTranslator::ConcatenatePhrases(CommitEntry& commit_entry,
                                          const vector<an<Phrase>>& phrases) {
  const int kCoreWordLength = core_word_length();
  const int n = phrases.size();
  for (int i = 0; i < n; ++i) {
    int cur_len = 0;
    int j = i;
    for (; j < n; ++j) {
      commit_entry.AppendPhrase(phrases.at(j));
      cur_len += phrases.at(j)->code().size();
      if (kCoreWordLength <= 0 || cur_len > kCoreWordLength) {
        break;
      }
      SaveCommitEntry(commit_entry);
    }
    if (kCoreWordLength > 0) {
      if (j == i) {
        SaveCommitEntry(commit_entry);
      }
      commit_entry.Clear();
    }
  }
  SaveCommitEntry(commit_entry);
  commit_entry.Clear();

  return true;
}

bool ScriptTranslator::ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                              const Segment& seg) {
  auto phrase =
      As<Phrase>(Candidate::GetGenuineCandidate(seg.GetSelectedCandidate()));
  bool recognized = Language::intelligible(phrase, this);
  if (recognized) {
    queue_.push_back(phrase);
  }

  if (!recognized || seg.status >= Segment::kConfirmed) {
    ConcatenatePhrases(commit_entry, queue_);
    queue_.clear();
  }

  return true;
}

string ScriptTranslator::FormatPreedit(const string& preedit) {
  string result = preedit;
  preedit_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::Spell(const Code& code) {
  string result;
  vector<string> syllables;
  if (!dict_ || !dict_->Decode(code, &syllables) || syllables.empty())
    return result;
  result = boost::algorithm::join(syllables, string(1, delimiters_.at(0)));
  comment_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::GetPrecedingText(size_t start) const {
  return !contextual_suggestions_ ? string()
         : start > 0 ? engine_->context()->composition().GetTextBefore(start)
                     : engine_->context()->commit_history().latest_text();
}

bool ScriptTranslator::UpdateElements(const CommitEntry& commit_entry) {
  bool update_elements = false;
  // avoid updating single character entries within a phrase which is
  // composed with single characters only
  if (commit_entry.elements.size() > 1) {
    for (const DictEntry* e : commit_entry.elements) {
      if (e->code.size() > 1) {
        update_elements = true;
        break;
      }
    }
  }
  if (update_elements) {
    for (const DictEntry* e : commit_entry.elements) {
      user_dict_->UpdateEntry(*e, 0);
    }
  }
  return true;
}

bool ScriptTranslator::Memorize(const CommitEntry& commit_entry) {
  UpdateElements(commit_entry);
  user_dict_->UpdateEntry(commit_entry, 1);
  return true;
}

// ScriptSyllabifier implementation

Spans ScriptSyllabifier::Syllabify(const Phrase* phrase) {
  Spans result;
  vector<size_t> vertices;
  vertices.push_back(start_);
  SyllabifyTask task{
      phrase->code(), syllable_graph_, phrase->end() - start_,
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) { vertices.push_back(start_ + next_pos); },
      [&](SyllabifyTask* task, size_t depth) { vertices.pop_back(); }};
  if (syllabify_dfs(&task, 0, phrase->start() - start_)) {
    result.set_vertices(std::move(vertices));
  }
  return result;
}

size_t ScriptSyllabifier::BuildSyllableGraph(Prism& prism) {
  syllable_graph_.shixin_typo_probe_cap=translator_->shixin_typo_probe_cap();
  if (!syllable_graph_.shixin_typo_budget_carried)
    syllable_graph_.shixin_typo_work_left=translator_->shixin_typo_work_cap();
  return (size_t)syllabifier_.BuildSyllableGraph(input_, prism,
                                                 &syllable_graph_);
}

bool ScriptSyllabifier::IsCorrection(const Code& code,
                                     size_t code_length) const {
  vector<bool> path_attributes;
  path_attributes.reserve(8);

  SyllabifyTask task{
      code, syllable_graph_, code_length,
      // push
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) {
        auto id = task->code[depth];

        auto start_iter = syllable_graph_.edges.find(current_pos);
        if (start_iter != syllable_graph_.edges.end()) {
          auto end_iter = start_iter->second.find(next_pos);
          if (end_iter != start_iter->second.end()) {
            auto prop_iter = end_iter->second.find(id);
            if (prop_iter != end_iter->second.end()) {
              path_attributes.push_back(prop_iter->second.is_correction);
              return;
            }
          }
        }
        // edge not found
        path_attributes.push_back(false);
      },
      // pop
      [&](SyllabifyTask* task, size_t depth) { path_attributes.pop_back(); }};

  if (syllabify_dfs(&task, 0, 0)) {
    for (bool is_correction : path_attributes) {
      if (is_correction)
        return true;
    }
  }
  return false;
}

int ScriptSyllabifier::ShixinTypoCorrectionCount(const Code& code,
    size_t code_length, shixin_typo::EditTrace* trace) const {
  if (trace) *trace = {};
  if (code.size() > 32) return 99;
  std::array<const SpellingProperties*, 32> path{};
  SyllabifyTask task{
      code, syllable_graph_, code_length,
      [&](SyllabifyTask* task, size_t depth, size_t current_pos, size_t next_pos) {
        path[depth] = nullptr;
        auto begin = syllable_graph_.edges.find(current_pos);
        if (begin == syllable_graph_.edges.end()) return;
        auto end = begin->second.find(next_pos);
        if (end == begin->second.end()) return;
        auto prop = end->second.find(task->code[depth]);
        if (prop != end->second.end()) path[depth] = &prop->second;
      },
      [&](SyllabifyTask*, size_t depth) { path[depth] = nullptr; }};
  size_t budget = 1024;
  if (!syllabify_dfs(&task, 0, 0, &budget)) return 99;
  int count = 0;
  for (size_t depth = 0; depth < code.size(); ++depth) {
    if (!path[depth]) return 99;
    if (path[depth]->is_correction) {
      ++count;
      if (trace) *trace = path[depth]->shixin_typo_trace;
    }
  }
  if (trace && count != 1) *trace = {};
  return count;
}

string ScriptSyllabifier::GetPreeditString(const Phrase& cand) const {
  const auto& delimiters = translator_->delimiters();
  std::stack<size_t> lengths;
  string output;
  SyllabifyTask task{cand.matching_code(), syllable_graph_, cand.end() - start_,
                     [&](SyllabifyTask* task, size_t depth, size_t current_pos,
                         size_t next_pos) {
                       size_t len = output.length();
                       if (depth > 0 && len > 0 &&
                           delimiters.find(output[len - 1]) == string::npos) {
                         output += delimiters.at(0);
                       }
                       output +=
                           input_.substr(current_pos, next_pos - current_pos);
                       lengths.push(len);
                     },
                     [&](SyllabifyTask* task, size_t depth) {
                       output.resize(lengths.top());
                       lengths.pop();
                     }};
  if (syllabify_dfs(&task, 0, cand.start() - start_)) {
    return translator_->FormatPreedit(output);
  } else {
    return string();
  }
}

string ScriptSyllabifier::GetOriginalSpelling(const Phrase& cand) const {
  if (translator_ &&
      static_cast<int>(cand.code().size()) <= translator_->spelling_hints()) {
    return translator_->Spell(cand.code());
  }
  return string();
}

template <class Ptr, class Iter>
static bool has_exact_match_phrase(Ptr ptr, Iter iter, size_t consumed) {
  return ptr && iter->first == consumed && !iter->second.exhausted() &&
         iter->second.Peek()->IsExactMatch();
}

// ScriptTranslation implementation

bool ScriptTranslation::Evaluate(Dictionary* dict, UserDictionary* user_dict) {
  size_t consumed = syllabifier_->BuildSyllableGraph(*dict->prism());
  const auto& syllable_graph = syllabifier_->syllable_graph();
  if (syllable_graph.shixin_typo_exhausted) return false;
  bool predict_word = !syllable_graph.shixin_typo_bounded && translator_->enable_word_completion() &&
                      start_ + consumed == end_of_input_;

  phrase_ =
      dict->Lookup(syllable_graph, 0, &translator_->blacklist(), predict_word);
  // User-dictionary traversal is unbounded; corrections use only the system
  // dictionary. Ordinary lookup and explicit-selection learning are unchanged.
  if (user_dict && !syllable_graph.shixin_typo_bounded && !syllable_graph.shixin_typo_exhausted && !syllable_graph.shixin_typo_three_initials) {
    if (syllable_graph.shixin_typo_bounded) syllable_graph.shixin_typo_phase = ShixinTypoSearchPhase::UserLookup;
    const size_t kUnlimitedDepth = 0;
    const size_t kNumSyllablesToPredictWord = 4;
    user_phrase_ =
        user_dict->Lookup(syllable_graph, 0, kUnlimitedDepth,
                          predict_word ? kNumSyllablesToPredictWord : 0);
  }
  if (!phrase_ && !user_phrase_)
    return false;

  if (phrase_)
    phrase_iter_ = phrase_->rbegin();
  if (user_phrase_)
    user_phrase_iter_ = user_phrase_->rbegin();

  // A completed graph can safely support entries materialized before lookup
  // exhaustion. Do not spend more time constructing sentences or user lookups.
  if (syllable_graph.shixin_typo_exhausted) return !CheckEmpty();

  auto is_correction_match = [&](auto& iter_pair, size_t len) {
    if (iter_pair.first != len || iter_pair.second.exhausted())
      return false;
    return syllabifier_->IsCorrection(iter_pair.second.Peek()->code, len);
  };

  bool has_reliable_phrase =
      has_exact_match_phrase(phrase_, phrase_iter_, consumed) &&
      !is_correction_match(*phrase_iter_, consumed);

  bool has_reliable_user_phrase =
      has_exact_match_phrase(user_phrase_, user_phrase_iter_, consumed) &&
      !is_correction_match(*user_phrase_iter_, consumed);

  bool has_at_least_two_syllables = syllable_graph.edges.size() >= 2;
  DLOG(INFO) << "consumed: " << consumed
             << ", has_reliable_phrase: " << has_reliable_phrase
             << ", has_reliable_user_phrase: " << has_reliable_user_phrase
             << ", has_at_least_two_syllables: " << has_at_least_two_syllables;
  // make sentences when there is no exact-matching phrase candidate
  const bool shixin_typo_has_full_phrase = syllable_graph.shixin_typo_bounded &&
      (has_exact_match_phrase(phrase_, phrase_iter_, consumed) ||
       has_exact_match_phrase(user_phrase_, user_phrase_iter_, consumed));
  if (has_at_least_two_syllables && !has_reliable_phrase &&
      !has_reliable_user_phrase && !shixin_typo_has_full_phrase) {
    if (syllable_graph.shixin_typo_bounded) syllable_graph.shixin_typo_phase = ShixinTypoSearchPhase::Sentence;
    if (max_sentences_ > 1)
      sentences_ = MakeSentences(dict, user_dict);
    else if (max_sentences_) {
      auto sentence = MakeSentence(dict, user_dict);
      if (sentence)
        sentences_ = {sentence};
      else
        sentences_.clear();
    }
  }

  if (syllable_graph.shixin_typo_exhausted) return false;
  if (syllable_graph.shixin_typo_bounded) syllable_graph.shixin_typo_phase = ShixinTypoSearchPhase::Ready;
  return !CheckEmpty();
}

bool ScriptTranslation::Next() {
  do {
    if (exhausted())
      return false;
    if (candidate_source_ == kUninitialized) {
      PrepareCandidate();  // to determine candidate_source_
    }
    switch (candidate_source_) {
      case kUninitialized:
        break;
      case kSentence:
        if (!sentences_.empty())
          sentences_.pop_front();
        break;
      case kUserPhrase: {
        UserDictEntryIterator& uter(user_phrase_iter_->second);
        if (!uter.Next()) {
          ++user_phrase_iter_;
        }
      } break;
      case kSysPhrase: {
        DictEntryIterator& iter(phrase_iter_->second);
        if (!iter.Next()) {
          ++phrase_iter_;
        }
      } break;
    }
    candidate_.reset();
    candidate_source_ = kUninitialized;
    if (enable_correction_) {
      // populate next candidate and skip it if it's a correction beyond max
      // numbers.
      if (!PrepareCandidate()) {
        break;
      }
    }
  } while (enable_correction_ && !translator_->shixin_typo_protect() &&
           syllabifier_->IsCorrection(candidate_->code(),
                                      candidate_->end() - start_) &&
           // limit the number of correction candidates
           ++correction_count_ > max_corrections_);
  if (!CheckEmpty()) {
    ++candidate_index_;
    return true;
  }
  return false;
}

an<Candidate> ScriptTranslation::Peek() {
  if (candidate_source_ == kUninitialized && !PrepareCandidate()) {
    return nullptr;
  }
  if (candidate_->preedit().empty()) {
    candidate_->set_preedit(syllabifier_->GetPreeditString(*candidate_));
  }
  if (candidate_->comment().empty()) {
    auto spelling = syllabifier_->GetOriginalSpelling(*candidate_);
    if (!spelling.empty() && (translator_->always_show_comments() ||
                              spelling != candidate_->preedit())) {
      candidate_->set_comment(/*quote_left + */ spelling /* + quote_right*/);
    }
  }
  candidate_->set_syllabifier(syllabifier_);
  return candidate_;
}

static bool always_true() {
  return true;
}

template <typename T>
inline static bool prefer_user_phrase(
    T user_phrase_weight,
    T sys_phrase_weight,
    function<bool()> compare_on_tie = always_true) {
  return user_phrase_weight > sys_phrase_weight ||
         (user_phrase_weight == sys_phrase_weight && compare_on_tie());
}

bool ScriptTranslation::PrepareCandidate() {
iter_incremented:
  if (exhausted()) {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
  if (!sentences_.empty()) {
    candidate_source_ = kSentence;
    candidate_ = sentences_[0];
    return true;
  }
  const size_t full_code_length = end_of_input_ - start_;
  size_t user_phrase_code_length = 0;
  if (user_phrase_ && user_phrase_iter_ != user_phrase_->rend()) {
    user_phrase_code_length = user_phrase_iter_->first;
  }
  size_t phrase_code_length = 0;
  if (phrase_ && phrase_iter_ != phrase_->rend()) {
    phrase_code_length = phrase_iter_->first;
  }
  if (user_phrase_code_length > 0 &&
      prefer_user_phrase(
          user_phrase_code_length, phrase_code_length,
          // 編碼長度相同時, 用戶詞優先
          [this, full_code_length, phrase_code_length]() {
            UserDictEntryIterator& uter = user_phrase_iter_->second;
            DictEntryIterator& iter = phrase_iter_->second;
            // 但若用戶詞爲糾錯結果, 則免去優先權, 回歸權重比較
            bool user_is_correction = syllabifier_->IsCorrection(
                uter.Peek()->code, phrase_code_length);
            if (user_is_correction) {
              // 因對糾錯候選施加了較大懲罰, 通常結果爲 系統原文 > 用戶糾錯
              // 兩者皆糾錯時, 用戶詞的動態權重有助達成 用戶糾錯 > 系統糾錯
              return uter.Peek()->weight >= iter.Peek()->weight;
            }
            // 長詞聯想之前須至少出一個嚴格匹配的候選
            // 故確定首選之際, 系統嚴格匹配 > 用戶長詞聯想
            const int kNumExactMatchOnTop = 1;
            return candidate_index_ >= kNumExactMatchOnTop ||
                   prefer_user_phrase(
                       has_exact_match_phrase(user_phrase_, user_phrase_iter_,
                                              full_code_length),
                       has_exact_match_phrase(phrase_, phrase_iter_,
                                              full_code_length));
          })) {
    UserDictEntryIterator& uter = user_phrase_iter_->second;
    const auto& entry = uter.Peek();
    DLOG(INFO) << "user phrase '" << entry->text
               << "', code length: " << user_phrase_code_length;
    candidate_source_ = kUserPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "user_phrase",
                    start_, start_ + user_phrase_code_length, entry);
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else if (phrase_code_length > 0) {
    DictEntryIterator& iter = phrase_iter_->second;
    if (iter.exhausted()) {
      ++phrase_iter_;
      goto iter_incremented;
    }
    const auto& entry = iter.Peek();
    DLOG(INFO) << "phrase '" << entry->text
               << "', code length: " << phrase_code_length;
    candidate_source_ = kSysPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "phrase",
                    start_, start_ + phrase_code_length, entry);
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
}

bool ScriptTranslation::CheckEmpty() {
  set_exhausted((!phrase_ || phrase_iter_ == phrase_->rend()) &&
                (!user_phrase_ || user_phrase_iter_ == user_phrase_->rend()));
  return exhausted();
}

template <class QueryResult>
void ScriptTranslation::EnrollEntries(
    map<int, DictEntryList>& entries_by_end_pos,
    const an<QueryResult>& query_result) {
  if (query_result) {
    for (auto& y : *query_result) {
      DictEntryList& homophones = entries_by_end_pos[y.first];
      while (homophones.size() < translator_->max_homophones() &&
             !y.second.exhausted()) {
        homophones.push_back(y.second.Peek());
        if (!y.second.Next())
          break;
      }
    }
  }
}

WordGraph ScriptTranslation::PrepareForMakingSentence(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const int kMaxSyllablesForUserPhraseQuery = 5;
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph;
  for (const auto& x : syllable_graph.edges) {
    auto& same_start_pos = graph[x.first];
    if (user_dict) {
      EnrollEntries(same_start_pos,
                    user_dict->Lookup(syllable_graph, x.first,
                                      kMaxSyllablesForUserPhraseQuery));
    }
    // merge lookup results
    EnrollEntries(same_start_pos, dict->Lookup(syllable_graph, x.first,
                                               &translator_->blacklist()));
  }
  return graph;
}

deque<an<Sentence>> ScriptTranslation::MakeSentences(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  auto sentences =
      poet_->MakeSentences(graph, syllable_graph.interpreted_length,
                           translator_->GetPrecedingText(start_),
                           max_sentences_, sentence_cutoff_threshold_);
  for (auto& sentence : sentences) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
  }
  return sentences;
}

an<Sentence> ScriptTranslation::MakeSentence(Dictionary* dict,
                                             UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  if (auto sentence =
          poet_->MakeSentence(graph, syllable_graph.interpreted_length,
                              translator_->GetPrecedingText(start_))) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
    return sentence;
  }
  return nullptr;
}

}  // namespace rime
