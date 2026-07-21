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
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

#include "basics/empty.hpp"
#include "disjunction.hpp"
#include "iresearch/analysis/token_attributes.hpp"
#include "iresearch/formats/posting/iterator_pos.hpp"
#include "iresearch/index/iterators.hpp"
#include "iresearch/search/score_function.hpp"

// Sloppy phrase frequency over per-slot position lists.
//
// Matching: Run anchors at the rarest slot and, for each of its positions,
// runs a pruned DFS (CountFromAnchor) over the remaining slots, rightward
// then leftward, accumulating freq + best_distance. StepCost is directional,
// so every transition is scored in phrase order against the slot's adjacent
// partner. The DFS walks only the slop-reachable [lo, hi] slice of each slot
// and prunes on running cost, so its work tracks the valid (and near-valid)
// tuples; it never materializes a per-anchor window. Given a collector, the
// same pass emits one EnumeratedMatch per valid tuple, so freq and the
// enumerated count come from one walk and agree by construction.
//
// Gather: every slot is read in full via the bulk ReadAll decode. A
// windowed seek-gather alternative (read the rarest slot, seek the rest
// into slop-reachable windows) was built and then removed: intra-doc
// position seek is a sequential decode in this format (iterator_pos.hpp
// seek() walks every skipped position and its attributes), so windowing
// saves no decode work and only adds per-window overhead - measured
// 1.2-11x slower than read-all across skew ratios 1-60, slop 1-50, on
// shallow and multi-block postings alike. See the removal note in the PR.
//
// expected_step == 1 is the plain adjacent-term cost model (ES-verified);
// expected_step > 1 adds per-slot positional gaps from push_back(term, offs).
// Interval gaps (offs_min != offs_max) with slop are rejected upstream by
// SDB_ENSURE.

// Profiling aid: with -DSLOP_PROFILE, force the matcher (Run) and the
// pair join (JoinPair) out of line so a profiler can split matching vs
// position decode (iterator next/seek, already out of line). Each runs
// once per candidate doc, so the perturbation is negligible. Never
// defined in production.
#ifdef SLOP_PROFILE
#define SLOP_PROF_NOINLINE [[gnu::noinline]]
#else
#define SLOP_PROF_NOINLINE
#endif

namespace irs {

template<typename Frequency>
class PhrasePosition;
template<typename T>
struct HasPosition;

namespace detail::slop {

// Step cost for one slot transition, over delta = pos[slot] - pos[partner]
// in phrase order. For a regular adjacent term (expected == 1): adjacency
// (delta == 1) is free, a forward gap costs delta - 1, same position costs
// 1, and a reversal (delta <= -1) costs -delta + 1. For a requested gap
// (expected > 1): hitting it costs 0, too far costs delta - expected, too
// close costs expected - delta, and a reversal costs expected - delta + 1.
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

struct MatchResult {
  uint64_t freq = 0;
  PosAttr::value_t best_distance = 0;
  bool any = false;
};

// Scratch passed from the Frequency object into Run: the DFS chain and slot
// visitation order, whose capacity is reused across anchor positions and
// documents instead of reallocated per call.
struct MatchScratch {
  std::vector<PosAttr::value_t> chain;
  std::vector<uint32_t> order;
};

// Test seam for the n == 2 fused merge-join (JoinPair), fixed and variadic.
// Production leaves this false; tests set it to route n == 2 through the
// generic gather + Run path and assert join-vs-generic equivalence over
// identical data (the join bypasses gather, so this is the only way to run
// both).
inline bool gPairJoinDisabled = false;

// Bench seam for the offset-enabled read-all gather. Production leaves this
// false; the micro benchmark sets it (via a command-line flag) to route the
// Offs gather through the scalar per-position loop instead of the bulk
// ReadAll overload, giving an in-binary A/B of the two decode paths.
inline bool gOffsBulkGatherDisabled = false;

// (start, end) offsets of one token occurrence.
struct PosOffset {
  uint32_t start;
  uint32_t end;
};

// A single valid pair emitted by JoinPair: positions with slot ids
// (mirrors EnumeratedMatch) plus the already-resolved match offsets
// (leftmost token's start, rightmost token's end).
struct PairMatch {
  PosAttr::value_t leftmost;
  PosAttr::value_t rightmost;
  uint32_t leftmost_slot;
  uint32_t rightmost_slot;
  uint32_t start_offset;
  uint32_t end_offset;
};

// Sliding partner-position buffer for JoinPair, reused across Match()
// calls. buf_offs is parallel to buf_pos and filled only when the join
// is instantiated with Offs.
struct PairScratch {
  std::vector<PosAttr::value_t> buf_pos;
  std::vector<PosOffset> buf_offs;
};

class UninitU32Buf {
 public:
  size_t Size() const noexcept { return _size; }

  void Clear() noexcept { _size = 0; }

  void ResizeUninit(size_t n) {
    if (n > _cap) {
      _data = std::make_unique_for_overwrite<uint32_t[]>(n);
      _cap = n;
    }
    _size = n;
  }

  void PushBack(uint32_t v) {
    if (_size == _cap) [[unlikely]] {
      Grow();
    }
    _data[_size++] = v;
  }

  uint32_t* Data() noexcept { return _data.get(); }
  const uint32_t* Data() const noexcept { return _data.get(); }

  uint32_t operator[](size_t i) const noexcept {
    SDB_ASSERT(i < _size);
    return _data[i];
  }

