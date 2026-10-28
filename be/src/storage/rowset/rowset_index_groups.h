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

namespace doris {

// Where each segment position of a grouped rowset sits inside its segment group index.
//
// A group covers consecutive positions; its docid space is the concatenation of those segments'
// rows. Example: segment_counts [3, 2] with segment rows [10, 20, 5, 7, 8] gives
//   pos:     0   1   2   3   4
//   group:   0   0   0   1   1
//   offset:  0  10  30   0   7
// and group rows [35, 15]. Row 4 of position 1 is docid 14 of group 0.
class RowsetIndexGroups {
public:
    // segment_counts: RowsetMetaPB.inverted_index_group_segment_counts (non-empty).
    // segment_rows: rows of every position, num_segments entries.
    RowsetIndexGroups(const std::vector<int32_t>& segment_counts,
                      const std::vector<uint32_t>& segment_rows);

    int32_t group_count() const { return static_cast<int32_t>(_group_rows.size()); }
    int32_t group_of(int64_t pos) const { return _group_of_pos[pos]; }
    uint32_t offset_of(int64_t pos) const { return _offset_of_pos[pos]; }
    uint32_t group_rows(int32_t group_id) const { return _group_rows[group_id]; }
    int64_t first_pos(int32_t group_id) const { return _first_pos[group_id]; }
    int32_t segment_count(int32_t group_id) const { return _segment_counts[group_id]; }

private:
    std::vector<int32_t> _segment_counts;
    std::vector<int32_t> _group_of_pos;
    std::vector<uint32_t> _offset_of_pos;
    std::vector<uint32_t> _group_rows;
    std::vector<int64_t> _first_pos;
};

} // namespace doris
