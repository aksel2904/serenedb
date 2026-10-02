#!/usr/bin/env python3
"""CPU shares by category from `perf script -F comm,ip,sym --no-inline`.

Each sample goes to the first rule whose condition holds for its stack
(any frame, leaf to root):
  matcher       LikeMatcher:: or re2:: frame
  verify_call   ColumnSegment::FilterSelection frame (the per-row call of
                sdb_wildcard_ngram_verify: expression executor, the scalar
                function, MatchStoredTerms), rest of it
  decode        duckdb_fsst_decompress, dict_fsst ReconstructEntry,
                ScanToFlatVector, Select*, GetSelVec, UnpackCodes frame
  init          ColumnSegment::InitializeScan, StringInitScan,
                CompressedStringScanState::Initialize frame (bit unpacking,
                allocation and the rest of block scan setup)
  point_read    ColumnReader::PointReader::FetchRow frame (point reads of
                the inline path), rest of it
  gather        ColumnReader::Gather*, ColumnReader::ScanVector frame
  table_other   ColFilterChain:: or ColFilterVerify:: frame
  candidates    IResearchScanFunction frame (n-gram postings, conjunction,
                walk over candidates)
  rest          everything else (parse, plan, network, other threads)
"""

import collections
import sys

RULES = [
    ("matcher", ("irs::LikeMatcher::", "re2::")),
    ("verify_call", ("duckdb::ColumnSegment::FilterSelection(",)),
    ("decode", ("duckdb_fsst_decompress", "dict_fsst::CompressedStringScanState::ReconstructEntry(",
                "dict_fsst::CompressedStringScanState::ScanToFlatVector(",
                "dict_fsst::CompressedStringScanState::Select", "dict_fsst::CompressedStringScanState::GetSelVec(",
                "dict_fsst::CompressedStringScanState::UnpackCodes(")),
    ("init", ("duckdb::ColumnSegment::InitializeScan(", "DictFSSTCompressionStorage::StringInitScan(",
              "dict_fsst::CompressedStringScanState::Initialize(")),
    ("point_read", ("irs::ColumnReader::PointReader::FetchRow(",)),
    ("gather", ("irs::ColumnReader::Gather", "irs::ColumnReader::ScanVector(")),
    ("table_other", ("irs::ColFilterChain::", "sdb::connector::ColFilterVerify::")),
    ("candidates", ("sdb::connector::IResearchScanFunction(",)),
]
CATS = [r[0] for r in RULES] + ["rest"]


def classify(frames):
    for cat, pats in RULES:
        for f in frames:
            if any(p in f for p in pats):
                return cat
    return "rest"


def main():
    counts = collections.Counter()
    frames = []
    total = 0
    for line in sys.stdin:
        if line.strip() == "":
            if frames:
                counts[classify(frames)] += 1
                total += 1
                frames = []
            continue
        if line.startswith((" ", "\t")):
            frames.append(line.strip())
    if frames:
        counts[classify(frames)] += 1
        total += 1
    print("samples\t" + "\t".join(CATS))
    print(f"{total}\t" + "\t".join(f"{100.0 * counts[c] / total:.1f}" for c in CATS))


if __name__ == "__main__":
    main()
