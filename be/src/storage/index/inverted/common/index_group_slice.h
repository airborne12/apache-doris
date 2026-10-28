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
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace roaring {
class Roaring;
} // namespace roaring

namespace doris {
class QueryContext;
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

// Group-space index results of running queries: every segment of a group, in every scanner of
// the query, slices the same evaluation instead of re-running it over the whole group. An entry
// lives as long as its query (it holds a weak reference to the QueryContext); entries of finished
// queries are dropped on later inserts.
//
// Deliberately not the InvertedIndexQueryCache: that LRU-K cache refuses the first insert of a key
// once it is full, so the second segment of a group would miss and run the group again, and it
// would be written even when the session disabled the query cache.
// Example key: "<query id>|.../{rowset}_g0_10002|<column>|<query semantic>".
class QueryGroupResults {
public:
    static QueryGroupResults* instance();

    // `query` is the current query's context, `key` must already name the query (see make_key).
    std::shared_ptr<roaring::Roaring> lookup(const std::string& key);
    void insert(const std::string& key, const QueryContext* query,
                std::shared_ptr<roaring::Roaring> bitmap);

    static std::string make_key(const TUniqueId& query_id, std::string_view semantic_key);

private:
    struct Entry {
        std::weak_ptr<const QueryContext> owner;
        std::shared_ptr<roaring::Roaring> bitmap;
    };
    std::mutex _lock;
    std::unordered_map<std::string, Entry> _entries;
    size_t _purge_at = 64;
};

} // namespace doris::segment_v2
