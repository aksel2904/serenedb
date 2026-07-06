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

// Correctness coverage for the sloppy-phrase gather machinery and the n == 2
// fused merge-join.
//
// On balanced data the heuristic never selects seek-gather, and read-all
// would produce the same matches anyway, so black-box data tests can't prove
// the seek-gather path. Instead these drive both gather strategies over
// identical data via the gGatherOverride seam and assert bit-for-bit equal
// results, plus unit-test BuildWindows directly.
//
// n == 2 phrases - fixed and variadic alike - route to the merge-join in
// production and never reach gather, so every gather-equivalence check on a
// two-term query runs under PairJoinGuard - otherwise both sides would run
// the join and the comparison would be a tautology. Join-vs-legacy
// equivalence is asserted separately by the pair_join_equivalence_* tests.
// The SlopOverlapMatcher tests at the end pin the n >= 3 same-position
// (term-group) semantics of dp::Run.
//

#include "filter_test_case_base.hpp"
#include "iresearch/analysis/token_attributes.hpp"
#include "iresearch/index/iterators.hpp"
#include "iresearch/search/phrase_filter.hpp"
#include "iresearch/search/phrase_iterator.hpp"
#include "iresearch/search/phrase_query.hpp"
#include "iresearch/search/slop_phrase_dp.hpp"
#include "tests_shared.hpp"

namespace {

namespace dp = irs::detail::slop_dp;

constexpr irs::field_id kField = tests::FieldIdFor("phrase_anl");

// Restores the gather override to kAuto on scope exit so a forced mode
// never leaks into another test.
class GatherModeGuard {
 public:
  explicit GatherModeGuard(dp::GatherOverride mode) noexcept {
    dp::gGatherOverride = mode;
  }
  ~GatherModeGuard() { dp::gGatherOverride = dp::GatherOverride::kAuto; }

  GatherModeGuard(const GatherModeGuard&) = delete;
  GatherModeGuard& operator=(const GatherModeGuard&) = delete;
};

// Routes n == 2 phrases through the generic gather + Run path for the
// duration of the scope (the production default is the fused
// merge-join, which bypasses gather entirely). Same discipline as
// GatherModeGuard: never leaks past the scope.
class PairJoinGuard {
 public:
  PairJoinGuard() noexcept { dp::gPairJoinDisabled = true; }
  ~PairJoinGuard() { dp::gPairJoinDisabled = false; }

  PairJoinGuard(const PairJoinGuard&) = delete;
  PairJoinGuard& operator=(const PairJoinGuard&) = delete;
};

irs::bytes_view Term(std::string_view s) {
  return irs::ViewCast<irs::byte_type>(s);
}

// Collects matched doc ids across all segments for the prepared query.
// The gather mode is read at iteration time, so the same prepared query
// can be re-run under different GatherModeGuard scopes.
std::vector<irs::doc_id_t> CollectDocs(const irs::Filter::Query::ptr& prepared,
                                       const irs::DirectoryReader& rdr) {
  std::vector<irs::doc_id_t> out;
  for (auto sub = rdr.begin(); sub != rdr.end(); ++sub) {
    auto docs = prepared->execute({.segment = *sub});
    while (docs->next()) {
      out.push_back(docs->value());
    }
  }
  return out;
}

// Collects (doc, [(start,end)...]) per matched doc via ExecuteWithOffsets.
struct OffsetMatch {
  irs::doc_id_t doc;
  std::vector<std::pair<uint32_t, uint32_t>> offsets;

  bool operator==(const OffsetMatch&) const = default;
};

template<typename PhraseQueryT>
std::vector<OffsetMatch> CollectOffsets(const PhraseQueryT& phrase_query,
                                        const irs::DirectoryReader& rdr) {
  std::vector<OffsetMatch> out;
  for (auto sub = rdr.begin(); sub != rdr.end(); ++sub) {
    auto docs = phrase_query.ExecuteWithOffsets(*sub);
    if (!docs) {
      continue;
    }
    auto* pos = irs::GetMutable<irs::PosAttr>(docs.get());
    if (!pos) {
      continue;
    }
    auto* offs = irs::get<irs::OffsAttr>(*pos);
    while (docs->next()) {
      OffsetMatch m{.doc = docs->value()};
      while (pos->next()) {
        m.offsets.emplace_back(offs ? offs->start : 0, offs ? offs->end : 0);
      }
      out.push_back(std::move(m));
    }
  }
  return out;
}

}  // namespace

