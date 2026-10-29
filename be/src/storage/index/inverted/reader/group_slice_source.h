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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/index/inverted/common/index_group_slice.h"
#include "storage/index/inverted/query/spi/index_source.h"

namespace doris::segment_v2 {

// One segment's view of a segment group index source, in the segment's own rowids, for engines
// that read a source directly (SEARCH) instead of through the reader's group run.
//
// Every docid crossing the interface is translated: postings list only the slice's documents,
// shifted by -offset, and docids passed in (norms, liveness) are shifted by +offset. Dictionary
// facts (expansions, doc_freq, scoring stats) stay the group's, so a local idf keeps doc_count
// and doc_freq in one domain.
// Example: offset 10, rows 20, group posting {3, 12, 29, 31} -> this segment lists {2, 19}.
class GroupSliceSource final : public index_query::IndexSource {
public:
    GroupSliceSource(index_query::IndexSourcePtr inner, const IndexGroupSlice& slice);

    uint32_t doc_count() const override { return _slice.rows; }
    Status scoring_stats(index_query::IndexScoringStats* out) const override;
    Status prepare_terms(std::span<const std::string> terms) override;
    Status open_term(std::string_view term, bool positions, bool scoring,
                     std::unique_ptr<index_query::PostingsCursor>* out) override;
    Status doc_freq(std::string_view term, uint64_t* out) override;
    Status scoring_doc_freq(std::string_view term, uint64_t* out) override;
    Status scoring_doc_freq_among(std::string_view term, std::span<const uint32_t> docs,
                                  uint64_t* out) override;
    Status expand_terms(index_query::TermPattern& pattern, int32_t max_expansions,
                        std::vector<std::string>* out) override;
    std::span<const float> norm_lengths() const override;
    Status encoded_norms(std::span<const uint32_t> docs, std::vector<uint32_t>* out) override;
    Status may_hold(std::string_view term, bool* held) override;
    bool is_live(uint32_t doc) const override;

private:
    std::vector<uint32_t> _to_group(std::span<const uint32_t> docs) const;

    index_query::IndexSourcePtr _inner;
    IndexGroupSlice _slice;
};

} // namespace doris::segment_v2
