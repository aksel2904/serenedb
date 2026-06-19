////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "basics/empty.hpp"
#include "disjunction.hpp"
#include "iresearch/analysis/token_attributes.hpp"
#include "iresearch/formats/posting/iterator_pos.hpp"
#include "iresearch/index/iterators.hpp"
#include "iresearch/search/score_function.hpp"

// Sloppy phrase frequency over per-slot position lists.
//
// Matching: for each candidate lead position Run runs a pruned DFS
// (CountFromLead) over the remaining slots in phrase order (slots
// 0..n-1) and accumulates freq + best_distance. StepCost is directional,
// so the slot order is fixed. The DFS walks only the slop-reachable
// [lo, hi] slice of each slot and prunes on the running cost, so its work
// is proportional to the valid (and near-valid) tuples; it never
// materialises a per-lead window. The n == 2 case has a dedicated fast
// path. EnumerateAll is a separate DFS that lists every valid tuple for
// highlighting; CountFromLead mirrors its pruning so the scored freq
// matches the enumerated tuple count exactly.
//
// Gather: positions can be collected two ways. read-all reads every
// slot fully. seek-gather reads only the rarest slot fully, then seeks
// the other slots into the slop-reachable windows around it
// (BuildWindows); by the coverage lemma |p_a - p_b| <= slop +
// sum_expected this drops no match, so matching is identical either way -
// only fewer positions reach it. ResolveSeekGather picks the path
// (kSeekGatherSkew heuristic, overridable by the gGatherOverride test
// seam). Note the window is anchored on the rarest slot, not slot 0.
//
// expected_step == 1 keeps the original SlopPhraseFrequency formula
// bit-for-bit; expected_step > 1 supports per-slot positional gaps
// requested via push_back(term, offs). Interval gaps (offs_min !=
// offs_max) combined with slop are rejected upstream by SDB_ENSURE.

