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

// Microbenchmark: SlopPhrase (DP) vs functionally-equivalent disjunction
// of fixed phrases, on europarl text plus a synthetic skewed corpus.
//
// For two terms [t0, t1] with slop=N and expected step 1, the
// equivalent disjunction is built by MakeDisjunctionEquivalent.
//
// The slop-phrase Execute path is measured under three gather strategies
// driven via the gGatherOverride seam (A3 - no production counter):
//   _auto    : the production heuristic decides per document
//   _seek    : force the seek-gather path on every document
//   _readall : force the plain read-all path on every document
// Comparing _seek vs _readall isolates the gather mechanism; _auto shows
// what the heuristic actually picks; DisjunctionExec is the baseline.
//
// Term pairs include balanced pairs (where the gate stays on read-all,
// so _seek measures the no-skew regression) and one skewed real pair
// "the commission" (slot0 frequent -> exercises the reviewer's "frequent
// lead" case). A synthetic 60:1 corpus gives the mechanism's ceiling.
//
// Europarl dataset path comes from env SERENEDB_BENCH_EUROPARL, with a
// fallback path relative to the current working directory. Aborts if the
// file is missing.

#include <benchmark/benchmark.h>
#include <utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iresearch/analysis/analyzers.hpp>
#include <iresearch/analysis/tokenizers.hpp>
#include <iresearch/formats/formats.hpp>
#include <iresearch/index/directory_reader.hpp>
#include <iresearch/index/index_features.hpp>
#include <iresearch/index/index_writer.hpp>
#include <iresearch/search/boolean_filter.hpp>
#include <iresearch/search/phrase_filter.hpp>
#include <iresearch/search/phrase_query.hpp>
#include <iresearch/search/slop_phrase_dp.hpp>
#include <iresearch/store/data_output.hpp>
#include <iresearch/store/mmap_directory.hpp>
#include <iresearch/utils/string.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bench_sloppy {

// Indexes the body column of an europarl line into body_anl with
// Freq | Pos | Offs.

struct IField {
  using ptr = std::shared_ptr<IField>;
  virtual ~IField() = default;

  virtual irs::IndexFeatures GetIndexFeatures() const = 0;
  virtual irs::Tokenizer& GetTokens() const = 0;
  virtual std::string_view Name() const = 0;
  virtual bool Write(irs::DataOutput& out) const = 0;
};

class FieldBase : public IField {
 public:
  FieldBase() = default;

  irs::IndexFeatures GetIndexFeatures() const noexcept final {
    return _index_features;
  }
  std::string_view Name() const noexcept final { return _name; }

  void SetName(std::string name) { _name = std::move(name); }
  void SetIndexFeatures(irs::IndexFeatures f) noexcept { _index_features = f; }

 private:
  std::string _name;
  irs::IndexFeatures _index_features{irs::IndexFeatures::None};
};

class TextField final : public FieldBase {
 public:
  TextField(std::string name, irs::IndexFeatures extra_features)
    : _stream(irs::analysis::analyzers::Get(
        "text", irs::Type<irs::text_format::Json>::get(),
        "{\"locale\":\"C\", \"stopwords\":[]}")) {
    SetName(std::move(name));
    SetIndexFeatures(irs::IndexFeatures::Freq | irs::IndexFeatures::Pos |
                     irs::IndexFeatures::Offs | extra_features);
  }

  void SetValue(std::string_view value) noexcept { _value = value; }

  irs::Tokenizer& GetTokens() const final {
    _stream->reset(_value);
    return *_stream;
  }

  bool Write(irs::DataOutput&) const final { return false; }

 private:
  irs::analysis::Analyzer::ptr _stream;
  std::string_view _value;
};

class FieldList {
 public:
  class Iterator {
   public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = IField;
    using reference = IField&;
    using pointer = IField*;
    using difference_type = std::ptrdiff_t;

    Iterator() = default;
    explicit Iterator(std::vector<IField::ptr>::const_iterator it) : _it{it} {}