 private:
  void Grow() {
    const size_t cap = _cap == 0 ? 16 : _cap * 2;
    auto data = std::make_unique_for_overwrite<uint32_t[]>(cap);
    if (_size != 0) {
      std::memcpy(data.get(), _data.get(), _size * sizeof(uint32_t));
    }
    _data = std::move(data);
    _cap = cap;
  }

  std::unique_ptr<uint32_t[]> _data;
  size_t _size = 0;
  size_t _cap = 0;
};

// n == 2 fused merge-join over two forward-only position iterators, replacing
// gather + Run for two-slot phrases. Anchor positions are read
// straight off their iterator; partner positions are decoded exactly once
// into a sliding buffer bounded by the anchor window [pa - w, pa + w],
// w = slop + expected (the coverage window). Candidates are then
// exact-checked with StepCost, so the accept set equals Run's. Both bounds
// are nondecreasing in pa, which is what makes the single forward pass and
// the front-trimmed buffer sound.
//
// anchor_is_slot0 picks the phrase-order delta sign; anchor should be the
// rarer slot but either is correct. enforce_uniqueness mirrors Run: a pair
// may not share one index position. With 'out' non-null (Offs && HasFreq) one
// PairMatch per valid pair is appended, sorted by (leftmost, rightmost,
// leftmost_slot, rightmost_slot) as Run's collector does. When !HasFreq the
// join returns at the first valid pair.
template<bool Offs, bool HasFreq, typename AnchorIt, typename PartnerIt>
SLOP_PROF_NOINLINE MatchResult JoinPair(
  AnchorIt& anchor, PartnerIt& partner, const OffsAttr* anchor_offs,
  const OffsAttr* partner_offs, bool anchor_is_slot0, PosAttr::value_t slop,
  PosAttr::value_t expected, bool enforce_uniqueness, PairScratch& scratch,
  std::vector<PairMatch>* out) {
  if constexpr (!HasFreq) {
    // A collector implies a full count; early exit would truncate it.
    SDB_ASSERT(out == nullptr);
  }
  MatchResult res{};
  if (out) {
    out->clear();
  }

  constexpr PosAttr::value_t kMax =
    std::numeric_limits<PosAttr::value_t>::max();
  const PosAttr::value_t w = (expected > kMax - slop)
                               ? kMax
                               : static_cast<PosAttr::value_t>(slop + expected);

  auto& buf = scratch.buf_pos;
  buf.clear();
  if constexpr (Offs) {
    scratch.buf_offs.clear();
  }
  size_t head = 0;

  bool partner_eof = false;
  bool partner_primed = false;
  PosAttr::value_t pv = 0;

  bool anchor_live = anchor.next();
  while (anchor_live) {
    const PosAttr::value_t pa = anchor.value();
    if (pos_limits::eof(pa)) {
      break;
    }
    const PosAttr::value_t lo = (pa > w) ? (pa - w) : pos_limits::min();
    const PosAttr::value_t hi = (pa > kMax - w) ? kMax : (pa + w);

    // Drop buffered partner positions below lo; lo is nondecreasing, so
    // they can never re-enter a later window.
    while (head < buf.size() && buf[head] < lo) {
      ++head;
    }
    if (head == buf.size()) {
      if (head != 0) {
        buf.clear();
        if constexpr (Offs) {
          scratch.buf_offs.clear();
        }
        head = 0;
      }
    } else if (head > 32 && head * 2 >= buf.size()) {
      buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(head));
      if constexpr (Offs) {
        scratch.buf_offs.erase(
          scratch.buf_offs.begin(),
          scratch.buf_offs.begin() + static_cast<ptrdiff_t>(head));
      }
      head = 0;
    }

    // Extend decode up to hi; hi is nondecreasing, so every partner
    // position is decoded exactly once. Offsets must be captured while
    // the iterator still sits on the position.
    if (!partner_eof) {
      if (!partner_primed || pv < lo) {
        pv = partner.seek(lo);
        partner_primed = true;
        if (!pos_limits::valid(pv) || pos_limits::eof(pv)) {
          partner_eof = true;
        }
      }
      while (!partner_eof && pv <= hi) {
        buf.push_back(pv);
        if constexpr (Offs) {
          if (partner_offs) {
            scratch.buf_offs.push_back(
              {partner_offs->start, partner_offs->end});
          } else {
            scratch.buf_offs.push_back({0, 0});
          }
        }
        if (!partner.next()) {
          partner_eof = true;
          break;
        }
        pv = partner.value();
        if (pos_limits::eof(pv)) {
          partner_eof = true;
        }
      }
    }

    if (head == buf.size()) {
      if (partner_eof) {
        // Nothing buffered and no partner positions left: no anchor
        // position can match anymore.
        break;
      }
      // Nothing buffered and the next partner position sits beyond this
      // window (pv > hi). No anchor below pv - w can reach it, so gallop
      // the anchor forward instead of stepping; the seeked position is
      // reprocessed by the loop. lo and hi stay nondecreasing.
      const PosAttr::value_t target = (pv > w) ? (pv - w) : pos_limits::min();
      if (target > pa) {
        const PosAttr::value_t av = anchor.seek(target);
        anchor_live = pos_limits::valid(av) && !pos_limits::eof(av);
      } else {
        anchor_live = anchor.next();
      }
      continue;
    }

    PosOffset aoffs{0, 0};
    if constexpr (Offs) {
      if (anchor_offs) {
        aoffs = {anchor_offs->start, anchor_offs->end};
      }
    }

    // All buffered positions are in [lo, hi] here (pushed under a hi
    // that has only grown, trimmed by the current lo); the exact
    // StepCost check below is the real filter.
    for (size_t i = head; i < buf.size(); ++i) {
      const PosAttr::value_t v = buf[i];
      if (enforce_uniqueness && v == pa) {
        continue;
      }
      const int64_t delta =
        anchor_is_slot0 ? static_cast<int64_t>(v) - static_cast<int64_t>(pa)
                        : static_cast<int64_t>(pa) - static_cast<int64_t>(v);
      const PosAttr::value_t step = StepCost(delta, expected);
      if (step > slop) {
        continue;
      }
      ++res.freq;
      if (!res.any || step < res.best_distance) {
        res.best_distance = step;
      }
      res.any = true;
      if constexpr (!HasFreq) {
        return res;
      }
      if (out) {
        const PosAttr::value_t p0 = anchor_is_slot0 ? pa : v;
        const PosAttr::value_t p1 = anchor_is_slot0 ? v : pa;
        // Tie-breaking mirrors Run's collector: slot 0 wins both ends
        // when positions coincide.
        const uint32_t ls = (p1 < p0) ? 1u : 0u;
        const uint32_t rs = (p1 > p0) ? 1u : 0u;
        PosOffset o0;
        PosOffset o1;
        if constexpr (Offs) {
          o0 = anchor_is_slot0 ? aoffs : scratch.buf_offs[i];
          o1 = anchor_is_slot0 ? scratch.buf_offs[i] : aoffs;
        } else {
          o0 = {0, 0};
          o1 = {0, 0};
        }
        out->push_back({p0 < p1 ? p0 : p1, p0 < p1 ? p1 : p0, ls, rs,
                        (ls == 0 ? o0 : o1).start, (rs == 0 ? o0 : o1).end});
      }
    }
    anchor_live = anchor.next();
  }

