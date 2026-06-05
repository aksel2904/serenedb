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

// Correctness coverage for the sloppy-phrase seek-gather path.
//
// On balanced data the gather heuristic never selects seek-gather, and
// even if it did, read-all produces the same matches and would mask any
// divergence. So black-box data tests cannot prove the seek-gather path
// is correct. These tests instead drive both gather strategies over
// identical data via the gGatherOverride seam and assert the results are
// bit-for-bit equal, plus unit-test BuildWindows directly.
//
// NOTE: phrase_iterator.hpp must precede slop_phrase_dp.hpp - the latter
// uses FixedTermPosition / VariadicTermPosition from the former and does
// not include it.

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

constexpr std::string_view kField = "phrase_anl";

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
  // Runs the prepared query under read-all and under seek-gather, asserts
  // identical doc ids. ctx is forwarded to the failure message.
  void AssertExecuteEquivalent(const irs::Filter::Query::ptr& prepared,
                               const irs::DirectoryReader& rdr,
                               std::string_view ctx) {
    std::vector<irs::doc_id_t> read_all;
    std::vector<irs::doc_id_t> seek;
    {
      GatherModeGuard g{dp::GatherOverride::kForceReadAll};
      read_all = CollectDocs(prepared, rdr);
    }
    {
      GatherModeGuard g{dp::GatherOverride::kForceSeek};
      seek = CollectDocs(prepared, rdr);
    }
    ASSERT_EQ(read_all, seek) << "seek-gather diverged from read-all: " << ctx;
    ASSERT_FALSE(read_all.empty()) << "expected matches for: " << ctx;
  }
};

// ---------------------------------------------------------------------------
// BuildWindows: pure unit tests. Highest-certainty coverage of the merge
// logic, which is the genuinely new algorithmic bit (overlap handling is
// load-bearing because forward-only seek cannot re-read).
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Equivalence: drive read-all vs seek-gather over the standard sequential
// corpus for a representative set of sloppy queries. Reuses the same query
// shapes as the existing sloppy tests.
// ---------------------------------------------------------------------------

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
    *q.mutable_field() = kField;
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
  *q.mutable_field() = kField;
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
  *q.mutable_field() = kField;
  q.mutable_options()->push_back<irs::ByPrefixOptions>().term = Term("qui");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("fox");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  ASSERT_NE(nullptr, prepared);
  AssertExecuteEquivalent(prepared, rdr, "qui* fox s1 (variadic)");
}

// ---------------------------------------------------------------------------
// Offsets equivalence: ExecuteWithOffsets must emit identical per-match
// offsets under both gather strategies (covers _offs correctness after a
// multi-step forward seek).
// ---------------------------------------------------------------------------

TEST_P(SlopSeekGatherTestCase, equivalence_offsets_fixed) {
  {
    tests::JsonDocGenerator gen(resource("phrase_sequential.json"),
                                &tests::PayloadedJsonFieldFactory);
    add_segment(gen);
  }
  auto rdr = open_reader();

  irs::ByPhrase q;
  *q.mutable_field() = kField;
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("quick");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("fox");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  const auto* phrase_query =
    dynamic_cast<const irs::FixedPhraseQuery*>(prepared.get());
  ASSERT_NE(nullptr, phrase_query);

  std::vector<OffsetMatch> read_all;
  std::vector<OffsetMatch> seek;
  {
    GatherModeGuard g{dp::GatherOverride::kForceReadAll};
    read_all = CollectOffsets(*phrase_query, rdr);
  }
  {
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
  *q.mutable_field() = kField;
  q.mutable_options()->push_back<irs::ByPrefixOptions>().term = Term("qui");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("fox");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  const auto* phrase_query =
    dynamic_cast<const irs::VariadicPhraseQuery*>(prepared.get());
  ASSERT_NE(nullptr, phrase_query);

  std::vector<OffsetMatch> read_all;
  std::vector<OffsetMatch> seek;
  {
    GatherModeGuard g{dp::GatherOverride::kForceReadAll};
    read_all = CollectOffsets(*phrase_query, rdr);
  }
  {
    GatherModeGuard g{dp::GatherOverride::kForceSeek};
    seek = CollectOffsets(*phrase_query, rdr);
  }
  ASSERT_FALSE(read_all.empty());
  ASSERT_EQ(read_all, seek);
}

// ---------------------------------------------------------------------------
// Skewed inline data: builds a document with a 1:N in-doc frequency ratio
// so the natural heuristic (kAuto) actually selects seek-gather, and an
// adjacent slop-0 hit must still be found. The large ratio keeps the test
// robust if kSeekGatherSkew is raised. The kAuto vs kForceReadAll equality
// cross-checks the gate decision against the safe path.
// ---------------------------------------------------------------------------

TEST_P(SlopSeekGatherTestCase, skewed_engages_and_matches) {
  // rarexyz @1, commonxyz @2..21 (20 occurrences). df ratio 1:20.
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
  *q.mutable_field() = kField;
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("rarexyz");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("commonxyz");
  q.mutable_options()->set_slop(1);

  auto prepared = q.prepare({.index = rdr});
  ASSERT_NE(nullptr, prepared);

  std::vector<irs::doc_id_t> automatic;
  std::vector<irs::doc_id_t> read_all;
  {
    GatherModeGuard g{dp::GatherOverride::kAuto};  // gate should pick seek
    automatic = CollectDocs(prepared, rdr);
  }
  {
    GatherModeGuard g{dp::GatherOverride::kForceReadAll};
    read_all = CollectDocs(prepared, rdr);
  }
  ASSERT_EQ(1u, automatic.size());  // SK matches (rarexyz@1, commonxyz@2)
  ASSERT_EQ(read_all, automatic);
}

// ---------------------------------------------------------------------------
// Overlapping windows: two occurrences of the rare term close enough that
// their slop windows merge, exercising the merged-window forward sweep in
// the seek-gather collectors.
// ---------------------------------------------------------------------------

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
  *q.mutable_field() = kField;
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("rareq");
  q.mutable_options()->push_back<irs::ByTermOptions>().term = Term("commonq");
  q.mutable_options()->set_slop(
    3);  // W = slop + 1 = 4; windows around 1 & 4 overlap

  auto prepared = q.prepare({.index = rdr});
  ASSERT_NE(nullptr, prepared);
  AssertExecuteEquivalent(prepared, rdr, "overlapping windows");
}

static constexpr auto kTestDirs = tests::GetDirectories<tests::kTypesDefault>();

INSTANTIATE_TEST_SUITE_P(slop_seek_gather_test, SlopSeekGatherTestCase,
                         ::testing::Combine(::testing::ValuesIn(kTestDirs),
                                            ::testing::Values(tests::FormatInfo{
                                              "1_5simd"})),
                         SlopSeekGatherTestCase::to_string);