    reference operator*() const { return **_it; }
    pointer operator->() const { return _it->get(); }

    Iterator& operator++() {
      ++_it;
      return *this;
    }
    Iterator operator++(int) {
      Iterator tmp = *this;
      ++_it;
      return tmp;
    }

    bool operator==(const Iterator& rhs) const { return _it == rhs._it; }
    bool operator!=(const Iterator& rhs) const { return _it != rhs._it; }

    difference_type operator-(const Iterator& rhs) const {
      return _it - rhs._it;
    }

   private:
    std::vector<IField::ptr>::const_iterator _it;
  };

  void PushBack(IField::ptr f) { _fields.push_back(std::move(f)); }

  Iterator begin() const { return Iterator{_fields.begin()}; }
  Iterator end() const { return Iterator{_fields.end()}; }

 private:
  std::vector<IField::ptr> _fields;
};

struct Document {
  FieldList indexed;
  FieldList stored;
};

class EuroparlBodyTemplate {
 public:
  EuroparlBodyTemplate() {
    auto body_anl = std::make_shared<TextField>(std::string{"body_anl"},
                                                irs::IndexFeatures::None);
    _body_anl = body_anl.get();
    _doc.indexed.PushBack(std::move(body_anl));
  }

  void SetColumn(size_t idx, const std::string& value) {
    if (idx == 2) {
      _body = value;
      _body_anl->SetValue(_body);
    }
  }

  const Document& Get() const { return _doc; }

 private:
  Document _doc;
  TextField* _body_anl;
  std::string _body;
};

template<typename OctetIterator>
class BreakIterator {
 public:
  using Utf8Iterator = utf8::unchecked::iterator<OctetIterator>;

  BreakIterator(utf8::uint32_t delim, const OctetIterator& begin,
                const OctetIterator& end)
    : _delim{delim}, _wbegin{begin}, _wend{begin}, _end{end} {
    if (!Done()) {
      Next();
    }
  }

  explicit BreakIterator(const OctetIterator& end)
    : _wbegin{end}, _wend{end}, _end{end} {}

  const std::string& operator*() const { return _res; }

  bool operator==(const BreakIterator& rhs) const {
    return _wbegin == rhs._wbegin && _wend == rhs._wend;
  }
  bool operator!=(const BreakIterator& rhs) const { return !(*this == rhs); }

  bool Done() const { return _wbegin == _end; }

  BreakIterator& operator++() {
    Next();
    return *this;
  }

 private:
  void Next() {
    _wbegin = _wend;
    _wend = std::find(_wbegin, _end, _delim);
    if (_wend != _end) {
      _res.assign(_wbegin.base(), _wend.base());
      ++_wend;
    } else {
      _res.assign(_wbegin.base(), _end.base());
    }
  }

  utf8::uint32_t _delim;
  std::string _res;
  Utf8Iterator _wbegin;
  Utf8Iterator _wend;
  Utf8Iterator _end;
};

class EuroparlReader {
 public:
  EuroparlReader(const std::filesystem::path& file, EuroparlBodyTemplate& tpl,
                 uint32_t delim = 0x0009)
    : _ifs{file, std::ifstream::in | std::ifstream::binary},
      _tpl{&tpl},
      _delim{delim} {}

  const Document* Next() {
    if (!std::getline(_ifs, _line)) {
      return nullptr;
    }
    if (utf8::find_invalid(_line.begin(), _line.end()) != _line.end()) {
      return nullptr;
    }

    using Iter = BreakIterator<std::string::const_iterator>;
    Iter end{_line.end()};
    Iter it{_delim, _line.begin(), _line.end()};
    for (size_t i = 0; it != end; ++it, ++i) {
      _tpl->SetColumn(i, *it);
    }

    return &_tpl->Get();
  }

