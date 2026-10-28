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
#include <string>
#include <string_view>

namespace roaring {
class Roaring;
} // namespace roaring

namespace doris {
class TUniqueId;
} // namespace doris

namespace doris::segment_v2 {

// The rows of one segment inside the docid space of its segment group index
// (see RowsetMetaPB.inverted_index_group_segment_counts): group docids [offset, offset + rows).
// Example: offset 10, rows 20 -> group docid 14 is segment rowid 4.
struct IndexGroupSlice {
    uint32_t offset = 0;
    uint32_t rows = 0;
    uint32_t group_rows = 0;

    // Group-space bitmap -> this segment's rowids.
    roaring::Roaring to_segment(const roaring::Roaring& group_bitmap) const;
    bool contains(uint32_t group_docid) const {
        return group_docid >= offset && group_docid - offset < rows;
    }
    // Distinguishes per-segment derived cache entries of one group file.
    std::string cache_suffix() const;
};

// Index path of the query cache entry holding a group-space result for ONE query: every segment
// of the group, in every scanner of that query, slices the same evaluation instead of re-running
// it over the whole group. The query id keeps later queries from ever hitting the entry, so it is
// written whether or not the session enables the query cache; unused entries age out of the LRU.
// Example: ".../{rowset}_g0_10002" + "@q:5f3a-..." for query 5f3a-...
std::string query_scoped_group_path(std::string_view index_file_key, const TUniqueId& query_id);

} // namespace doris::segment_v2