  if (out) {
    std::sort(out->begin(), out->end(),
              [](const PairMatch& a, const PairMatch& b) noexcept {
                return std::tie(a.leftmost, a.rightmost, a.leftmost_slot,
                                a.rightmost_slot) <
                       std::tie(b.leftmost, b.rightmost, b.leftmost_slot,
                                b.rightmost_slot);
              });
  }
  return res;
}

// Merged position stream over one variadic slot's sub-iterators: a k-way
// merge of the per-term position lists with duplicate positions collapsed,
// exposing the next()/value()/seek() contract JoinPair expects from a single
// slot. Duplicates must collapse because the gather path does the same
// (finalize_slot's sort + unique keys on position alone): two terms of the
// set at one position yield one slot position, hence one counted pair. Which
// duplicate's offsets survive is unspecified in the gather path (unstable
// sort); the stream deterministically keeps the first sub-iterator in visit
// order.
//
// Offsets are copied into stream-owned storage on every reposition: the
// active sub-iterator changes as the merge advances, so no single sub's
// OffsAttr pointer stays correct. JoinPair reads offsets immediately after
// positioning, which the refresh-on-move exactly covers.
//
// Linear scans over the subs: a slot's term set is a handful of synonyms,
// where a heap costs more than it saves.
//
// Templated on the sub-iterator type so the fuzz oracle can drive it over
// mocks; production always uses the PosAttr default.
template<bool Offs, typename SubPos = PosAttr>
class MergedPosStream {
 public:
  void Clear() {
    _subs.clear();
    _cur = pos_limits::invalid();
  }

  // Registers one sub-iterator of the current document, rewound and primed
  // at its first position. Exhausted (empty) subs are dropped up front.
  void Add(SubPos* pos, const OffsAttr* offs) {
    SDB_ASSERT(pos);
    pos->reset();
    Sub s{.pos = pos, .offs = offs, .val = pos_limits::eof()};
    if (pos->next()) {
      const auto v = pos->value();
      if (!pos_limits::eof(v)) {
        s.val = v;
      }
    }
    if (!pos_limits::eof(s.val)) {
      _subs.push_back(s);
    }
  }

  bool Empty() const noexcept { return _subs.empty(); }

  PosAttr::value_t value() const noexcept { return _cur; }

  bool next() {
    if (pos_limits::valid(_cur)) {
      if (pos_limits::eof(_cur)) {
        return false;
      }
      // Consume every sub entry sitting on the current position; this is
      // where duplicates collapse into the one merged position already
      // emitted. The inner loop also skips repeats within one sub - real
      // postings are strictly increasing, but one compare buys not having
      // to assume it.
      for (auto& s : _subs) {
        while (s.val == _cur) {
          AdvanceSub(s);
        }
      }
    }
    return Reposition();
  }

  PosAttr::value_t seek(PosAttr::value_t target) {
    if (pos_limits::valid(_cur) && _cur >= target) {
      return _cur;  // forward-only, like the underlying iterators
    }
    for (auto& s : _subs) {
      if (s.val < target) {
        const auto v = s.pos->seek(target);
        s.val =
          (!pos_limits::valid(v) || pos_limits::eof(v)) ? pos_limits::eof() : v;
      }
    }
    Reposition();
    return _cur;
  }

  // Stable pointer to the current position's offsets, refreshed on every
  // reposition; {0, 0} when the winning sub carries no offsets.
  const OffsAttr* GetOffs() const noexcept { return &_offs; }