 private:
  std::ifstream _ifs;
  EuroparlBodyTemplate* _tpl;
  uint32_t _delim;
  std::string _line;
};

}  // namespace bench_sloppy
namespace {

namespace dp = irs::detail::slop_dp;

constexpr std::string_view kEuroparlFallbackPath =
  "resources/tests/iresearch/europarl.subset.big.txt";

constexpr std::string_view kFieldName = "body_anl";

constexpr std::string_view kFormatName = "1_5simd";

constexpr int kRepetitions = 5;

// Number of documents in the synthetic skewed corpus.
constexpr size_t kSyntheticDocs = 400;

// A3: drive the gather strategy explicitly so the benchmark can compare
// the seek-gather and read-all paths over identical data. Production
// never writes gGatherOverride; this is benchmark/test-only.
struct GatherModeGuard {
  explicit GatherModeGuard(dp::GatherOverride mode) noexcept {
    dp::gGatherOverride = mode;
  }
  ~GatherModeGuard() { dp::gGatherOverride = dp::GatherOverride::kAuto; }
};

struct ModeReg {
  const char* suffix;
  dp::GatherOverride mode;
};

constexpr ModeReg kModes[] = {
  {"_auto", dp::GatherOverride::kAuto},
  {"_seek", dp::GatherOverride::kForceSeek},
  {"_readall", dp::GatherOverride::kForceReadAll},
};

struct TermPair {
  std::string_view label;
  std::string_view term0;
  std::string_view term1;
};

constexpr TermPair kTermPairs[] = {
  {"european_union", "european", "union"},
  {"human_rights", "human", "rights"},
  {"climate_change", "climate", "change"},
  // Skewed: slot0 "the" is dense, "commission" is sparse, so the gate
  // engages and the DP would otherwise lead from the dense slot.
  {"the_commission", "the", "commission"},
};

constexpr irs::PosAttr::value_t kSlopValues[] = {1, 2, 5};

struct Corpus {
  std::filesystem::path dir_path;
  std::unique_ptr<irs::MMapDirectory> dir;
  irs::Format::ptr format;
  irs::DirectoryReader reader;
};

[[noreturn]] void Die(const char* msg) {
  std::fprintf(stderr, "sloppy_phrase_vs_disjunction bench: %s\n", msg);
  std::abort();
}

// Registers analyzers and formats exactly once across all corpora.
void EnsureRegistered() {
  static const bool once = [] {
    irs::analysis::analyzers::Init();
    irs::formats::Init();
    return true;
  }();
  (void)once;
}

std::filesystem::path ResolveDataPath() {
  if (const char* env = std::getenv("SERENEDB_BENCH_EUROPARL")) {
    return env;
  }
  return std::filesystem::path{kEuroparlFallbackPath};
}

Corpus BuildIndex() {
  auto data_path = ResolveDataPath();
  if (!std::filesystem::exists(data_path)) {
    std::fprintf(stderr,
                 "sloppy_phrase_vs_disjunction bench: europarl dataset not "
                 "found at '%s'\nSet SERENEDB_BENCH_EUROPARL or run from the "
                 "repo root so the relative fallback resolves.\n",
                 data_path.string().c_str());
    std::abort();
  }

  auto tmp_root = std::filesystem::temp_directory_path() /
                  "serenedb-bench-sloppy-phrase-vs-disjunction";
  std::filesystem::remove_all(tmp_root);
  std::filesystem::create_directories(tmp_root);

  EnsureRegistered();

  auto format = irs::formats::Get(std::string{kFormatName});
  if (!format) {
    Die("format 1_5simd not registered");
  }

  auto dir = std::make_unique<irs::MMapDirectory>(tmp_root);

  auto writer = irs::IndexWriter::Make(*dir, format, irs::kOmCreate,
                                       irs::IndexWriterOptions{});
  if (!writer) {
    Die("IndexWriter::Make returned null");
  }

  bench_sloppy::EuroparlBodyTemplate tpl;
  bench_sloppy::EuroparlReader reader{data_path, tpl};

  size_t inserted = 0;
  while (auto* doc = reader.Next()) {
    auto trx = writer->GetBatch();
    auto inserter = trx.Insert();
    if (!inserter.Insert<irs::Action::INDEX>(doc->indexed.begin(),
                                             doc->indexed.end())) {
      Die("Insert returned false");
    }
    ++inserted;
  }
  writer->Commit();

  if (inserted == 0) {
    Die("inserted 0 documents - dataset file empty?");
  }

  std::fprintf(
    stderr,
    "sloppy_phrase_vs_disjunction bench: indexed %zu documents from %s\n",
    inserted, data_path.string().c_str());

  auto rdr = irs::DirectoryReader{*dir, format};
  return Corpus{.dir_path = std::move(tmp_root),
                .dir = std::move(dir),
                .format = std::move(format),
                .reader = std::move(rdr)};
}

// Builds a synthetic corpus where every document is 30x "zzcmn", one
// "zzrre", 30x "zzcmn" - a 60:1 in-document frequency skew with the
// dense term as phrase slot0. This is the ceiling case for seek-gather.
Corpus BuildSyntheticIndex() {
  auto tmp_root =
    std::filesystem::temp_directory_path() / "serenedb-bench-sloppy-synthetic";
  std::filesystem::remove_all(tmp_root);
  std::filesystem::create_directories(tmp_root);

  EnsureRegistered();

  auto format = irs::formats::Get(std::string{kFormatName});
  if (!format) {
    Die("format 1_5simd not registered");
  }

  auto dir = std::make_unique<irs::MMapDirectory>(tmp_root);

  auto writer = irs::IndexWriter::Make(*dir, format, irs::kOmCreate,
                                       irs::IndexWriterOptions{});
  if (!writer) {
    Die("IndexWriter::Make returned null");
  }

  std::string body;
  body.reserve(512);
  for (int i = 0; i < 30; ++i) {
    body += "zzcmn ";
  }
  body += "zzrre ";
  for (int i = 0; i < 30; ++i) {
    body += "zzcmn ";
  }

  bench_sloppy::EuroparlBodyTemplate tpl;
  for (size_t d = 0; d < kSyntheticDocs; ++d) {
    tpl.SetColumn(2, body);
    auto trx = writer->GetBatch();
    auto inserter = trx.Insert();
    const auto& doc = tpl.Get();
    if (!inserter.Insert<irs::Action::INDEX>(doc.indexed.begin(),
                                             doc.indexed.end())) {
      Die("synthetic Insert returned false");
    }
  }
  writer->Commit();

  std::fprintf(stderr,
               "sloppy_phrase_vs_disjunction bench: indexed %zu synthetic "
               "documents (60:1 skew)\n",
               kSyntheticDocs);

  auto rdr = irs::DirectoryReader{*dir, format};
  return Corpus{.dir_path = std::move(tmp_root),
                .dir = std::move(dir),
                .format = std::move(format),
                .reader = std::move(rdr)};
}

const Corpus& GetCorpus() {
  static const Corpus corpus = BuildIndex();
  return corpus;
}

const Corpus& GetSyntheticCorpus() {
  static const Corpus corpus = BuildSyntheticIndex();
  return corpus;
}

irs::ByPhrase MakeSlopPhrase(std::string_view t0, std::string_view t1,
                             irs::PosAttr::value_t slop) {
  irs::ByPhrase q;
  *q.mutable_field() = kFieldName;
  q.mutable_options()->push_back<irs::ByTermOptions>().term =
    irs::ViewCast<irs::byte_type>(t0);
  q.mutable_options()->push_back<irs::ByTermOptions>().term =
    irs::ViewCast<irs::byte_type>(t1);
  q.mutable_options()->set_slop(slop);
  return q;
}

// push_back<ByTermOptions>(offs) sets offs_min=offs_max=offs+1, so to
// request position delta g pass offs=g-1.
void AppendFixedPhrase(irs::Or& or_filter, std::string_view first,
                       std::string_view second, irs::PosAttr::value_t gap) {
  SDB_ASSERT(gap >= 1);
  auto& phrase = or_filter.add<irs::ByPhrase>();
  *phrase.mutable_field() = kFieldName;
  phrase.mutable_options()->push_back<irs::ByTermOptions>().term =
    irs::ViewCast<irs::byte_type>(first);
  phrase.mutable_options()
    ->push_back<irs::ByTermOptions>(/*offs=*/gap - 1)
    .term = irs::ViewCast<irs::byte_type>(second);
}

// Builds the disjunction that matches the same chains as MakeSlopPhrase
// for two terms with expected step 1:
//   forward  phrases for gaps in [1, slop+1]
//   reversed phrases for gaps in [1, slop-1]
// Reversed phrases have cost = gap + 1, so the loop bound is slop-1.
irs::Or MakeDisjunctionEquivalent(std::string_view t0, std::string_view t1,
                                  irs::PosAttr::value_t slop) {
  irs::Or q;
  for (irs::PosAttr::value_t g = 1; g <= slop + 1; ++g) {
    AppendFixedPhrase(q, t0, t1, g);
  }
  for (irs::PosAttr::value_t g = 1; g + 1 <= slop; ++g) {
    AppendFixedPhrase(q, t1, t0, g);
  }
  return q;
}

// Tracks peak allocated bytes; reset between iterations.
struct MaxMemoryCounter final : irs::IResourceManager {
  void Reset() noexcept {
    current = 0;
    max = 0;
  }