namespace irs {

template<typename Frequency>
class PhrasePosition;
template<typename T>
struct HasPosition;

namespace detail::slop_dp {

// Step cost for one slot transition.
//
// expected == 1 (regular phrase term, adjacent to previous):
//   delta >= 2  -> delta - 1   (forward gap)
//   delta <= -1 -> -delta + 1  (reversal)
//   delta == 0  -> 1
//   delta == 1  -> 0
//
// expected > 1 (push_back(term, offs) requested a gap):
//   delta == expected           -> 0
//   delta > expected            -> delta - expected      (too far)
//   0 <= delta < expected       -> expected - delta      (too close)
//   delta < 0                   -> expected - delta + 1  (reversal)
constexpr PosAttr::value_t StepCost(int64_t delta,
                                    PosAttr::value_t expected) noexcept {
  if (expected == 1) {
    if (delta >= 2) {
      return static_cast<PosAttr::value_t>(delta - 1);
    }
    if (delta <= -1) {
      return static_cast<PosAttr::value_t>(-delta + 1);
    }
    if (delta == 0) {
      return 1;  // same position: one move to separate the two slots
    }
    return 0;  // delta == 1: already adjacent
  }
  const int64_t exp = static_cast<int64_t>(expected);
  if (delta == exp) {
    return 0;
  }
  if (delta > exp) {
    return static_cast<PosAttr::value_t>(delta - exp);
  }
  if (delta >= 0) {
    return static_cast<PosAttr::value_t>(exp - delta);
  }
  return static_cast<PosAttr::value_t>(exp - delta + 1);
}

struct DpResult {
  uint64_t freq = 0;
  PosAttr::value_t best_distance = 0;
  bool any = false;
};

// Reusable scratch passed from the Frequency object into Run. Holds the
// per-lead DFS chain; its capacity is reused across leads and documents
// instead of being reallocated on each call (Match runs once per
// candidate doc, Run walks every lead).
struct DpScratch {
  std::vector<PosAttr::value_t> chain;
};

// Disjoint, ascending [lo, hi] gather interval (inclusive).
using Window = std::pair<PosAttr::value_t, PosAttr::value_t>;

// Engage seek-based gather (read the rarest slot, seek the rest into
// slop-reachable windows) only when the rarest slot is at least this
// many times rarer than the densest. Conservative on purpose: when
// term frequencies are balanced the plain read-all path is the safe
// choice. TODO(perf): this crossover and its interaction with large
// slop, where windows widen toward full coverage and erode the win
// measured: 8 is conservative, leaves win on the table on
// moderately-skewed real data; calibration pending
inline constexpr uint64_t kSeekGatherSkew = 3;

// Test seam for the gather strategy. Production always leaves this at
// kAuto; gGatherOverride is never written outside tests. Tests flip it
// to kForceSeek / kForceReadAll so the seek-gather and read-all paths
// can be driven over identical data and asserted to match - the only
// way to cover the seek-gather branch, since on balanced data the
// heuristic never selects it and read-all would mask any divergence.
enum class GatherOverride : uint8_t {
  kAuto,
  kForceSeek,
  kForceReadAll,
};

inline GatherOverride gGatherOverride = GatherOverride::kAuto;

// Applies the test seam on top of the heuristic decision. Seek-gather
// is correct for any input (it only changes which positions are read,
// never the match set), so forcing it on balanced data is safe.
inline bool ResolveSeekGather(bool heuristic) noexcept {
  switch (gGatherOverride) {
    case GatherOverride::kForceSeek:
      return true;
    case GatherOverride::kForceReadAll:
      return false;
    case GatherOverride::kAuto:
      break;
  }
  return heuristic;
}

// Merge [pos - W, pos + W] over the lead slot's already-sorted
// positions into disjoint ascending windows. lo is clamped to
// pos_limits::min() because positions are 1-based and seek(0) is a
// no-op on a freshly positioned iterator. By the coverage lemma
// (|p_a - p_b| <= slop + sum_expected for any two slots of a valid
// tuple) every position of every valid match lies in one of these
// windows, so gathering other slots within them drops no match.
inline void BuildWindows(const std::vector<PosAttr::value_t>& lead_pos,
                         PosAttr::value_t w, std::vector<Window>& out) {
  out.clear();
  for (const PosAttr::value_t p : lead_pos) {
    const PosAttr::value_t lo = (p > w) ? (p - w) : pos_limits::min();
    const PosAttr::value_t hi =
      (p > std::numeric_limits<PosAttr::value_t>::max() - w)
        ? std::numeric_limits<PosAttr::value_t>::max()
        : (p + w);
    if (!out.empty() && lo <= out.back().second) {
      // Overlaps the previous window: extend it. Forward-only seek
      // cannot re-read, so the gather windows must stay disjoint.
      if (hi > out.back().second) {
        out.back().second = hi;
      }
    } else {
      out.push_back({lo, hi});
    }
  }
}

// Counts a single lead's valid tuples and accumulates freq +
// best_distance into 'res'. This is the n >= 3 matcher: Run calls it for
// every lead. Mirrors DfsEnumerate's pruning so the scored frequency
// matches EnumerateAll's tuple count. Stops at the first tuple under
// early_exit.
inline void CountFromLead(
  const std::vector<std::vector<PosAttr::value_t>>& slots,
  PosAttr::value_t slop, const std::vector<PosAttr::value_t>& expected_steps,
  std::vector<PosAttr::value_t>& chain, size_t i, PosAttr::value_t cost_so_far,
  DpResult& res, bool early_exit, bool enforce_uniqueness = true) {
  const size_t n = slots.size();
  if (i == n) {
    ++res.freq;
    if (!res.any || cost_so_far < res.best_distance) {
      res.best_distance = cost_so_far;
    }
    res.any = true;
    return;
  }
  const PosAttr::value_t prev = chain[i - 1];
  const PosAttr::value_t budget = slop - cost_so_far;
  const PosAttr::value_t expected = expected_steps[i - 1];
  const PosAttr::value_t span = budget + 1;
  const PosAttr::value_t exp_plus = expected + span;
  const PosAttr::value_t exp_target =
    (prev > std::numeric_limits<PosAttr::value_t>::max() - expected)
      ? std::numeric_limits<PosAttr::value_t>::max()
      : prev + expected;
  const PosAttr::value_t lo = (exp_target > span) ? exp_target - span : 0;
  const PosAttr::value_t hi =
    (prev > std::numeric_limits<PosAttr::value_t>::max() - exp_plus)
      ? std::numeric_limits<PosAttr::value_t>::max()
      : prev + exp_plus;
  const auto& sp = slots[i];
  auto begin = std::lower_bound(sp.begin(), sp.end(), lo);
  auto end = std::upper_bound(sp.begin(), sp.end(), hi);
  for (auto it = begin; it != end; ++it) {
    const PosAttr::value_t p = *it;
    bool dup = false;
    if (enforce_uniqueness) {
      for (size_t k = 0; k < i; ++k) {
        if (chain[k] == p) {
          dup = true;
          break;
        }
      }
    }
    if (dup) {
      continue;
    }
    const int64_t delta = static_cast<int64_t>(p) - static_cast<int64_t>(prev);
    const PosAttr::value_t step = StepCost(delta, expected);
    if (cost_so_far + step > slop) {
      continue;
    }
    chain[i] = p;
    CountFromLead(slots, slop, expected_steps, chain, i + 1, cost_so_far + step,
                  res, early_exit, enforce_uniqueness);
    if (early_exit && res.any) {
      return;
    }
  }
}

inline DpResult Run(const std::vector<std::vector<PosAttr::value_t>>& slot_pos,
                    PosAttr::value_t slop,
                    const std::vector<PosAttr::value_t>& expected_steps,
                    DpScratch& scratch, bool early_exit,
                    const std::vector<uint32_t>& groups = {}) {
  DpResult res{};
  const size_t n = slot_pos.size();
  if (n < 2) {
    return res;
  }
  for (const auto& sp : slot_pos) {
    if (sp.empty()) {
      return res;
    }
  }
  SDB_ASSERT(expected_steps.size() == n - 1);

  // Window half-width: bounded by slop + sum_expected
  // (StepCost invariant |delta_i| <= cost_i + expected_i).
  PosAttr::value_t sum_expected = 0;
  for (auto e : expected_steps) {
    sum_expected += e;
  }
  const PosAttr::value_t W = slop + sum_expected;

  // Fast path for the common two-term phrase: pick the slot with fewer
  // positions as the lead, then for each lead position binary-search the
  // other slot's slop-reachable window. Iterating the smaller slot
  // minimises the number of searches - a large win on phrases with one
  // very common term (e.g. "the X"), especially under seek-gather, where
  // the dense slot is gathered into windows around the rare one yet would
  // otherwise still drive the lead loop. The step cost is directional
  // (delta = pos[slot 1] - pos[slot 0] in phrase order); it is formed with
  // the correct sign regardless of which slot is iterated, so freq and
  // best_distance are identical to leading from slot 0. The +-W window is
  // symmetric, so the same bound covers the other slot either way.
  if (n == 2) {
    const PosAttr::value_t expected = expected_steps[0];
    const bool lead0 = slot_pos[0].size() <= slot_pos[1].size();
    const auto& lead = lead0 ? slot_pos[0] : slot_pos[1];
    const auto& other = lead0 ? slot_pos[1] : slot_pos[0];
    // Same index position may fill both slots only for distinct terms
    // (different groups); dropped when groups are absent or the terms match.
    const bool same_pos_ok = !groups.empty() && groups[0] != groups[1];
    for (const PosAttr::value_t pl : lead) {
      const PosAttr::value_t win_lo = (pl > W) ? (pl - W) : 0;
      const PosAttr::value_t win_hi =
        (pl > std::numeric_limits<PosAttr::value_t>::max() - W)
          ? std::numeric_limits<PosAttr::value_t>::max()
          : (pl + W);
      auto begin = std::lower_bound(other.begin(), other.end(), win_lo);
      auto end = std::upper_bound(other.begin(), other.end(), win_hi);
      for (auto it = begin; it != end; ++it) {
        const PosAttr::value_t po = *it;
        if (po == pl && !same_pos_ok) {
          continue;
        }
        const int64_t delta =
          lead0 ? (static_cast<int64_t>(po) - static_cast<int64_t>(pl))
                : (static_cast<int64_t>(pl) - static_cast<int64_t>(po));
        const PosAttr::value_t step = StepCost(delta, expected);
        if (step > slop) {
          continue;
        }
        if (early_exit) {
          res.any = true;
          return res;
        }
        ++res.freq;
        if (!res.any || step < res.best_distance) {
          res.best_distance = step;
        }
        res.any = true;
      }
    }
    return res;
  }

  // Same index position may be shared by slots of DIFFERENT terms (e.g.
  // synonyms at increment 0), but one occurrence / a repeated term must not
  // fill two slots. Enforce strict per-position uniqueness only when groups
  // are absent (variadic / opt-out) or a term repeats; otherwise distinct
  // terms are free to share a position (cost via StepCost == 1, so excluded
  // at slop 0). Matches ES match_phrase over synonym-expanded positions.
  bool enforce_uniqueness = groups.empty();
  if (!enforce_uniqueness) {
    for (size_t a = 0; a < groups.size() && !enforce_uniqueness; ++a) {
      for (size_t b = a + 1; b < groups.size(); ++b) {
        if (groups[a] == groups[b]) {
          enforce_uniqueness = true;
          break;
        }
      }
    }
  }

  // n >= 3: enumerate each lead's valid tuples with the pruned DFS.
  // CountFromLead walks only the slop-reachable [lo, hi] slice of each
  // slot and prunes on the running cost, so it never materialises a
  // window; W above bounds that reachable range and serves the n == 2
  // fast path.
  auto& chain = scratch.chain;
  chain.resize(n);
  for (PosAttr::value_t p0 : slot_pos[0]) {
    chain[0] = p0;
    CountFromLead(slot_pos, slop, expected_steps, chain, 1, 0, res, early_exit,
                  enforce_uniqueness);
    if (early_exit && res.any) {
      return res;
    }
  }

  return res;
}

// A single enumerated valid tuple, recording leftmost and rightmost
// position with their originating slot indices (used to look up
// OffsAttr from per-slot materialized offset arrays).
struct EnumeratedMatch {
  PosAttr::value_t leftmost;
  PosAttr::value_t rightmost;
  uint32_t leftmost_slot;
  uint32_t rightmost_slot;
};

// DFS over all valid n-tuples, one EnumeratedMatch per tuple, sorted
// by leftmost position ascending. Pruned by the same budget and
// uniqueness checks as the DP pass.
inline void DfsEnumerate(
  const std::vector<std::vector<PosAttr::value_t>>& slots,
  PosAttr::value_t slop, const std::vector<PosAttr::value_t>& expected_steps,
  std::vector<PosAttr::value_t>& chain, size_t i, PosAttr::value_t cost_so_far,
  std::vector<EnumeratedMatch>& out) {
  const size_t n = slots.size();
  if (i == n) {
    PosAttr::value_t lp = chain[0], rp = chain[0];
    uint32_t ls = 0, rs = 0;
    for (size_t k = 1; k < n; ++k) {
      if (chain[k] < lp) {
        lp = chain[k];
        ls = static_cast<uint32_t>(k);
      }
      if (chain[k] > rp) {
        rp = chain[k];
        rs = static_cast<uint32_t>(k);
      }
    }
    out.push_back({lp, rp, ls, rs});
    return;
  }
  const PosAttr::value_t prev = chain[i - 1];
  const PosAttr::value_t budget = slop - cost_so_far;
  const PosAttr::value_t expected = expected_steps[i - 1];
  // Candidate window for p with +1 slack each side; out-of-range
  // drops via the StepCost check below.
  const PosAttr::value_t span = budget + 1;
  const PosAttr::value_t exp_plus = expected + span;
  const PosAttr::value_t exp_target =
    (prev > std::numeric_limits<PosAttr::value_t>::max() - expected)
      ? std::numeric_limits<PosAttr::value_t>::max()
      : prev + expected;
  const PosAttr::value_t lo = (exp_target > span) ? exp_target - span : 0;
  const PosAttr::value_t hi =
    (prev > std::numeric_limits<PosAttr::value_t>::max() - exp_plus)
      ? std::numeric_limits<PosAttr::value_t>::max()
      : prev + exp_plus;
  const auto& sp = slots[i];
  auto begin = std::lower_bound(sp.begin(), sp.end(), lo);
  auto end = std::upper_bound(sp.begin(), sp.end(), hi);
  for (auto it = begin; it != end; ++it) {
    const PosAttr::value_t p = *it;
    bool dup = false;
    for (size_t k = 0; k < i; ++k) {
      if (chain[k] == p) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    const int64_t delta = static_cast<int64_t>(p) - static_cast<int64_t>(prev);
    const PosAttr::value_t step = StepCost(delta, expected);
    if (cost_so_far + step > slop) {
      continue;
    }
    chain[i] = p;
    DfsEnumerate(slots, slop, expected_steps, chain, i + 1, cost_so_far + step,
                 out);
  }
}

inline std::vector<EnumeratedMatch> EnumerateAll(
  const std::vector<std::vector<PosAttr::value_t>>& slots,
  PosAttr::value_t slop, const std::vector<PosAttr::value_t>& expected_steps) {
  std::vector<EnumeratedMatch> out;
  const size_t n = slots.size();
  if (n < 2) {
    return out;
  }
  for (const auto& sp : slots) {
    if (sp.empty()) {
      return out;
    }
  }
  SDB_ASSERT(expected_steps.size() == n - 1);
  std::vector<PosAttr::value_t> chain(n);
  for (PosAttr::value_t p0 : slots[0]) {
    chain[0] = p0;
    DfsEnumerate(slots, slop, expected_steps, chain, 1, 0, out);
  }
  std::sort(out.begin(), out.end(),
            [](const EnumeratedMatch& a, const EnumeratedMatch& b) {
              return a.leftmost < b.leftmost;
            });
  return out;
}

}  // namespace detail::slop_dp

// Replaces SlopPhraseFrequency. Template parameters:
//   Offs    - collect OffsAttr and emit per-match offsets through
//             PhrasePosition iteration.
//   HasFreq - compute exact freq + best_distance. When false, the DP
//             early-exits on the first valid tuple and freq is set
//             to 1 to signal a match.
// kHasBoost = HasFreq follows the original convention.
// document.
template<bool Offs, bool HasFreq>
class SlopPhraseFrequencyDP {
 public:
  using TermPosition = FixedTermPosition<Offs>;
  using Positions = std::vector<TermPosition>;

