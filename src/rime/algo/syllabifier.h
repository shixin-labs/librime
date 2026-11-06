//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-7-12 Zou xu <zouivex@gmail.com>
//

#ifndef RIME_SYLLABIFIER_H_
#define RIME_SYLLABIFIER_H_

#include <stdint.h>
#include <chrono>
#include <rime_api.h>
#include <rime/common.h>
#include "spelling.h"

namespace rime {

class Prism;
class Corrector;

using SyllableId = int32_t;

struct EdgeProperties : SpellingProperties {
  EdgeProperties(SpellingProperties sup) : SpellingProperties(sup) {};
  EdgeProperties() = default;
  // 切分歧義編碼段的起始位置
  set<size_t> ambiguous_source_positions;
};

using SpellingMap = map<SyllableId, EdgeProperties>;
using VertexMap = map<size_t, SpellingType>;
using EndVertexMap = map<size_t, SpellingMap>;
using EdgeMap = map<size_t, EndVertexMap>;

using SpellingPropertiesList = vector<const EdgeProperties*>;
using SpellingIndex = map<SyllableId, SpellingPropertiesList>;
using SpellingIndices = map<size_t, SpellingIndex>;

enum class ShixinTypoSearchPhase : uint8_t { Graph, Lookup, Sort, UserLookup, Sentence, Ready, BeforeSort };

enum class ShixinTypoStopReason : uint8_t { None, Deadline, Work, Nodes, Chunks, Other };

struct SyllableGraph {
  bool shixin_typo_bounded = false;
  bool shixin_typo_budget_carried = false;
  bool shixin_typo_specific_only = false;
  bool shixin_typo_three_initials = false;
  bool shixin_typo_initial_exact = false;
  size_t shixin_typo_probe_cap = 32768;
  std::array<char,3> shixin_typo_raw_initials{};
  bool shixin_typo_graph_complete = false;
  mutable size_t shixin_typo_work_left = 32768;
  mutable size_t shixin_typo_nodes_left = 2048, shixin_typo_chunks_left = 2048;
  std::chrono::steady_clock::time_point shixin_typo_deadline;
  mutable bool shixin_typo_exhausted = false;
  mutable ShixinTypoStopReason shixin_typo_stop_reason = ShixinTypoStopReason::None;
  mutable ShixinTypoSearchPhase shixin_typo_phase = ShixinTypoSearchPhase::Graph;
  bool FailShixinTypo(ShixinTypoStopReason reason) const {
    if (!shixin_typo_exhausted) shixin_typo_stop_reason = reason;
    shixin_typo_exhausted = true;
    return false;
  }
  void CarryShixinTypoBudgetFrom(const SyllableGraph& source) {
    shixin_typo_budget_carried = true;
    shixin_typo_work_left = source.shixin_typo_work_left;
    shixin_typo_nodes_left = source.shixin_typo_nodes_left;
    shixin_typo_chunks_left = source.shixin_typo_chunks_left;
    shixin_typo_deadline = source.shixin_typo_deadline;
    shixin_typo_exhausted = source.shixin_typo_exhausted;
    shixin_typo_stop_reason = source.shixin_typo_stop_reason;
  }
  bool CheckShixinTypoDeadline() const {
    if (!shixin_typo_bounded) return true;
    if (shixin_typo_exhausted) return false;
    if (std::chrono::steady_clock::now() >= shixin_typo_deadline)
      return FailShixinTypo(ShixinTypoStopReason::Deadline);
    return true;
  }
  bool SpendShixinTypoWork() const {
    if (!shixin_typo_bounded) return true;
    if (shixin_typo_exhausted) return false;
    if (!shixin_typo_work_left) return FailShixinTypo(ShixinTypoStopReason::Work);
    if ((shixin_typo_work_left & 63) == 0 && std::chrono::steady_clock::now() >= shixin_typo_deadline)
      return FailShixinTypo(ShixinTypoStopReason::Deadline);
    --shixin_typo_work_left;
    return true;
  }
  bool SpendShixinTypoNode() const {
    if (!shixin_typo_bounded) return true;
    if (!shixin_typo_nodes_left) return FailShixinTypo(ShixinTypoStopReason::Nodes);
    --shixin_typo_nodes_left; return SpendShixinTypoWork();
  }
  bool SpendShixinTypoChunk() const {
    if (!shixin_typo_bounded) return true;
    if (!shixin_typo_chunks_left) return FailShixinTypo(ShixinTypoStopReason::Chunks);
    --shixin_typo_chunks_left; return SpendShixinTypoWork();
  }
  size_t input_length = 0;
  size_t interpreted_length = 0;
  VertexMap vertices;
  EdgeMap edges;
  SpellingIndices indices;
};

class Syllabifier {
 public:
  Syllabifier() = default;
  explicit Syllabifier(const string& delimiters,
                       bool enable_completion = false,
                       bool strict_spelling = false)
      : delimiters_(delimiters),
        enable_completion_(enable_completion),
        strict_spelling_(strict_spelling) {}

  RIME_DLL int BuildSyllableGraph(const string& input,
                                  Prism& prism,
                                  SyllableGraph* graph);
  RIME_DLL void EnableCorrection(Corrector* corrector);

 protected:
  void CheckOverlappedSpellings(SyllableGraph* graph, size_t start, size_t end);
  void Transpose(SyllableGraph* graph);

  string delimiters_;
  bool enable_completion_ = false;
  bool strict_spelling_ = false;
  Corrector* corrector_ = nullptr;
};

}  // namespace rime

#endif  // RIME_SYLLABIFIER_H_