class SlopSeekGatherTestCase : public tests::FilterTestCaseBase {
 protected:
  // Runs the prepared query under read-all and seek-gather and asserts
  // identical doc ids (ctx is forwarded to the failure message). PairJoinGuard
  // keeps n == 2 queries - fixed and variadic alike - on the gather path this
  // helper exercises (production would route them to the join, making the
  // comparison vacuous); it is a no-op for n >= 3.
  void AssertExecuteEquivalent(const irs::Filter::Query::ptr& prepared,
                               const irs::DirectoryReader& rdr,
                               std::string_view ctx) {
    std::vector<irs::doc_id_t> read_all;
    std::vector<irs::doc_id_t> seek;
    {
      PairJoinGuard pj;
      GatherModeGuard g{dp::GatherOverride::kForceReadAll};
      read_all = CollectDocs(prepared, rdr);
    }
    {
      PairJoinGuard pj;
      GatherModeGuard g{dp::GatherOverride::kForceSeek};
      seek = CollectDocs(prepared, rdr);
    }
    ASSERT_EQ(read_all, seek) << "seek-gather diverged from read-all: " << ctx;
    ASSERT_FALSE(read_all.empty()) << "expected matches for: " << ctx;
  }
};

// BuildWindows: pure unit tests. Highest-certainty coverage of the merge
// logic, which is the genuinely new algorithmic bit (overlap handling is
// load-bearing because forward-only seek cannot re-read).

TEST(SlopBuildWindows, single) {
  std::vector<dp::Window> out;
  dp::BuildWindows(/*lead_pos=*/{10}, /*w=*/2, out);
  ASSERT_EQ(1u, out.size());
  ASSERT_EQ(8u, out[0].first);
  ASSERT_EQ(12u, out[0].second);
}

TEST(SlopBuildWindows, lo_clamped_to_min) {
  // p <= w must clamp lo to pos_limits::min() (==1), not 0, so seek() on
  // a fresh iterator is not a no-op.
  std::vector<dp::Window> out;
  dp::BuildWindows(/*lead_pos=*/{2}, /*w=*/5, out);
  ASSERT_EQ(1u, out.size());
  ASSERT_EQ(irs::pos_limits::min(), out[0].first);
  ASSERT_EQ(7u, out[0].second);
}

TEST(SlopBuildWindows, disjoint) {
  std::vector<dp::Window> out;
  dp::BuildWindows(/*lead_pos=*/{10, 100}, /*w=*/2, out);
  ASSERT_EQ(2u, out.size());
  ASSERT_EQ(8u, out[0].first);
  ASSERT_EQ(12u, out[0].second);
  ASSERT_EQ(98u, out[1].first);
  ASSERT_EQ(102u, out[1].second);
}

TEST(SlopBuildWindows, overlap_merges) {
  // 10 -> [8,12], 13 -> [11,15]; 11 <= 12 so they merge to [8,15].
  std::vector<dp::Window> out;
  dp::BuildWindows(/*lead_pos=*/{10, 13}, /*w=*/2, out);
  ASSERT_EQ(1u, out.size());
  ASSERT_EQ(8u, out[0].first);
  ASSERT_EQ(15u, out[0].second);
}

TEST(SlopBuildWindows, touching_then_gap) {
  // 10->[8,12], 12->[10,14] merge to [8,14]; 100->[98,102] stays separate.
  std::vector<dp::Window> out;
  dp::BuildWindows(/*lead_pos=*/{10, 12, 100}, /*w=*/2, out);
  ASSERT_EQ(2u, out.size());
  ASSERT_EQ(8u, out[0].first);
  ASSERT_EQ(14u, out[0].second);
  ASSERT_EQ(98u, out[1].first);
  ASSERT_EQ(102u, out[1].second);
}