  static constexpr bool kHasBoost = HasFreq;
  static constexpr bool kHasFreq = HasFreq;

  SlopPhraseFrequencyDP(std::vector<TermPosition>&& pos,
                        PosAttr::value_t max_slop,
                        std::vector<PosAttr::value_t>&& expected_steps) noexcept
    : _pos{std::move(pos)},
      _max_slop{max_slop},
      _expected_steps{std::move(expected_steps)} {
    SDB_ASSERT(_pos.size() >= 2);
    SDB_ASSERT(_max_slop > 0);
    SDB_ASSERT(_expected_steps.size() == _pos.size() - 1);
    _window_half = _max_slop;
    for (auto e : _expected_steps) {
      _window_half += e;
    }
  }

  IRS_FORCE_INLINE bool Match() {
    _phrase_freq = 0;
    _best_distance = _max_slop + 1;
    if constexpr (Offs) {
      _start_offset = 0;
      _end_offset = 0;
      _matches.clear();
      _match_idx = 0;
    }

    const size_t n = _pos.size();
    // Reuse inner vector capacity across calls; n is fixed for the iterator.
    _slot_pos.resize(n);

    if (_term_groups.size() != n) {
      _term_groups.clear();
      _term_groups.reserve(n);
      for (const auto& p : _pos) {
        _term_groups.push_back(p.second.term_group);
      }
    }

    for (auto& s : _slot_pos) {
      s.clear();
    }
    if constexpr (Offs) {
      _slot_offs.resize(n);
      for (auto& s : _slot_offs) {
        s.clear();
      }
    }
    // Per-slot gather. gather_all reads the whole posting; gather_windows
    // reads only positions inside the precomputed slop-reachable windows
    // via forward seek. Both fill _slot_pos[i] (+ _slot_offs[i] when Offs).
    const auto gather_all = [&](size_t i) {
      auto& it = *_pos[i].first;
      auto& positions = _slot_pos[i];
      while (it.next()) {
        const auto v = it.value();
        if (pos_limits::eof(v)) {
          break;
        }
        positions.push_back(v);
        if constexpr (Offs) {
          if (auto* o = irs::get<OffsAttr>(it); o) {
            _slot_offs[i].push_back({o->start, o->end});
          } else {
            _slot_offs[i].push_back({0, 0});
          }
        }
      }
    };

    const auto gather_windows = [&](size_t i) {
      auto& it = *_pos[i].first;
      auto& positions = _slot_pos[i];
      for (const auto& [lo, hi] : _windows) {
        auto v = it.seek(lo);
        while (pos_limits::valid(v) && !pos_limits::eof(v) && v <= hi) {
          positions.push_back(v);
          if constexpr (Offs) {
            if (auto* o = irs::get<OffsAttr>(it); o) {
              _slot_offs[i].push_back({o->start, o->end});
            } else {
              _slot_offs[i].push_back({0, 0});
            }
          }
          if (!it.next()) {
            break;
          }
          v = it.value();
        }
      }
    };

    // Pick the rarest slot by exact per-doc position count (DocFreq is
    // already set by the doc-level advance and reads nothing). When the
    // frequencies are skewed enough, lead the gather from the rarest
    // slot and seek the rest only within slop-reachable windows;
    // otherwise keep the plain read-all path.
    size_t rare = 0;
    uint32_t min_df = std::numeric_limits<uint32_t>::max();
    uint32_t max_df = 0;
    for (size_t i = 0; i < n; ++i) {
      const uint32_t df = _pos[i].first->DocFreq();
      if (df < min_df) {
        min_df = df;
        rare = i;
      }
      if (df > max_df) {
        max_df = df;
      }
    }
    const bool seek_gather = detail::slop_dp::ResolveSeekGather(
      min_df != 0 &&
      static_cast<uint64_t>(min_df) * detail::slop_dp::kSeekGatherSkew <=
        max_df);

    if (seek_gather) {
      gather_all(rare);
      if (_slot_pos[rare].empty()) {
        return false;
      }
      detail::slop_dp::BuildWindows(_slot_pos[rare], _window_half, _windows);
      for (size_t i = 0; i < n; ++i) {
        if (i == rare) {
          continue;
        }
        gather_windows(i);
        if (_slot_pos[i].empty()) {
          return false;
        }
      }
    } else {
      for (size_t i = 0; i < n; ++i) {
        gather_all(i);
        if (_slot_pos[i].empty()) {
          return false;
        }
      }
    }

    auto res =
      detail::slop_dp::Run(_slot_pos, _max_slop, _expected_steps, _dp_scratch,
                           /*early_exit=*/!HasFreq, _term_groups);
    if (!res.any) {
      return false;
    }

    if constexpr (HasFreq) {
      _phrase_freq = static_cast<uint32_t>(res.freq);
      _best_distance = res.best_distance;
      if constexpr (Offs) {
        BuildMatches();
      }
    } else {
      _phrase_freq = 1;
    }
    return true;
  }

