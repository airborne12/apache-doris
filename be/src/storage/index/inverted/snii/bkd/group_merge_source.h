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
#include <vector>

#include "common/status.h"
#include "storage/index/inverted/snii/bkd/bkd_reader.h"
#include "storage/index/inverted/snii/bkd/leaf_codec.h"
#include "storage/index/inverted/snii/bkd/point_source.h"
#include "storage/index/inverted/snii/common/slice.h"

namespace doris::snii::bkd {

// The point stream of a segment group BKD index: the k-way merge of the sealed per-segment
// indexes, each of which is already one run ordered by (value, doc_id).
//
// Source i contributes its points with doc_id + doc_offsets[i]. Offsets ascend with the source
// order and every source's doc ids stay below the next offset, so for equal values the merged
// (value, doc_id) order is the same as the order a single build over the concatenated rows
// would produce. Example: sources {(5,0),(9,1)} offset 0 and {(5,0),(7,1)} offset 2 merge to
// (5,0) (5,2) (7,3) (9,1).
//
// Holds one decoded leaf per source, so memory is K leaves whatever the group's row count.
class GroupBkdMergeSource final : public PointSource {
public:
    // Readers are borrowed and must outlive this object; all share bytes_per_dim.
    GroupBkdMergeSource(std::vector<const BkdReader*> sources, std::vector<uint32_t> doc_offsets,
                        uint32_t bytes_per_dim);

    Status next_block(uint32_t max_points, Slice* records) override;

private:
    struct Cursor {
        const BkdReader* reader = nullptr;
        uint32_t doc_offset = 0;
        uint32_t next_leaf = 0;
        std::vector<uint8_t> leaf_bytes;
        DecodedLeafBlock leaf;
        uint32_t point = 0; // next point in `leaf`
        uint32_t run = 0;   // run holding `point`
        // The current point's full record [value][doc_id + doc_offset, big endian].
        std::vector<uint8_t> record;
        bool exhausted = false;
    };

    // Positions `cursor` on its next point, loading the next leaf when the current one is used up.
    Status advance(Cursor* cursor);
    void build_record(Cursor* cursor) const;
    // Index of the cursor holding the smallest record, or -1 when all are exhausted.
    int smallest() const;

    const uint32_t _bytes_per_dim;
    const size_t _record_size;
    std::vector<Cursor> _cursors;
    bool _started = false;
    std::vector<uint8_t> _block;
};

} // namespace doris::snii::bkd
