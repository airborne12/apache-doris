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

#include <roaring/roaring.hh>
#include <vector>

#include "util/uid_util.h"

namespace doris::segment_v2 {

roaring::Roaring IndexGroupSlice::to_segment(const roaring::Roaring& group_bitmap) const {
    roaring::Roaring out;
    if (rows == 0) {
        return out;
    }
    std::vector<uint32_t> buffer;
    buffer.reserve(1024);
    auto it = group_bitmap.begin();
    it.equalorlarger(offset);
    const uint64_t end = static_cast<uint64_t>(offset) + rows;
    for (; it != group_bitmap.end() && *it < end; ++it) {
        buffer.push_back(*it - offset);
        if (buffer.size() == 1024) {
            out.addMany(buffer.size(), buffer.data());
            buffer.clear();
        }
    }
    out.addMany(buffer.size(), buffer.data());
    out.runOptimize();
    return out;
}

std::string IndexGroupSlice::cache_suffix() const {
    return fmt::format("@slice:{}:{}", offset, rows);
}

std::string query_scoped_group_path(std::string_view index_file_key, const TUniqueId& query_id) {
    return fmt::format("{}@q:{}", index_file_key, print_id(query_id));
}

} // namespace doris::segment_v2