  void Increase(size_t value) final {
    current += value;
    max = std::max(max, current);
  }

  void Decrease(size_t value) noexcept final { current -= value; }

  size_t current{0};
  size_t max{0};
};

template<typename MakeFn>
void BenchPrepare(benchmark::State& state, const irs::DirectoryReader& rdr,
                  MakeFn make) {
  {
    auto q = make();
    auto check = q.prepare({.index = rdr});
    if (!check) {
      state.SkipWithError("prepare returned null");
      return;
    }
  }

  MaxMemoryCounter counter;
  for (auto _ : state) {
    counter.Reset();
    auto q = make();
    auto prepared = q.prepare({.index = rdr, .memory = counter});
    benchmark::DoNotOptimize(prepared);
  }

  state.counters["prepare_mem_bytes"] = static_cast<double>(counter.max);
}

template<typename MakeFn>
[[gnu::noinline]] void BenchExecuteOnly(
  benchmark::State& state, const irs::DirectoryReader& rdr, MakeFn make,
  dp::GatherOverride mode = dp::GatherOverride::kAuto) {
  GatherModeGuard guard{mode};

  auto q = make();
  auto prepared = q.prepare({.index = rdr});
  if (!prepared) {
    state.SkipWithError("prepare returned null");
    return;
  }

  size_t per_iter = 0;
  for (auto _ : state) {
    per_iter = 0;
    for (const auto& sub : rdr) {
      auto docs = prepared->execute({.segment = sub});
      while (docs->next()) {
        ++per_iter;
      }
    }
    benchmark::DoNotOptimize(per_iter);
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(per_iter));
  state.counters["docs"] = static_cast<double>(per_iter);
}

// ExecuteWithOffsets is a FixedPhraseQuery method; slop only (no
// disjunction analogue). Drains pos->next() per matched doc.
void BenchExecuteWithOffsets(benchmark::State& state,
                             const irs::DirectoryReader& rdr,
                             std::string_view t0, std::string_view t1,
                             irs::PosAttr::value_t slop) {
  auto q = MakeSlopPhrase(t0, t1, slop);
  auto prepared = q.prepare({.index = rdr});
  if (!prepared) {
    state.SkipWithError("prepare returned null");
    return;
  }
  const auto* phrase_query =
    dynamic_cast<const irs::FixedPhraseQuery*>(prepared.get());
  if (!phrase_query) {
    state.SkipWithError("expected FixedPhraseQuery, got different prepared");
    return;
  }

  size_t docs_per_iter = 0;
  size_t matches_per_iter = 0;
  for (auto _ : state) {
    docs_per_iter = 0;
    matches_per_iter = 0;
    for (const auto& sub : rdr) {
      auto docs = phrase_query->ExecuteWithOffsets(sub);
      if (!docs) {
        continue;
      }
      auto* pos = irs::GetMutable<irs::PosAttr>(docs.get());
      if (!pos) {
        continue;
      }
      while (docs->next()) {
        ++docs_per_iter;
        while (pos->next()) {
          ++matches_per_iter;
        }
      }
    }
    benchmark::DoNotOptimize(docs_per_iter);
    benchmark::DoNotOptimize(matches_per_iter);
  }

  state.counters["docs"] = static_cast<double>(docs_per_iter);
  state.counters["matches"] = static_cast<double>(matches_per_iter);
}

// Registers the slop Execute benchmark under all three gather modes plus
// the disjunction baseline for one (corpus, term pair, slop).
void RegisterExecVariants(const std::string& suffix, std::string_view t0,
                          std::string_view t1, irs::PosAttr::value_t slop,
                          const Corpus& (*corpus)()) {
  for (const auto& m : kModes) {
    benchmark::RegisterBenchmark(
      ("SlopPhraseExec" + suffix + m.suffix).c_str(),
      [t0, t1, slop, mode = m.mode, corpus](benchmark::State& state) {
        BenchExecuteOnly(
          state, corpus().reader,
          [t0, t1, slop] { return MakeSlopPhrase(t0, t1, slop); }, mode);
      })
      ->Repetitions(kRepetitions)
      ->ReportAggregatesOnly(true);
  }

  benchmark::RegisterBenchmark(
    ("DisjunctionExec" + suffix).c_str(),
    [t0, t1, slop, corpus](benchmark::State& state) {
      BenchExecuteOnly(state, corpus().reader, [t0, t1, slop] {
        return MakeDisjunctionEquivalent(t0, t1, slop);
      });
    })
    ->Repetitions(kRepetitions)
    ->ReportAggregatesOnly(true);
}

void RegisterAll() {
  for (const auto& pair : kTermPairs) {
    for (auto slop : kSlopValues) {
      const std::string suffix = std::string{"_"} + std::string{pair.label} +
                                 "_slop" +
                                 std::to_string(static_cast<unsigned>(slop));

      benchmark::RegisterBenchmark(
        ("SlopPhrasePrepare" + suffix).c_str(),
        [t0 = pair.term0, t1 = pair.term1, slop](benchmark::State& state) {
          BenchPrepare(state, GetCorpus().reader,
                       [t0, t1, slop] { return MakeSlopPhrase(t0, t1, slop); });
        })
        ->Repetitions(kRepetitions)
        ->ReportAggregatesOnly(true);

      RegisterExecVariants(suffix, pair.term0, pair.term1, slop, &GetCorpus);

      benchmark::RegisterBenchmark(
        ("SlopPhraseExecOffs" + suffix).c_str(),
        [t0 = pair.term0, t1 = pair.term1, slop](benchmark::State& state) {
          BenchExecuteWithOffsets(state, GetCorpus().reader, t0, t1, slop);
        })
        ->Repetitions(kRepetitions)
        ->ReportAggregatesOnly(true);

      benchmark::RegisterBenchmark(
        ("DisjunctionPrepare" + suffix).c_str(),
        [t0 = pair.term0, t1 = pair.term1, slop](benchmark::State& state) {
          BenchPrepare(state, GetCorpus().reader, [t0, t1, slop] {
            return MakeDisjunctionEquivalent(t0, t1, slop);
          });
        })
        ->Repetitions(kRepetitions)
        ->ReportAggregatesOnly(true);
    }
  }

  // Synthetic 60:1 skew corpus: ceiling case for the seek-gather win.
  for (auto slop : kSlopValues) {
    const std::string suffix =
      "_synthetic_slop" + std::to_string(static_cast<unsigned>(slop));
    RegisterExecVariants(suffix, "zzcmn", "zzrre", slop, &GetSyntheticCorpus);
  }
}

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  RegisterAll();
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