// Equivalence: drive read-all vs seek-gather over the standard sequential
// corpus for a representative set of sloppy queries. Reuses the same query
// shapes as the existing sloppy tests.

TEST_P(SlopSeekGatherTestCase, equivalence_fixed) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  struct Spec {
    std::string_view ctx;
    std::vector<std::string_view> terms;
    irs::PosAttr::value_t slop;
  };
  const Spec specs[] = {
    {"quick fox s1", {"quick", "fox"}, 1},
    {"quick moved s3", {"quick", "moved"}, 3},
    {"fox brown s2 (reversal)", {"fox", "brown"}, 2},
    {"quick brown fox s1", {"quick", "brown", "fox"}, 1},
    {"quick fox moved s2", {"quick", "fox", "moved"}, 2},
    {"fox brown quick s4", {"fox", "brown", "quick"}, 4},
  };

  for (const auto& s : specs) {
    irs::ByPhrase q;
    *q.mutable_field_id() = kField;
    for (auto t : s.terms) {
      q.mutable_options()->push_back<irs::ByTermOptions>().term = Term(t);
    }
    q.mutable_options()->set_slop(s.slop);

    auto prepared = q.prepare({.index = rdr});
    ASSERT_NE(nullptr, prepared) << s.ctx;
    AssertExecuteEquivalent(prepared, rdr, s.ctx);
  }
}

TEST_P(SlopSeekGatherTestCase, equivalence_explicit_gap) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  // "quick __ moved": expected step 2 between slots.
  irs::ByPhrase q;
  *q.mutable_field_id() = kField;
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("quick");
  q.mutable_options()->push_back<irs::ByTermOptions>(/*offs=*/1).term =
    Term("moved");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  ASSERT_NE(nullptr, prepared);
  AssertExecuteEquivalent(prepared, rdr, "quick __ moved s1");
}

TEST_P(SlopSeekGatherTestCase, equivalence_variadic) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  // prefix qui* + fox -> VariadicPhraseQuery path.
  irs::ByPhrase q;
  *q.mutable_field_id() = kField;
  q.mutable_options()->push_back<irs::ByPrefixOptions>().term = Term("qui");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("fox");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  ASSERT_NE(nullptr, prepared);
  AssertExecuteEquivalent(prepared, rdr, "qui* fox s1 (variadic)");
}

// Offsets equivalence: ExecuteWithOffsets must emit identical per-match
// offsets under both gather strategies (covers _offs correctness after a
// multi-step forward seek).

TEST_P(SlopSeekGatherTestCase, equivalence_offsets_fixed) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  irs::ByPhrase q;
  *q.mutable_field_id() = kField;
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("quick");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("fox");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  const auto* phrase_query =
    dynamic_cast<const irs::FixedPhraseQuery*>(prepared.get());
  ASSERT_NE(nullptr, phrase_query);

  // n == 2: guard keeps the offsets runs on the gather path (see header).
  std::vector<OffsetMatch> read_all;
  std::vector<OffsetMatch> seek;
  {
    PairJoinGuard pj;
    GatherModeGuard g{dp::GatherOverride::kForceReadAll};
    read_all = CollectOffsets(*phrase_query, rdr);
  }
  {
    PairJoinGuard pj;
    GatherModeGuard g{dp::GatherOverride::kForceSeek};
    seek = CollectOffsets(*phrase_query, rdr);
  }
  ASSERT_FALSE(read_all.empty());
  ASSERT_EQ(read_all, seek);
}

TEST_P(SlopSeekGatherTestCase, equivalence_offsets_variadic) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  irs::ByPhrase q;
  *q.mutable_field_id() = kField;
  q.mutable_options()->push_back<irs::ByPrefixOptions>().term = Term("qui");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("fox");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  const auto* phrase_query =
    dynamic_cast<const irs::VariadicPhraseQuery*>(prepared.get());
  ASSERT_NE(nullptr, phrase_query);

  // n == 2: without the guard both runs would take the variadic join and
  // never consult the gather gate at all (see file header).
  std::vector<OffsetMatch> read_all;
  std::vector<OffsetMatch> seek;
  {
    PairJoinGuard pj;
    GatherModeGuard g{dp::GatherOverride::kForceReadAll};
    read_all = CollectOffsets(*phrase_query, rdr);
  }
  {
    PairJoinGuard pj;
    GatherModeGuard g{dp::GatherOverride::kForceSeek};
    seek = CollectOffsets(*phrase_query, rdr);
  }
  ASSERT_FALSE(read_all.empty());
  ASSERT_EQ(read_all, seek);
}