 private:
  struct Sub {
    SubPos* pos;
    // Null when the sub-iterator carries no offsets.
    const OffsAttr* offs;
    // Cached pos->value(); pos_limits::eof() once exhausted.
    PosAttr::value_t val;
  };

  static void AdvanceSub(Sub& s) {
    if (!s.pos->next()) {
      s.val = pos_limits::eof();
      return;
    }
    const auto v = s.pos->value();
    s.val = pos_limits::eof(v) ? pos_limits::eof() : v;
  }

  // Emits the minimum over the subs' cached positions (exhausted subs sit
  // at eof and never win) and captures the winner's offsets.
  bool Reposition() {
    auto min = pos_limits::eof();
    for (const auto& s : _subs) {
      min = std::min(min, s.val);
    }
    _cur = min;
    if (pos_limits::eof(_cur)) {
      return false;
    }
    if constexpr (Offs) {
      for (const auto& s : _subs) {
        if (s.val == _cur) {
          if (s.offs) {
            _offs.start = s.offs->start;
            _offs.end = s.offs->end;
          } else {
            _offs.start = 0;
            _offs.end = 0;
          }
          break;
        }
      }
    }
    return true;
  }

  std::vector<Sub> _subs;
  PosAttr::value_t _cur = pos_limits::invalid();
  OffsAttr _offs;
};

// A single enumerated valid tuple, recording leftmost and rightmost
// position with their originating slot indices (used to look up
// OffsAttr from per-slot materialized offset arrays).
struct EnumeratedMatch {
  PosAttr::value_t leftmost;
  PosAttr::value_t rightmost;
  uint32_t leftmost_slot;
  uint32_t rightmost_slot;
};

// Gate for the duplicate-position check: true when groups are absent
// (variadic / opt-out; the check is then global) or when some group
// holds two or more slots (the check is then scoped per group, see
// CountFromAnchor). ES-verified rule: uniqueness applies only between
// slots of one group (a connectivity component of query term sets);
// slots of different groups may share a position, the delta-0 step
// costing 1 via StepCost, so excluded at slop 0.
inline bool EnforceUniqueness(const std::vector<uint32_t>& groups) noexcept {
  if (groups.empty()) {
    return true;
  }
  for (size_t a = 0; a < groups.size(); ++a) {
    for (size_t b = a + 1; b < groups.size(); ++b) {
      if (groups[a] == groups[b]) {
        return true;
      }
    }
  }
  return false;
}

// Counts one anchor position's valid tuples into 'res' (freq +
// best_distance); Run calls it per anchor position. With 'out' non-null the
// completed chain is also emitted as one EnumeratedMatch per tuple, so
// res.freq == out->size() by construction. early_exit stops at the first
// tuple (never combined with 'out').
// With enforce_uniqueness a candidate position is rejected when an
// already-placed slot of the SAME group sits on it; empty groups widen
// the check to all placed slots (strict global).
inline void CountFromAnchor(
  const std::vector<std::vector<PosAttr::value_t>>& slots,
  PosAttr::value_t slop, const std::vector<PosAttr::value_t>& expected_steps,
  std::vector<PosAttr::value_t>& chain, const std::vector<uint32_t>& order,
  uint32_t anchor, size_t d, PosAttr::value_t cost_so_far, MatchResult& res,
  bool early_exit, const std::vector<uint32_t>& groups, bool enforce_uniqueness,
  std::vector<EnumeratedMatch>* out) {
  const size_t n = slots.size();
  if (d == n) {
    ++res.freq;
    if (!res.any || cost_so_far < res.best_distance) {
      res.best_distance = cost_so_far;
    }
    res.any = true;
    if (out) {
      PosAttr::value_t lp = chain[0];
      PosAttr::value_t rp = chain[0];
      uint32_t ls = 0;
      uint32_t rs = 0;
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
      out->push_back({lp, rp, ls, rs});
    }
    return;
  }

  constexpr PosAttr::value_t kMax =
    std::numeric_limits<PosAttr::value_t>::max();

  const uint32_t slot = order[d];
  // Phrase-adjacent partner and the pair's expected step. Rightward slots
  // pair with slot-1, leftward with slot+1; `forward` picks the sign so the
  // delta is always in phrase order.
  const bool forward = slot > anchor;
  const uint32_t partner = forward ? slot - 1 : slot + 1;
  const PosAttr::value_t expected =
    forward ? expected_steps[slot - 1] : expected_steps[slot];
  const PosAttr::value_t pv = chain[partner];

  const PosAttr::value_t budget = slop - cost_so_far;
  const PosAttr::value_t span = budget + 1;

  // Loose [lo, hi] superset of positions whose StepCost can still fit the
  // remaining budget; centred on pv + expected (forward) or pv - expected
  // (backward). The exact StepCost check below does the real pruning.
  PosAttr::value_t lo;
  PosAttr::value_t hi;
  if (forward) {
    const PosAttr::value_t center =
      (pv > kMax - expected) ? kMax : pv + expected;
    lo = (center > span) ? static_cast<PosAttr::value_t>(center - span) : 0;
    hi = (center > kMax - span) ? kMax
                                : static_cast<PosAttr::value_t>(center + span);
  } else {
    const PosAttr::value_t center =
      (pv > expected) ? static_cast<PosAttr::value_t>(pv - expected) : 0;
    lo = (center > span) ? static_cast<PosAttr::value_t>(center - span) : 0;
    hi = (center > kMax - span) ? kMax
                                : static_cast<PosAttr::value_t>(center + span);
  }

  const auto& sp = slots[slot];
  auto begin = std::lower_bound(sp.begin(), sp.end(), lo);
  auto end = std::upper_bound(sp.begin(), sp.end(), hi);
  for (auto it = begin; it != end; ++it) {
    const PosAttr::value_t p = *it;
    if (enforce_uniqueness) {
      bool dup = false;
      for (size_t k = 0; k < d; ++k) {
        if (chain[order[k]] == p &&
            (groups.empty() || groups[order[k]] == groups[slot])) {
          dup = true;
          break;
        }
      }
      if (dup) {
        continue;
      }
    }
    const int64_t delta =
      forward ? static_cast<int64_t>(p) - static_cast<int64_t>(pv)
              : static_cast<int64_t>(pv) - static_cast<int64_t>(p);
    const PosAttr::value_t step = StepCost(delta, expected);
    if (cost_so_far + step > slop) {
      continue;
    }
    chain[slot] = p;
    CountFromAnchor(slots, slop, expected_steps, chain, order, anchor, d + 1,
                    static_cast<PosAttr::value_t>(cost_so_far + step), res,
                    early_exit, groups, enforce_uniqueness, out);
    if (early_exit && res.any) {
      return;
    }
  }
}

SLOP_PROF_NOINLINE inline MatchResult Run(
  const std::vector<std::vector<PosAttr::value_t>>& slot_pos,
  PosAttr::value_t slop, const std::vector<PosAttr::value_t>& expected_steps,
  MatchScratch& scratch, bool early_exit,
  const std::vector<uint32_t>& groups = {},
  std::vector<EnumeratedMatch>* out = nullptr) {
  // A collector implies a full count; early exit would truncate it.
  SDB_ASSERT(!(early_exit && out));
  if (out) {
    out->clear();
  }
  MatchResult res{};
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
  SDB_ASSERT(groups.empty() || groups.size() == n);

  const bool enforce_uniqueness = EnforceUniqueness(groups);

  // Anchor at the rarest slot: iterating the fewest anchor positions
  // minimizes window searches, and lets n >= 3 phrases lead from the rarest
  // slot rather than slot 0 (a win on dense-slot0 phrases like "the european
  // union"). Step cost sums over phrase-adjacent pairs regardless of which
  // slot leads, so freq and best_distance are unchanged.
  uint32_t anchor = 0;
  for (uint32_t i = 1; i < n; ++i) {
    if (slot_pos[i].size() < slot_pos[anchor].size()) {
      anchor = i;
    }
  }

  // Visitation order: anchor, then rightward r+1..n-1, then leftward r-1..0.
  auto& order = scratch.order;
  order.resize(n);
  size_t idx = 0;
  order[idx++] = anchor;
  for (uint32_t s = anchor + 1; s < n; ++s) {
    order[idx++] = s;
  }
  for (uint32_t s = anchor; s-- > 0;) {
    order[idx++] = s;
  }
  SDB_ASSERT(idx == n);

  auto& chain = scratch.chain;
  chain.resize(n);
  for (PosAttr::value_t pa : slot_pos[anchor]) {
    chain[anchor] = pa;
    CountFromAnchor(slot_pos, slop, expected_steps, chain, order, anchor, 1, 0,
                    res, early_exit, groups, enforce_uniqueness, out);
    if (early_exit && res.any) {
      return res;
    }
  }

  if (out) {
    // Anchor order is not document order; emit matches sorted by
    // leftmost position (full tuple compare keeps ties deterministic).
    std::sort(out->begin(), out->end(),
              [](const EnumeratedMatch& a, const EnumeratedMatch& b) noexcept {
                return std::tie(a.leftmost, a.rightmost, a.leftmost_slot,
                                a.rightmost_slot) <
                       std::tie(b.leftmost, b.rightmost, b.leftmost_slot,
                                b.rightmost_slot);
              });
  }
  return res;
}

}  // namespace detail::slop