  uint32_t GetFreq() const noexcept { return _phrase_freq; }

  score_t GetBoost() const noexcept {
    if (_best_distance == 0) {
      return kNoBoost;
    }
    return 1.f / (1.f + static_cast<score_t>(_best_distance));
  }

 private:
  friend class PhrasePosition<SlopPhraseFrequencyDP>;

  struct OffsetPair {
    uint32_t start;
    uint32_t end;
  };

  std::pair<const uint32_t*, const uint32_t*> GetOffsets() const noexcept {
    return {&_start_offset, &_end_offset};
  }

  // Emits matches in successive NextPosition() calls. Pre-loads match
  // #0 in BuildMatches(), so call N reads match #N-1 and pre-loads #N.
  // Returns 1 while matches remain (PhrasePosition::_left stays 1),
  // 0 after the last (PhrasePosition::_left drops to 0, next() returns
  // false on the following call).
  uint32_t NextPosition() {
    if constexpr (!Offs || !HasFreq) {
      // Filter-only or no-offsets path: single emission, prior behavior.
      return 0;
    } else {
      if (_match_idx >= _matches.size()) {
        return 0;
      }
      _start_offset = _matches[_match_idx].start;
      _end_offset = _matches[_match_idx].end;
      ++_match_idx;
      return 1;
    }
  }