// Skewed inline data: a document with a 1:N in-doc frequency ratio so the
// heuristic (kAuto) actually selects seek-gather, and an adjacent slop-0 hit
// must still be found. The large ratio keeps the test robust if
// kSeekGatherSkew is raised; kAuto vs kForceReadAll cross-checks the gate.

TEST_P(SlopSeekGatherTestCase, skewed_engages_and_matches) {
  static constexpr char kData[] =
    R"([{"name":"SK","phrase":"rarexyz commonxyz commonxyz commonxyz )"
    R"(commonxyz commonxyz commonxyz commonxyz commonxyz commonxyz commonxyz )"
    R"(commonxyz commonxyz commonxyz commonxyz commonxyz commonxyz commonxyz )"
    R"(commonxyz commonxyz commonxyz"}])";
  {
    tests::JsonDocGenerator gen(kData, &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  irs::ByPhrase q;
  *q.mutable_field_id() = kField;
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("rarexyz");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("commonxyz");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  ASSERT_NE(nullptr, prepared);

  // Guard: without it this n == 2 query runs the join and the gate is
  // never consulted at all (see file header).
  std::vector<irs::doc_id_t> automatic;
  std::vector<irs::doc_id_t> read_all;
  {
    PairJoinGuard pj;
    GatherModeGuard g{dp::GatherOverride::kAuto};  // gate should pick seek
    automatic = CollectDocs(prepared, rdr);
  }
  {
    PairJoinGuard pj;
    GatherModeGuard g{dp::GatherOverride::kForceReadAll};
    read_all = CollectDocs(prepared, rdr);
  }
  ASSERT_EQ(1u, automatic.size());  // SK matches (rarexyz@1, commonxyz@2)
  ASSERT_EQ(read_all, automatic);
}

// Overlapping windows: two occurrences of the rare term close enough that
// their slop windows merge, exercising the merged-window forward sweep in
// the seek-gather collectors.

TEST_P(SlopSeekGatherTestCase, overlapping_windows_equivalent) {
  // rareq appears twice (@1 and @4); with slop large enough the two
  // windows around them overlap. commonq is dense so the gate triggers.
  static constexpr char kData[] =
    R"([{"name":"OV","phrase":"rareq commonq commonq rareq commonq commonq )"
    R"(commonq commonq commonq commonq commonq commonq commonq commonq )"
    R"(commonq commonq commonq commonq commonq commonq"}])";
  {
    tests::JsonDocGenerator gen(kData, &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  irs::ByPhrase q;
  *q.mutable_field_id() = kField;
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("rareq");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("commonq");
  q.mutable_options()->set_slop(
    3);  // W = slop + 1 = 4; windows around 1 & 4 overlap

  auto prepared = q.prepare({.index = rdr});
  ASSERT_NE(nullptr, prepared);
  AssertExecuteEquivalent(prepared, rdr, "overlapping windows");
}

// Pair-join equivalence: the n == 2 merge-join (production default) must
// produce exactly the docs the generic gather + Run path does. The join
// bypasses gather, so gGatherOverride can't reach it; gPairJoinDisabled
// exposes the legacy path, driven here under both gather modes.
TEST_P(SlopSeekGatherTestCase, pair_join_equivalence_fixed) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  struct Spec {
    std::string_view ctx;
    std::vector<std::string_view> terms;
    size_t gap_offs;  // extra offset before the second term (0 == none)
    irs::PosAttr::value_t slop;
  };
  const Spec specs[] = {
    {"quick fox s1", {"quick", "fox"}, 0, 1},
    {"quick moved s3", {"quick", "moved"}, 0, 3},
    {"fox brown s2 (reversal)", {"fox", "brown"}, 0, 2},
    {"quick __ moved s1 (gap)", {"quick", "moved"}, 1, 1},
  };

  for (const auto& s : specs) {
    irs::ByPhrase q;
    *q.mutable_field_id() = kField;
    q.mutable_options()->push_back<irs::ByTermOptions>().term =
      Term(s.terms[0]);
    q.mutable_options()->push_back<irs::ByTermOptions>(s.gap_offs).term =
      Term(s.terms[1]);
    q.mutable_options()->set_slop(s.slop);

    auto prepared = q.prepare({.index = rdr});
    ASSERT_NE(nullptr, prepared) << s.ctx;

    const auto join = CollectDocs(prepared, rdr);
    std::vector<irs::doc_id_t> legacy_readall;
    std::vector<irs::doc_id_t> legacy_seek;
    {
      PairJoinGuard pj;
      GatherModeGuard g{dp::GatherOverride::kForceReadAll};
      legacy_readall = CollectDocs(prepared, rdr);
    }
    {
      PairJoinGuard pj;
      GatherModeGuard g{dp::GatherOverride::kForceSeek};
      legacy_seek = CollectDocs(prepared, rdr);
    }
    ASSERT_EQ(legacy_readall, join)
      << "pair join diverged from read-all: " << s.ctx;
    ASSERT_EQ(legacy_seek, join)
      << "pair join diverged from seek-gather: " << s.ctx;
    ASSERT_FALSE(join.empty()) << "expected matches for: " << s.ctx;
  }
}

