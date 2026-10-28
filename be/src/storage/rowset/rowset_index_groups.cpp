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

#include "storage/rowset/rowset_index_groups.h"

#include <limits>

#include "common/check.h"

namespace doris {

RowsetIndexGroups::RowsetIndexGroups(const std::vector<int32_t>& segment_counts,
                                     const std::vector<uint32_t>& segment_rows)
        : _segment_counts(segment_counts) {
    DORIS_CHECK(!segment_counts.empty());
    _group_of_pos.reserve(segment_rows.size());
    _offset_of_pos.reserve(segment_rows.size());
    int64_t pos = 0;
    for (int32_t group_id = 0; group_id < static_cast<int32_t>(segment_counts.size()); ++group_id) {
        DORIS_CHECK_GT(segment_counts[group_id], 0);
        _first_pos.push_back(pos);
        // A group's docids are uint32 and every bitmap over them is a 32-bit roaring, so a group
        // must hold fewer than 2^32 rows; the writer cuts groups to respect this.
        uint64_t rows = 0;
        for (int32_t i = 0; i < segment_counts[group_id]; ++i, ++pos) {
            DORIS_CHECK_LT(pos, static_cast<int64_t>(segment_rows.size()));
            _group_of_pos.push_back(group_id);
            _offset_of_pos.push_back(static_cast<uint32_t>(rows));
            rows += segment_rows[pos];
        }
        DORIS_CHECK_LT(rows, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
        _group_rows.push_back(static_cast<uint32_t>(rows));
    }
    DORIS_CHECK_EQ(pos, static_cast<int64_t>(segment_rows.size()));
}

} // namespace doris