  // Enumerate all valid tuples, resolve their leftmost/rightmost
  // OffsAttr, and pre-load match #0 into _start_offset/_end_offset.
  void BuildMatches() {
    if constexpr (!Offs || !HasFreq) {
      return;
    }
    auto enumerated =
      detail::slop_dp::EnumerateAll(_slot_pos, _max_slop, _expected_steps);
    // DP-reported freq must match enumeration count exactly.
    SDB_ASSERT(static_cast<uint32_t>(enumerated.size()) == _phrase_freq);
    _matches.clear();
    _matches.reserve(enumerated.size());
    auto find_index = [&](size_t slot, PosAttr::value_t p) -> size_t {
      const auto& sp = _slot_pos[slot];
      auto it = std::lower_bound(sp.begin(), sp.end(), p);
      SDB_ASSERT(it != sp.end() && *it == p);
      return static_cast<size_t>(it - sp.begin());
    };
    for (const auto& m : enumerated) {
      const size_t li = find_index(m.leftmost_slot, m.leftmost);
      const size_t ri = find_index(m.rightmost_slot, m.rightmost);
      _matches.push_back({_slot_offs[m.leftmost_slot][li].start,
                          _slot_offs[m.rightmost_slot][ri].end});
    }
    if (!_matches.empty()) {
      _start_offset = _matches[0].start;
      _end_offset = _matches[0].end;
      _match_idx = 1;
    }
  }