// Same for the offsets path: per-match offsets (and, via the match
// count, freq) from the join must be identical to the generic path's.
TEST_P(SlopSeekGatherTestCase, pair_join_equivalence_offsets) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  irs::ByPhrase q;
  *q.mutable_field_id() = kField;
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("quick");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("fox");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  const auto* phrase_query =
    dynamic_cast<const irs::FixedPhraseQuery*>(prepared.get());
  ASSERT_NE(nullptr, phrase_query);

  const auto join = CollectOffsets(*phrase_query, rdr);
  std::vector<OffsetMatch> legacy;
  {
    PairJoinGuard pj;
    legacy = CollectOffsets(*phrase_query, rdr);
  }
  ASSERT_FALSE(join.empty());
  ASSERT_EQ(legacy, join);
}

// Variadic pair-join equivalence: an n == 2 variadic phrase (a term set per
// slot, here from prefix expansion) routes to the same merge-join through
// MergedPosStream. Same join-vs-legacy discipline as the fixed test.
// Duplicate positions inside a slot (same-position synonyms) cannot occur
// on this corpus; that case is pinned by the merged-stream fuzz oracle.
TEST_P(SlopSeekGatherTestCase, pair_join_equivalence_variadic) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  struct Slot {
    bool prefix;
    std::string_view term;
  };
  struct Spec {
    std::string_view ctx;
    Slot first;
    Slot second;
    size_t gap_offs;  // extra offset before the second term (0 == none)
    irs::PosAttr::value_t slop;
  };
  const Spec specs[] = {
    {"qui* fox s1", {true, "qui"}, {false, "fox"}, 0, 1},
    {"qui* moved s3", {true, "qui"}, {false, "moved"}, 0, 3},
    {"fox qui* s3 (reversal)", {false, "fox"}, {true, "qui"}, 0, 3},
    {"qui* __ moved s1 (gap)", {true, "qui"}, {false, "moved"}, 1, 1},
  };

  for (const auto& s : specs) {
    irs::ByPhrase q;
    *q.mutable_field_id() = kField;
    auto& opts = *q.mutable_options();
    if (s.first.prefix) {
      opts.push_back<irs::ByPrefixOptions>().term = Term(s.first.term);
    } else {
      opts.push_back<irs::ByTermOptions>().term = Term(s.first.term);
    }
    if (s.second.prefix) {
      opts.push_back<irs::ByPrefixOptions>(s.gap_offs).term =
        Term(s.second.term);
    } else {
      opts.push_back<irs::ByTermOptions>(s.gap_offs).term = Term(s.second.term);
    }
    opts.set_slop(s.slop);

    auto prepared = q.prepare({.index = rdr});
    ASSERT_NE(nullptr, prepared) << s.ctx;

    const auto join = CollectDocs(prepared, rdr);
    std::vector<irs::doc_id_t> legacy_readall;
    std::vector<irs::doc_id_t> legacy_seek;
    {
      PairJoinGuard pj;
      GatherModeGuard g{dp::GatherOverride::kForceReadAll};
      legacy_readall = CollectDocs(prepared, rdr);
    }
    {
      PairJoinGuard pj;
      GatherModeGuard g{dp::GatherOverride::kForceSeek};
      legacy_seek = CollectDocs(prepared, rdr);
    }
    ASSERT_EQ(legacy_readall, join)
      << "variadic pair join diverged from read-all: " << s.ctx;
    ASSERT_EQ(legacy_seek, join)
      << "variadic pair join diverged from seek-gather: " << s.ctx;
    ASSERT_FALSE(join.empty()) << "expected matches for: " << s.ctx;
  }
}

