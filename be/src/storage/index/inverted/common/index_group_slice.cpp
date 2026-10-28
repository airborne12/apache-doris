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

#include "storage/index/inverted/common/index_group_slice.h"

#include <fmt/format.h>

#include <algorithm>
#include <roaring/roaring.hh>

#include "common/check.h"
#include "runtime/query_context.h"
#include "util/uid_util.h"

namespace doris::segment_v2 {

// Container-level, not per value: the intersection with the slice's range and the shift both work
// on whole 2^16-value containers, so slicing a dense filter bitmap (status = 'error' over a whole
// group) costs about its container count, not its cardinality, for every segment of the group.
roaring::Roaring IndexGroupSlice::to_segment(const roaring::Roaring& group_bitmap) const {
    roaring::Roaring range;
    range.addRange(offset, static_cast<uint64_t>(offset) + rows);
    range &= group_bitmap;
    roaring::Roaring out(
            roaring::api::roaring_bitmap_add_offset(&range.roaring, -static_cast<int64_t>(offset)));
    out.runOptimize();
    return out;
}

std::string IndexGroupSlice::cache_suffix() const {
    return fmt::format("@slice:{}:{}", offset, rows);
}

QueryGroupResults* QueryGroupResults::instance() {
    static QueryGroupResults results;
    return &results;
}

std::string QueryGroupResults::make_key(const TUniqueId& query_id, std::string_view semantic_key) {
    return fmt::format("{}|{}", print_id(query_id), semantic_key);
}

std::shared_ptr<roaring::Roaring> QueryGroupResults::lookup(const std::string& key) {
    std::lock_guard<std::mutex> guard(_lock);
    auto it = _entries.find(key);
    if (it == _entries.end() || it->second.owner.expired()) {
        return nullptr;
    }
    return it->second.bitmap;
}

void QueryGroupResults::insert(const std::string& key, const QueryContext* query,
                               std::shared_ptr<roaring::Roaring> bitmap) {
    DORIS_CHECK(query != nullptr);
    std::lock_guard<std::mutex> guard(_lock);
    // Amortized cleanup: entries of finished queries are dropped whenever the table has doubled
    // since the last sweep, so its size stays proportional to the results of running queries.
    if (_entries.size() >= _purge_at) {
        std::erase_if(_entries, [](const auto& entry) { return entry.second.owner.expired(); });
        _purge_at = std::max<size_t>(64, _entries.size() * 2);
    }
    _entries[key] = Entry {.owner = query->weak_from_this(), .bitmap = std::move(bitmap)};
}

} // namespace doris::segment_v2
