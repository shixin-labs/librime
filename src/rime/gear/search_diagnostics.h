#pragma once
// Scratch-only, engine-thread diagnostics. No raw text or candidate text.
#include <rime/common.h>
#include <rime/algo/syllabifier.h>
namespace rime {
struct ShixinTypoStageDiagnostics {
  bool evaluated = false, found = false, available = false, graph_complete = false;
  ShixinTypoStopReason stop = ShixinTypoStopReason::None;
  ShixinTypoSearchPhase phase = ShixinTypoSearchPhase::Graph;
  size_t work_left = 0, nodes_left = 0, chunks_left = 0;
};
struct ShixinTypoSearchDiagnostics {
  size_t input_bytes = 0, preferred_anchor = 0;
  ShixinTypoStageDiagnostics stages[2];
};
static_assert(sizeof(ShixinTypoSearchDiagnostics) <= 128, "bounded diagnostic snapshot");
RIME_DLL void EnableShixinTypoSearchDiagnostics(bool enabled);
RIME_DLL ShixinTypoSearchDiagnostics ReadShixinTypoSearchDiagnostics();
} // namespace rime