// Offsets path through the variadic join: per-match offsets resolved by the
// merged streams must be identical to the generic gather path's. Exact
// comparison is safe here: with no same-position tokens in the corpus no
// slot holds duplicate positions, the one case where the two paths may
// legitimately source offsets from different equal-position terms.
TEST_P(SlopSeekGatherTestCase, pair_join_equivalence_offsets_variadic) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  irs::ByPhrase q;
  *q.mutable_field_id() = kField;
  q.mutable_options()->push_back<irs::ByPrefixOptions>().term = Term("qui");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("fox");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  const auto* phrase_query =
    dynamic_cast<const irs::VariadicPhraseQuery*>(prepared.get());
  ASSERT_NE(nullptr, phrase_query);

  const auto join = CollectOffsets(*phrase_query, rdr);
  std::vector<OffsetMatch> legacy;
  {
    PairJoinGuard pj;
    legacy = CollectOffsets(*phrase_query, rdr);
  }
  ASSERT_FALSE(join.empty());
  ASSERT_EQ(legacy, join);
}

static constexpr auto kTestDirs = tests::GetDirectories<tests::kTypesDefault>();

INSTANTIATE_TEST_SUITE_P(slop_seek_gather_test, SlopSeekGatherTestCase,
                         ::testing::Combine(::testing::ValuesIn(kTestDirs),
                                            ::testing::Values(tests::FormatInfo{
                                              "1_5simd"})),
                         SlopSeekGatherTestCase::to_string);

// SlopOverlapMatcher: n >= 3 same-position matching, driving
// detail::slop_dp::Run directly with synthetic per-slot position lists and
// term-group ids. Encodes the empirically-verified Elasticsearch n >= 3 spec
// for "foo qux" indexed with synonym foo,bar (postings foo@0, bar@0, qux@1).
// "foo bar qux": no match at slop 0, one match at slop >= 1 (distinct terms
// may share position 0, but that costs 1, so it needs slop). "foo foo qux":
// no match at any slop (one foo occurrence can't fill both foo slots). Group
// ids mark same-term: {0,1,2} for foo/bar/qux, {0,0,2} when foo repeats. Slot
// positions: foo {0}, bar {0}, qux {1} (the repeated foo slot reads foo's
// postings too).

// Small slots: anchor DFS over single-position slots.

TEST(SlopOverlapMatcher, n3_distinct_terms_share_position) {
  dp::DpScratch scratch;
  const std::vector<std::vector<irs::PosAttr::value_t>> slot_pos = {
    {0}, {0}, {1}};
  const std::vector<irs::PosAttr::value_t> expected_steps = {1, 1};
  const std::vector<uint32_t> groups = {0, 1, 2};  // foo, bar, qux

  // slop 0: same position is not adjacency -> no match.
  {
    auto r = dp::Run(slot_pos, /*slop=*/0, expected_steps, scratch,
                     /*early_exit=*/false, groups);
    EXPECT_FALSE(r.any);
  }
  // slop >= 1: foo@0 -> bar@0 (delta 0, cost 1) -> qux@1 (delta 1, cost 0) = 1.
  for (const irs::PosAttr::value_t slop : {1u, 2u, 5u}) {
    auto r = dp::Run(slot_pos, slop, expected_steps, scratch,
                     /*early_exit=*/false, groups);
    EXPECT_TRUE(r.any) << "slop=" << slop;
    EXPECT_EQ(1u, r.freq) << "slop=" << slop;
    EXPECT_EQ(1u, r.best_distance) << "slop=" << slop;
  }
}