  Positions _pos;
  PosAttr::value_t _max_slop;
  std::vector<PosAttr::value_t> _expected_steps;
  // Window half-width slop + sum(expected_steps); cached for gather.
  PosAttr::value_t _window_half = 0;
  std::vector<std::vector<PosAttr::value_t>> _slot_pos;
  std::vector<std::vector<OffsetPair>> _slot_offs;
  // Merged slop-reachable windows around the rarest slot's positions
  // (filled only on the seek-gather path). Reused across Match() calls.
  std::vector<detail::slop_dp::Window> _windows;
  // Reused across Match() calls to keep the DP off the allocator on
  // the hot path.
  detail::slop_dp::DpScratch _dp_scratch;
  // All emitted matches (filled only when Offs && HasFreq). Sorted
  // by leftmost position. Each entry holds the start offset of the
  // leftmost token and the end offset of the rightmost token of the
  // corresponding valid tuple.
  std::vector<OffsetPair> _matches;
  size_t _match_idx = 0;
  uint32_t _phrase_freq = 0;
  PosAttr::value_t _best_distance = 0;
  uint32_t _start_offset{0};
  uint32_t _end_offset{0};

  std::vector<uint32_t> _term_groups;
};

// Variadic sloppy phrase frequency. Replaces SlopVariadicPhraseFrequency.
template<typename Adapter, bool HasFreq>
class SlopVariadicPhraseFrequencyDP {
 public:
  using TermPosition = VariadicTermPosition<Adapter>;
  using Positions = std::vector<TermPosition>;

  static constexpr bool kHasBoost = HasFreq;
  static constexpr bool kHasFreq = HasFreq;

  SlopVariadicPhraseFrequencyDP(
    std::vector<TermPosition>&& pos, PosAttr::value_t max_slop,
    std::vector<PosAttr::value_t>&& expected_steps) noexcept
    : _pos{std::move(pos)},
      _max_slop{max_slop},
      _expected_steps{std::move(expected_steps)} {
    SDB_ASSERT(_pos.size() >= 2);
    SDB_ASSERT(_max_slop > 0);
    SDB_ASSERT(_expected_steps.size() == _pos.size() - 1);
    _window_half = _max_slop;
    for (auto e : _expected_steps) {
      _window_half += e;
    }
  }

