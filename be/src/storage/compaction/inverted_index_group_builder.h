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
#include "storage/rowset/rowset_fwd.h"

namespace doris {

// Turns a freshly built, not yet visible compaction output rowset into the segment group index
// layout: the SNII inverted and BKD indexes of consecutive segments are merged into
// {rowset_id}_g{n}.idx files, the per-segment index files are deleted and the rowset meta records
// the groups (RowsetMetaPB.inverted_index_group_segment_counts).
//
// Only a rowset whose every index can be grouped is converted (all-or-nothing, so no segment
// container has to be rewritten to drop part of its indexes); ANN indexes and indexes on variant
// subcolumns keep the rowset in the segment layout. A failed merge also leaves the segment layout:
// the group files written so far are removed, the warning is logged and compaction goes on, since
// the output is complete either way.
class InvertedIndexGroupBuilder {
public:
    // Greedy left-to-right cut of positions into groups: a group closes before it would exceed
    // max_group_bytes of summed per-segment index bytes or reach 2^32 rows. Every group holds at
    // least one segment. Example: rows [10,10,10], bytes [4,4,4], max 8 -> [2, 1].
    static std::vector<int32_t> plan_groups(const std::vector<uint32_t>& segment_rows,
                                            const std::vector<int64_t>& segment_index_bytes,
                                            int64_t max_group_bytes);

    // No-op unless config::enable_inverted_index_segment_group is on, the BE is not in cloud
    // mode, the rowset uses SNII and has at least two segments.
    static Status build(const RowsetSharedPtr& rowset, int64_t tablet_id);
};

} // namespace doris