// Sloppy counterpart of FixedPhraseFrequency. Offs collects OffsAttr and
// emits per-match offsets through PhrasePosition iteration. HasFreq computes
// exact freq + best_distance; when false the matcher early-exits on the first
// valid tuple and sets freq to 1 to signal a match. kHasBoost = HasFreq.
template<bool Offs, bool HasFreq>
class SlopPhraseFrequency {
 public:
  using TermPosition = FixedTermPosition<Offs>;
  using Positions = std::vector<TermPosition>;

  static constexpr bool kHasBoost = HasFreq;
  static constexpr bool kHasFreq = HasFreq;

  SlopPhraseFrequency(std::vector<TermPosition>&& pos,
                      PosAttr::value_t max_slop,
                      std::vector<PosAttr::value_t>&& expected_steps) noexcept
    : _pos{std::move(pos)},
      _max_slop{max_slop},
      _expected_steps{std::move(expected_steps)} {
    SDB_ASSERT(_pos.size() >= 2);
    SDB_ASSERT(_max_slop > 0);
    SDB_ASSERT(_expected_steps.size() == _pos.size() - 1);
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

    // Two-slot phrases bypass gather + Run entirely: the fused
    // merge-join consumes the position iterators directly.
    if (n == 2 && !detail::slop::gPairJoinDisabled) {
      return MatchPair();
    }

    if constexpr (Offs) {
      _slot_offs_start.resize(n);
      _slot_offs_end.resize(n);
    }

    // Per-slot gather: read the whole posting into _slot_pos[i]
    // (+ _slot_offs_start[i] / _slot_offs_end[i] when Offs).
    const auto gather_all = [&](size_t i) {
      auto& it = *_pos[i].first;
      auto& positions = _slot_pos[i];
      // Gather is the first consumer of positions for this doc, so the
      // whole per-doc posting is still pending and DocFreq is exact.
      // No clear() first: resize from the previous size skips the
      // value-initialization that a resize from zero would do, and
      // ReadAll overwrites the slot in full anyway.
      if constexpr (!Offs) {
        positions.resize(it.DocFreq());
        const auto count = it.ReadAll(positions.data());
        SDB_ASSERT(count == positions.size());
      } else {
        auto& starts = _slot_offs_start[i];
        auto& ends = _slot_offs_end[i];
        if (detail::slop::gOffsBulkGatherDisabled) [[unlikely]] {
          positions.clear();
          starts.Clear();
          ends.Clear();
          const OffsAttr* offs = irs::get<OffsAttr>(it);
          while (it.next()) {
            positions.push_back(it.value());
            starts.PushBack(offs ? offs->start : 0);
            ends.PushBack(offs ? offs->end : 0);
          }
        } else {
          positions.resize(it.DocFreq());
          starts.ResizeUninit(positions.size());
          ends.ResizeUninit(positions.size());
          const auto count =
            it.ReadAll(positions.data(), starts.Data(), ends.Data());
          SDB_ASSERT(count == positions.size());
        }
      }
    };

    for (size_t i = 0; i < n; ++i) {
      gather_all(i);
      if (_slot_pos[i].empty()) {
        return false;
      }
    }

    std::vector<detail::slop::EnumeratedMatch>* collect = nullptr;
    if constexpr (Offs && HasFreq) {
      collect = &_enumerated;
    }
    auto res =
      detail::slop::Run(_slot_pos, _max_slop, _expected_steps, _match_scratch,
                        /*early_exit=*/!HasFreq, _term_groups, collect);
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
  friend class PhrasePosition<SlopPhraseFrequency>;

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
      // filter-only or no-offsets path: single emission
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

  // n == 2 path: runs JoinPair over the two position iterators and maps its
  // result onto the generic outputs (_phrase_freq, _best_distance, _matches).
  // Anchor = rarer slot by per-doc DocFreq; ties keep slot 0, as Run does.
  bool MatchPair() {
    const bool anchor_is_slot0 =
      _pos[0].first->DocFreq() <= _pos[1].first->DocFreq();
    auto& anchor = *_pos[anchor_is_slot0 ? 0 : 1].first;
    auto& partner = *_pos[anchor_is_slot0 ? 1 : 0].first;

    const OffsAttr* anchor_offs = nullptr;
    const OffsAttr* partner_offs = nullptr;
    if constexpr (Offs) {
      anchor_offs = irs::get<OffsAttr>(anchor);
      partner_offs = irs::get<OffsAttr>(partner);
    }

    std::vector<detail::slop::PairMatch>* collect = nullptr;
    if constexpr (Offs && HasFreq) {
      collect = &_pair_matches;
    }

    const auto res = detail::slop::JoinPair<Offs, HasFreq>(
      anchor, partner, anchor_offs, partner_offs, anchor_is_slot0, _max_slop,
      _expected_steps[0], detail::slop::EnforceUniqueness(_term_groups),
      _pair_scratch, collect);
    if (!res.any) {
      return false;
    }

    if constexpr (HasFreq) {
      _phrase_freq = static_cast<uint32_t>(res.freq);
      _best_distance = res.best_distance;
      if constexpr (Offs) {
        // Same single-pass invariant as the generic path: the join
        // emits exactly one PairMatch per counted pair.
        SDB_ASSERT(static_cast<uint32_t>(_pair_matches.size()) == _phrase_freq);
        _matches.reserve(_pair_matches.size());
        for (const auto& m : _pair_matches) {
          _matches.push_back({m.start_offset, m.end_offset});
        }
        if (!_matches.empty()) {
          _start_offset = _matches[0].start;
          _end_offset = _matches[0].end;
          _match_idx = 1;
        }
      }
    } else {
      _phrase_freq = 1;
    }
    return true;
  }

  // Resolve leftmost/rightmost OffsAttr for the matcher-collected tuples and
  // pre-load match #0. The collector ran inside the same DFS that produced
  // _phrase_freq, so the assert is a cheap check of that invariant.
  void BuildMatches() {
    if constexpr (!Offs || !HasFreq) {
      return;
    }
    SDB_ASSERT(static_cast<uint32_t>(_enumerated.size()) == _phrase_freq);
    _matches.clear();
    _matches.reserve(_enumerated.size());
    auto find_index = [&](size_t slot, PosAttr::value_t p) -> size_t {
      const auto& sp = _slot_pos[slot];
      auto it = std::lower_bound(sp.begin(), sp.end(), p);
      SDB_ASSERT(it != sp.end() && *it == p);
      return static_cast<size_t>(it - sp.begin());
    };
    for (const auto& m : _enumerated) {
      const size_t li = find_index(m.leftmost_slot, m.leftmost);
      const size_t ri = find_index(m.rightmost_slot, m.rightmost);
      _matches.push_back({_slot_offs_start[m.leftmost_slot][li],
                          _slot_offs_end[m.rightmost_slot][ri]});
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
  std::vector<std::vector<PosAttr::value_t>> _slot_pos;
  // Per-slot offsets, parallel to _slot_pos, stored as separate start/end
  std::vector<detail::slop::UninitU32Buf> _slot_offs_start;
  std::vector<detail::slop::UninitU32Buf> _slot_offs_end;
  // Matcher scratch, kept off the allocator on the hot path.
  detail::slop::MatchScratch _match_scratch;
  // Valid tuples from the matcher pass, sorted by leftmost (Offs && HasFreq
  // only); source for BuildMatches.
  std::vector<detail::slop::EnumeratedMatch> _enumerated;
  // n == 2 fused merge-join state.
  detail::slop::PairScratch _pair_scratch;
  std::vector<detail::slop::PairMatch> _pair_matches;
  // All emitted matches (Offs && HasFreq only), sorted by leftmost; each holds
  // the leftmost token's start and the rightmost token's end offset.
  std::vector<OffsetPair> _matches;
  size_t _match_idx = 0;
  uint32_t _phrase_freq = 0;
  PosAttr::value_t _best_distance = 0;
  uint32_t _start_offset{0};
  uint32_t _end_offset{0};

  std::vector<uint32_t> _term_groups;
};

// Sloppy counterpart of VariadicPhraseFrequency.
template<typename Adapter, bool HasFreq>
class SlopVariadicPhraseFrequency {
 public:
  using TermPosition = VariadicTermPosition<Adapter>;
  using Positions = std::vector<TermPosition>;

  static constexpr bool kHasBoost = HasFreq;
  static constexpr bool kHasFreq = HasFreq;

  SlopVariadicPhraseFrequency(
    std::vector<TermPosition>&& pos, PosAttr::value_t max_slop,
    std::vector<PosAttr::value_t>&& expected_steps) noexcept
    : _pos{std::move(pos)},
      _max_slop{max_slop},
      _expected_steps{std::move(expected_steps)} {
    SDB_ASSERT(_pos.size() >= 2);
    SDB_ASSERT(_max_slop > 0);
    SDB_ASSERT(_expected_steps.size() == _pos.size() - 1);
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

    if (_term_groups.size() != n) {
      _term_groups.clear();
      _term_groups.reserve(n);
      for (const auto& p : _pos) {
        _term_groups.push_back(p.second.term_group);
      }
    }

    // Two-slot phrases bypass gather + Run entirely: the fused merge-join
    // consumes the merged per-slot position streams directly.
    if (n == 2 && !detail::slop::gPairJoinDisabled) {
      return MatchPair();
    }

    // Reuse inner vectors' capacity (see SlopPhraseFrequency::Match)
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

    for (size_t i = 0; i < n; ++i) {
      if (!gather_all(i)) {
        return false;
      }
    }

    // Uniqueness is scoped to slot groups: connectivity components of the
    // per-segment query term sets, computed at prepare-collect and carried
    // in TermInterval::term_group (ES-verified rule).
    std::vector<detail::slop::EnumeratedMatch>* collect = nullptr;
    if constexpr (kHasOffsets && HasFreq) {
      collect = &_enumerated;
    }
    auto res =
      detail::slop::Run(_slot_pos, _max_slop, _expected_steps, _match_scratch,
                        /*early_exit=*/!HasFreq, _term_groups, collect);
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
  friend class PhrasePosition<SlopVariadicPhraseFrequency>;

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

  struct BindCtx {
    detail::slop::MergedPosStream<kHasOffsets>* stream;
  };

  // Registers one sub-iterator of the current document into the slot's
  // merged stream (n == 2 join path).
  static bool BindOneSubIter(void* ctx, Adapter& adapter) {
    SDB_ASSERT(ctx);
    auto& c = *reinterpret_cast<BindCtx*>(ctx);
    auto* p = adapter.position;
    if (!p) {
      return true;
    }
    const OffsAttr* offs = nullptr;
    if constexpr (kHasOffsets) {
      offs = adapter.offset;
    }
    c.stream->Add(p, offs);
    return true;
  }

  struct SoloCtx {
    PosAttr* pos{nullptr};
    const OffsAttr* offs{nullptr};
    uint32_t count{0};
  };

  // Counts the current document's sub-iterators and captures the last one.
  // A slot with exactly one sub feeds JoinPair its raw position iterator;
  // the merged-stream layer only pays off when there is something to merge.
  static bool CaptureSoloSubIter(void* ctx, Adapter& adapter) {
    SDB_ASSERT(ctx);
    auto& c = *reinterpret_cast<SoloCtx*>(ctx);
    if (!adapter.position) {
      return true;
    }
    ++c.count;
    c.pos = adapter.position;
    if constexpr (kHasOffsets) {
      c.offs = adapter.offset;
    }
    return true;
  }

  std::pair<const uint32_t*, const uint32_t*> GetOffsets() const noexcept {
    return {&_start_offset, &_end_offset};
  }

  // See SlopPhraseFrequency::NextPosition() for state machine
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

  // n == 2 path: feeds JoinPair with the two slots' positions and maps its
  // result onto the generic outputs (_phrase_freq, _best_distance,
  // _matches). When both slots hold a single sub-iterator in the current
  // document (the common real-text case), the raw position iterators go
  // straight into the join; otherwise each slot is merged first. Anchor =
  // rarer slot by the disjunction's doc-level cost estimate (a compound
  // iterator has no exact per-doc count); ties keep slot 0, as Run does.
  bool MatchPair() {
    std::array<SoloCtx, 2> solo;
    for (size_t i = 0; i < 2; ++i) {
      _pos[i].first->visit(&solo[i], CaptureSoloSubIter);
      if (solo[i].count == 0) {
        return false;
      }
    }

    const bool anchor_is_slot0 =
      CostAttr::extract(*_pos[0].first) <= CostAttr::extract(*_pos[1].first);

    const size_t a = anchor_is_slot0 ? 0 : 1;
    const size_t p = a ^ 1;

    // Solo slots go into the join raw; only multi-sub slots pay for the
    // merged stream. A stream bound from count > 0 subs cannot be empty.
    for (size_t i = 0; i < 2; ++i) {
      if (solo[i].count == 1) {
        solo[i].pos->reset();
        continue;
      }
      auto& stream = _merged[i];
      stream.Clear();
      BindCtx ctx{&stream};
      _pos[i].first->visit(&ctx, BindOneSubIter);
      SDB_ASSERT(!stream.Empty());
    }

    const auto offs_of = [&](size_t i) -> const OffsAttr* {
      if constexpr (kHasOffsets) {
        return solo[i].count == 1 ? solo[i].offs : _merged[i].GetOffs();
      } else {
        return nullptr;
      }
    };
    const OffsAttr* a_offs = offs_of(a);
    const OffsAttr* p_offs = offs_of(p);

    if (solo[a].count == 1 && solo[p].count == 1) {
      return RunPair(*solo[a].pos, a_offs, *solo[p].pos, p_offs,
                     anchor_is_slot0);
    }
    if (solo[a].count == 1) {
      return RunPair(*solo[a].pos, a_offs, _merged[p], p_offs, anchor_is_slot0);
    }
    if (solo[p].count == 1) {
      return RunPair(_merged[a], a_offs, *solo[p].pos, p_offs, anchor_is_slot0);
    }
    return RunPair(_merged[a], a_offs, _merged[p], p_offs, anchor_is_slot0);
  }

  std::vector<detail::slop::PairMatch>* PairCollect() {
    if constexpr (kHasOffsets && HasFreq) {
      return &_pair_matches;
    } else {
      return nullptr;
    }
  }

  // Maps a JoinPair result onto the generic outputs; shared by the raw and
  // merged branches of MatchPair.
  bool FinishPair(const detail::slop::MatchResult& res) {
    if (!res.any) {
      return false;
    }
    if constexpr (HasFreq) {
      _phrase_freq = static_cast<uint32_t>(res.freq);
      _best_distance = res.best_distance;
      if constexpr (kHasOffsets) {
        // Same single-pass invariant as the generic path: the join emits
        // exactly one PairMatch per counted pair.
        SDB_ASSERT(static_cast<uint32_t>(_pair_matches.size()) == _phrase_freq);
        _matches.reserve(_pair_matches.size());
        for (const auto& m : _pair_matches) {
          _matches.push_back({m.start_offset, m.end_offset});
        }
        if (!_matches.empty()) {
          _start_offset = _matches[0].start;
          _end_offset = _matches[0].end;
          _match_idx = 1;
        }
      }
    } else {
      _phrase_freq = 1;
    }
    return true;
  }

  template<typename AnchorIt, typename PartnerIt>
  bool RunPair(AnchorIt& anchor, const OffsAttr* anchor_offs,
               PartnerIt& partner, const OffsAttr* partner_offs,
               bool anchor_is_slot0) {
    return FinishPair(detail::slop::JoinPair<kHasOffsets, HasFreq>(
      anchor, partner, anchor_offs, partner_offs, anchor_is_slot0, _max_slop,
      _expected_steps[0], detail::slop::EnforceUniqueness(_term_groups),
      _pair_scratch, PairCollect()));
  }

  // See SlopPhraseFrequency::BuildMatches
  void BuildMatches() {
    if constexpr (!kHasOffsets || !HasFreq) {
      return;
    }
    SDB_ASSERT(static_cast<uint32_t>(_enumerated.size()) == _phrase_freq);
    _matches.clear();
    _matches.reserve(_enumerated.size());
    auto find_index = [&](size_t slot, PosAttr::value_t p) -> size_t {
      const auto& sp = _slot_pos[slot];
      auto it = std::lower_bound(sp.begin(), sp.end(), p);
      SDB_ASSERT(it != sp.end() && *it == p);
      return static_cast<size_t>(it - sp.begin());
    };
    for (const auto& m : _enumerated) {
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
  std::vector<std::vector<PosAttr::value_t>> _slot_pos;
  std::vector<std::vector<OffsetPair>> _slot_offs;
  std::vector<PosEntry> _scratch_entries;
  // Matcher scratch, kept off the allocator on the hot path.
  detail::slop::MatchScratch _match_scratch;
  // Valid tuples from the matcher pass, sorted by leftmost (kHasOffsets &&
  // HasFreq only).
  std::vector<detail::slop::EnumeratedMatch> _enumerated;
  // n == 2 fused merge-join state.
  std::array<detail::slop::MergedPosStream<kHasOffsets>, 2> _merged;
  detail::slop::PairScratch _pair_scratch;
  std::vector<detail::slop::PairMatch> _pair_matches;
  std::vector<OffsetPair> _matches;
  size_t _match_idx = 0;
  uint32_t _phrase_freq = 0;
  PosAttr::value_t _best_distance = 0;
  uint32_t _start_offset{0};
  uint32_t _end_offset{0};
  std::vector<uint32_t> _term_groups;
};

}  // namespace irs