  IRS_FORCE_INLINE bool Match() {
    _phrase_freq = 0;
    _best_distance = _max_slop + 1;
    if constexpr (kHasOffsets) {
      _start_offset = 0;
      _end_offset = 0;
      _matches.clear();
      _match_idx = 0;
    }

    const size_t n = _pos.size();
    // Reuse inner vectors' capacity (see SlopPhraseFrequencyDP::Match)
    _slot_pos.resize(n);
    for (auto& s : _slot_pos) {
      s.clear();
    }
    if constexpr (kHasOffsets) {
      _slot_offs.resize(n);
      for (auto& s : _slot_offs) {
        s.clear();
      }
    }

    // finalize_slot sorts + dedups the per-sub-iterator scratch and
    // moves it into _slot_pos[i] (+ _slot_offs[i] when offsets). Returns
    // false on an empty slot (no possible match).
    const auto finalize_slot = [&](size_t i) -> bool {
      std::sort(_scratch_entries.begin(), _scratch_entries.end());
      auto last = std::unique(_scratch_entries.begin(), _scratch_entries.end());
      _scratch_entries.erase(last, _scratch_entries.end());
      auto& positions = _slot_pos[i];
      positions.reserve(_scratch_entries.size());
      for (const auto& e : _scratch_entries) {
        positions.push_back(e.pos);
      }
      if constexpr (kHasOffsets) {
        auto& offs = _slot_offs[i];
        offs.reserve(_scratch_entries.size());
        for (const auto& e : _scratch_entries) {
          offs.push_back({e.start_offs, e.end_offs});
        }
      }
      return !positions.empty();
    };

    const auto gather_all = [&](size_t i) -> bool {
      _scratch_entries.clear();
      CollectCtx ctx{&_scratch_entries};
      _pos[i].first->visit(&ctx, CollectOneSubIter);
      return finalize_slot(i);
    };

    const auto gather_windows = [&](size_t i) -> bool {
      _scratch_entries.clear();
      WindowCtx ctx{&_scratch_entries, &_windows};
      _pos[i].first->visit(&ctx, CollectWindowedSubIter);
      return finalize_slot(i);
    };

    // Variadic has no exact per-doc position count on the compound
    // iterator, so the rarest slot is picked by the disjunction's
    // doc-level cost estimate. This only steers which slot leads the
    // gather (perf), never correctness. TODO(perf): a per-doc count via
    // a DocFreq-summing visit pass would gate more precisely; deferred
    // to the measurement phase.
    size_t rare = 0;
    uint64_t min_cost = std::numeric_limits<uint64_t>::max();
    uint64_t max_cost = 0;
    for (size_t i = 0; i < n; ++i) {
      const uint64_t c = CostAttr::extract(*_pos[i].first);
      if (c < min_cost) {
        min_cost = c;
        rare = i;
      }
      if (c > max_cost) {
        max_cost = c;
      }
    }
    const bool seek_gather = detail::slop_dp::ResolveSeekGather(
      min_cost != 0 && min_cost * detail::slop_dp::kSeekGatherSkew <= max_cost);

    if (seek_gather) {
      if (!gather_all(rare)) {
        return false;
      }
      detail::slop_dp::BuildWindows(_slot_pos[rare], _window_half, _windows);
      for (size_t i = 0; i < n; ++i) {
        if (i == rare) {
          continue;
        }
        if (!gather_windows(i)) {
          return false;
        }
      }
    } else {
      for (size_t i = 0; i < n; ++i) {
        if (!gather_all(i)) {
          return false;
        }
      }
    }

    auto res = detail::slop_dp::Run(_slot_pos, _max_slop, _expected_steps,
                                    _dp_scratch, /*early_exit=*/!HasFreq);
    if (!res.any) {
      return false;
    }

    if constexpr (HasFreq) {
      _phrase_freq = static_cast<uint32_t>(res.freq);
      _best_distance = res.best_distance;
      if constexpr (kHasOffsets) {
        BuildMatches();
      }
    } else {
      _phrase_freq = 1;
    }
    return true;
  }

  uint32_t GetFreq() const noexcept { return _phrase_freq; }

  score_t GetBoost() const noexcept {
    if (_best_distance == 0) {
      return kNoBoost;
    }
    return 1.f / (1.f + static_cast<score_t>(_best_distance));
  }

 private:
  friend class PhrasePosition<SlopVariadicPhraseFrequencyDP>;

  static constexpr bool kHasOffsets =
    std::is_same_v<Adapter, VariadicPhraseOffsetAdapter>;

  struct OffsetPair {
    uint32_t start;
    uint32_t end;
  };

  struct PosEntry {
    PosAttr::value_t pos;
    uint32_t start_offs{0};
    uint32_t end_offs{0};
    bool operator<(const PosEntry& rhs) const noexcept { return pos < rhs.pos; }
    bool operator==(const PosEntry& rhs) const noexcept {
      return pos == rhs.pos;
    }
  };

  struct CollectCtx {
    std::vector<PosEntry>* out;
  };

