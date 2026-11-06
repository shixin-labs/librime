//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2013-01-06 GONG Chen <chen.sst@gmail.com>
//
#include <rime/candidate.h>

namespace rime {

// Scratch: conversion may wrap a correction shadow (and uniquification may
// wrap either). Preserve the native identity through a bounded wrapper chain.
// Reject malformed/deeper chains rather than recursing without a limit.
an<Candidate> Candidate::GetGenuineCandidate(const an<Candidate>& cand) {
  auto current = cand;
  for (size_t depth = 0; current; ++depth) {
    if (auto shadow = As<ShadowCandidate>(current)) {
      if (depth == 8) return nullptr;
      current = shadow->item();
    } else if (auto unique = As<UniquifiedCandidate>(current)) {
      if (depth == 8 || unique->items().empty()) return nullptr;
      current = unique->items().front();
    } else {
      return current;
    }
  }
  return nullptr;
}

vector<of<Candidate>> Candidate::GetGenuineCandidates(const an<Candidate>& cand) {
  vector<of<Candidate>> result;
  if (auto uniquified = As<UniquifiedCandidate>(cand)) {
    for (const auto& item : uniquified->items()) {
      if (auto genuine = GetGenuineCandidate(item)) result.push_back(genuine);
    }
  } else if (auto genuine = GetGenuineCandidate(cand)) {
    result.push_back(genuine);
  }
  return result;
}

int Candidate::compare(const Candidate& other) {
  // the one nearer to the beginning of segment comes first
  int k = start_ - other.start_;
  if (k != 0)
    return k;
  // then the longer comes first
  k = end_ - other.end_;
  if (k != 0)
    return -k;
  // compare quality
  double qdiff = quality_ - other.quality_;
  if (qdiff != 0.)
    return (qdiff > 0.) ? -1 : 1;
  // draw
  return 0;
}

}  // namespace rime