TEST(SlopOverlapMatcher, n3_repeated_term_never_matches) {
  dp::DpScratch scratch;
  const std::vector<std::vector<irs::PosAttr::value_t>> slot_pos = {
    {0}, {0}, {1}};
  const std::vector<irs::PosAttr::value_t> expected_steps = {1, 1};
  const std::vector<uint32_t> groups = {0, 0, 2};  // foo, foo, qux

  // The single foo occurrence (pos 0) cannot fill both foo slots, at any slop.
  for (const irs::PosAttr::value_t slop : {0u, 1u, 5u}) {
    auto r = dp::Run(slot_pos, slop, expected_steps, scratch,
                     /*early_exit=*/false, groups);
    EXPECT_FALSE(r.any) << "slop=" << slop;
  }
}

// Wide slot variant. Historically these covered the bitmask-DP overflow
// fallback; that path is gone (the anchor DFS is unconditional), so today
// they just re-check the same group-aware uniqueness with a wide third slot,
// which changes the window volume the DFS prunes over. Cheap extra coverage.

TEST(SlopOverlapMatcher, n3_overflow_distinct_terms_share_position) {
  dp::DpScratch scratch;
  std::vector<irs::PosAttr::value_t> qux;
  for (irs::PosAttr::value_t p = 1; p <= 130; ++p) {
    qux.push_back(p);
  }
  const std::vector<std::vector<irs::PosAttr::value_t>> slot_pos = {
    {0}, {0}, std::move(qux)};
  const std::vector<irs::PosAttr::value_t> expected_steps = {1, 1};
  const std::vector<uint32_t> groups = {0, 1, 2};  // foo, bar, qux

  auto r = dp::Run(slot_pos, /*slop=*/200, expected_steps, scratch,
                   /*early_exit=*/false, groups);
  EXPECT_TRUE(r.any);
  EXPECT_EQ(1u, r.best_distance);
}

TEST(SlopOverlapMatcher, n3_overflow_repeated_term_never_matches) {
  dp::DpScratch scratch;
  std::vector<irs::PosAttr::value_t> qux;
  for (irs::PosAttr::value_t p = 1; p <= 130; ++p) {
    qux.push_back(p);
  }
  const std::vector<std::vector<irs::PosAttr::value_t>> slot_pos = {
    {0}, {0}, std::move(qux)};
  const std::vector<irs::PosAttr::value_t> expected_steps = {1, 1};
  const std::vector<uint32_t> groups = {0, 0, 2};  // foo, foo, qux

  auto r = dp::Run(slot_pos, /*slop=*/200, expected_steps, scratch,
                   /*early_exit=*/false, groups);
  EXPECT_FALSE(r.any);
}

// Guard: empty groups (variadic / opt-out) keeps strict uniqueness.
// With no group info Run must fall back to per-position uniqueness, i.e. the
// pre-fix behavior: two slots cannot share a position.

TEST(SlopOverlapMatcher, n3_empty_groups_enforces_position_uniqueness) {
  dp::DpScratch scratch;
  const std::vector<std::vector<irs::PosAttr::value_t>> slot_pos = {
    {0}, {0}, {1}};
  const std::vector<irs::PosAttr::value_t> expected_steps = {1, 1};

  // No groups argument -> default empty -> strict uniqueness -> foo@0/bar@0
  // collision dropped -> no match at any slop.

  for (const irs::PosAttr::value_t slop : {0u, 1u, 5u}) {
    auto r = dp::Run(slot_pos, slop, expected_steps, scratch,
                     /*early_exit=*/false);
    EXPECT_FALSE(r.any) << "slop=" << slop;
  }
}