  static bool CollectOneSubIter(void* ctx, Adapter& adapter) {
    SDB_ASSERT(ctx);
    auto& c = *reinterpret_cast<CollectCtx*>(ctx);
    auto* p = adapter.position;
    if (!p) {
      return true;
    }
    const OffsAttr* offs = nullptr;
    if constexpr (kHasOffsets) {
      offs = adapter.offset;
    }
    p->reset();
    while (p->next()) {
      const auto v = p->value();
      if (pos_limits::eof(v)) {
        break;
      }
      PosEntry e{.pos = v};
      if constexpr (kHasOffsets) {
        if (offs) {
          e.start_offs = offs->start;
          e.end_offs = offs->end;
        }
      }
      c.out->push_back(e);
    }
    return true;
  }

  struct WindowCtx {
    std::vector<PosEntry>* out;
    const std::vector<detail::slop_dp::Window>* windows;
  };

  // Seek-gather variant: for each sub-iterator, read only positions
  // inside the precomputed disjoint windows via forward seek. reset()
  // rewinds to the doc start (forward-only seek must begin before the
  // first window); windows are ascending and disjoint, so the seeks
  // never go backwards.
  static bool CollectWindowedSubIter(void* ctx, Adapter& adapter) {
    SDB_ASSERT(ctx);
    auto& c = *reinterpret_cast<WindowCtx*>(ctx);
    auto* p = adapter.position;
    if (!p) {
      return true;
    }
    const OffsAttr* offs = nullptr;
    if constexpr (kHasOffsets) {
      offs = adapter.offset;
    }
    p->reset();
    for (const auto& [lo, hi] : *c.windows) {
      auto v = p->seek(lo);
      while (pos_limits::valid(v) && !pos_limits::eof(v) && v <= hi) {
        PosEntry e{.pos = v};
        if constexpr (kHasOffsets) {
          if (offs) {
            e.start_offs = offs->start;
            e.end_offs = offs->end;
          }
        }
        c.out->push_back(e);
        if (!p->next()) {
          v = pos_limits::eof();
          break;
        }
        v = p->value();
      }
    }
    return true;
  }

  std::pair<const uint32_t*, const uint32_t*> GetOffsets() const noexcept {
    return {&_start_offset, &_end_offset};
  }

  // See SlopPhraseFrequencyDP::NextPosition() for state machine
  uint32_t NextPosition() {
    if constexpr (!kHasOffsets || !HasFreq) {
      return 0;
    } else {
      if (_match_idx >= _matches.size()) {
        return 0;
      }
      _start_offset = _matches[_match_idx].start;
      _end_offset = _matches[_match_idx].end;
      ++_match_idx;
      return 1;
    }
  }

  // See SlopPhraseFrequencyDP::BuildMatches
  void BuildMatches() {
    if constexpr (!kHasOffsets || !HasFreq) {
      return;
    }
    auto enumerated =
      detail::slop_dp::EnumerateAll(_slot_pos, _max_slop, _expected_steps);
    SDB_ASSERT(static_cast<uint32_t>(enumerated.size()) == _phrase_freq);
    _matches.clear();
    _matches.reserve(enumerated.size());
    auto find_index = [&](size_t slot, PosAttr::value_t p) -> size_t {
      const auto& sp = _slot_pos[slot];
      auto it = std::lower_bound(sp.begin(), sp.end(), p);
      SDB_ASSERT(it != sp.end() && *it == p);
      return static_cast<size_t>(it - sp.begin());
    };
    for (const auto& m : enumerated) {
      const size_t li = find_index(m.leftmost_slot, m.leftmost);
      const size_t ri = find_index(m.rightmost_slot, m.rightmost);
      _matches.push_back({_slot_offs[m.leftmost_slot][li].start,
                          _slot_offs[m.rightmost_slot][ri].end});
    }
    if (!_matches.empty()) {
      _start_offset = _matches[0].start;
      _end_offset = _matches[0].end;
      _match_idx = 1;
    }
  }

  Positions _pos;
  PosAttr::value_t _max_slop;
  std::vector<PosAttr::value_t> _expected_steps;
  // Window half-width slop + sum(expected_steps); cached for gather.
  PosAttr::value_t _window_half = 0;
  std::vector<std::vector<PosAttr::value_t>> _slot_pos;
  std::vector<std::vector<OffsetPair>> _slot_offs;
  std::vector<PosEntry> _scratch_entries;
  // Merged slop-reachable windows around the rarest slot's positions
  // (filled only on the seek-gather path). Reused across Match() calls.
  std::vector<detail::slop_dp::Window> _windows;
  // Reused across Match() calls to keep the DP off the allocator on
  // the hot path.
  detail::slop_dp::DpScratch _dp_scratch;
  std::vector<OffsetPair> _matches;
  size_t _match_idx = 0;
  uint32_t _phrase_freq = 0;
  PosAttr::value_t _best_distance = 0;
  uint32_t _start_offset{0};
  uint32_t _end_offset{0};
};

}  // namespace irs
