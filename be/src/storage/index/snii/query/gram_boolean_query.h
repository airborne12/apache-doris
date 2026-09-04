// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cstdint>
#include <roaring/roaring.hh>
#include <string_view>

#include "common/status.h"
#include "storage/index/inverted/gram/gram_query.h"
#include "storage/index/snii/reader/logical_index_reader.h"

// gram_boolean_query -- evaluates a gram::GramQuery boolean tree against one SNII
// segment's gram-family dictionary/postings, producing a docid bitmap. The index
// only ever narrows the candidate set: a missing gram, an unsupported shape, or a
// lookup failure must fall back to "no acceleration" upstream, never to a changed
// result, so every codepath here returns a Status instead of asserting presence.
namespace doris::snii::query {

// Posting data source consumed by gram_boolean_query(). Production code adapts a
// LogicalIndexReader (LogicalIndexPostingSource below); tests inject a map-backed
// fake so the AND/OR/ALL/NONE evaluation logic is covered without building a real
// index file.
class GramPostingSource {
public:
    virtual ~GramPostingSource() = default;
    // Looks up one gram's document frequency. found=false means the gram is absent
    // from the dictionary, which makes any AND node containing it evaluate to NONE
    // (empty) without ever reading a posting list.
    virtual Status df(std::string_view gram, bool* found, uint64_t* df) = 0;
    // Decodes one gram's full docid set (no positions/frequencies) into out. Only
    // called after df() has confirmed the gram exists.
    virtual Status postings(std::string_view gram, roaring::Roaring* out) = 0;
};

// Production GramPostingSource backed by a LogicalIndexReader: df() is a plain
// dictionary lookup, postings() additionally decodes the docid-only posting.
class LogicalIndexPostingSource final : public GramPostingSource {
public:
    explicit LogicalIndexPostingSource(const reader::LogicalIndexReader& idx) : _idx(idx) {}
    Status df(std::string_view gram, bool* found, uint64_t* df) override;
    Status postings(std::string_view gram, roaring::Roaring* out) override;

private:
    const reader::LogicalIndexReader& _idx;
};

// Evaluates q against src: ALL -> [0, num_docs); NONE -> empty; AND queries df for
// every direct gram leaf first (a missing leaf short-circuits to empty without any
// posting read), sorts the remaining leaves by ascending df, and intersects them
// with early exit on an empty accumulator, then intersects every sub-query the same
// way (an AND with neither leaves nor sub-queries degenerates to ALL); OR unions
// every leaf's postings and every sub-query's result. Recursion depth is bounded by
// the tree GramQuery::parse produced (capped at construction time).
Status gram_boolean_query(GramPostingSource& src, const segment_v2::gram::GramQuery& q,
                          uint32_t num_docs, roaring::Roaring* out);

} // namespace doris::snii::query
