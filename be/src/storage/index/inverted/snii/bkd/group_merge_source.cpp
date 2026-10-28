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

#include "storage/index/inverted/snii/bkd/group_merge_source.h"

#include <algorithm>
#include <cstring>

#include "common/check.h"
#include "storage/index/inverted/snii/bkd/bkd_format.h"

namespace doris::snii::bkd {

GroupBkdMergeSource::GroupBkdMergeSource(std::vector<const BkdReader*> sources,
                                         std::vector<uint32_t> doc_offsets, uint32_t bytes_per_dim)
        : _bytes_per_dim(bytes_per_dim), _record_size(bytes_per_dim + kPointDocIdBytes) {
    DORIS_CHECK_EQ(sources.size(), doc_offsets.size());
    _cursors.resize(sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
        DORIS_CHECK(sources[i] != nullptr);
        DORIS_CHECK(sources[i]->empty() || sources[i]->header().bytes_per_dim == bytes_per_dim);
        _cursors[i].reader = sources[i];
        _cursors[i].doc_offset = doc_offsets[i];
        _cursors[i].record.resize(_record_size);
    }
}

void GroupBkdMergeSource::build_record(Cursor* cursor) const {
    const DecodedLeafBlock& leaf = cursor->leaf;
    while (cursor->point >= leaf.runs[cursor->run].first_point + leaf.runs[cursor->run].count) {
        ++cursor->run;
    }
    uint8_t* out = cursor->record.data();
    std::memcpy(out, leaf.common_prefix.data(), leaf.common_prefix.size());
    const Slice suffix = leaf.runs[cursor->run].suffix;
    std::memcpy(out + leaf.common_prefix.size(), suffix.data(), suffix.size());
    const uint32_t doc_id = leaf.doc_ids[cursor->point] + cursor->doc_offset;
    for (uint32_t i = 0; i < kPointDocIdBytes; ++i) {
        out[_bytes_per_dim + i] = static_cast<uint8_t>(doc_id >> (8 * (kPointDocIdBytes - 1 - i)));
    }
}

Status GroupBkdMergeSource::advance(Cursor* cursor) {
    if (cursor->exhausted) {
        return Status::OK();
    }
    if (cursor->next_leaf > 0) {
        ++cursor->point;
    }
    while (cursor->next_leaf == 0 || cursor->point >= cursor->leaf.point_count) {
        if (cursor->reader->empty() || cursor->next_leaf >= cursor->reader->leaf_count()) {
            cursor->exhausted = true;
            return Status::OK();
        }
        RETURN_IF_ERROR(
                cursor->reader->decode_leaf(cursor->next_leaf, &cursor->leaf_bytes, &cursor->leaf));
        ++cursor->next_leaf;
        cursor->point = 0;
        cursor->run = 0;
    }
    build_record(cursor);
    return Status::OK();
}

bool GroupBkdMergeSource::greater(int a, int b) const {
    return std::memcmp(_cursors[a].record.data(), _cursors[b].record.data(), _record_size) > 0;
}

Status GroupBkdMergeSource::next_block(uint32_t max_points, Slice* records) {
    const auto cmp = [this](int a, int b) { return greater(a, b); };
    if (!_started) {
        _started = true;
        for (int i = 0; i < static_cast<int>(_cursors.size()); ++i) {
            RETURN_IF_ERROR(advance(&_cursors[i]));
            if (!_cursors[i].exhausted) {
                _heap.push_back(i);
            }
        }
        std::make_heap(_heap.begin(), _heap.end(), cmp);
    }
    _block.clear();
    // A min-heap of the sources by current record: log K comparisons per point. Segment values
    // interleave (a column's values are spread over every segment), so the smallest source
    // changes almost every point and a linear scan would pay K comparisons each time; with 33
    // segments and 146M points that scan dominated the merge.
    while (!_heap.empty() && _block.size() < static_cast<size_t>(max_points) * _record_size) {
        std::pop_heap(_heap.begin(), _heap.end(), cmp);
        Cursor& cursor = _cursors[_heap.back()];
        _block.insert(_block.end(), cursor.record.begin(), cursor.record.end());
        RETURN_IF_ERROR(advance(&cursor));
        if (cursor.exhausted) {
            _heap.pop_back();
        } else {
            std::push_heap(_heap.begin(), _heap.end(), cmp);
        }
    }
    *records = Slice(_block);
    return Status::OK();
}

} // namespace doris::snii::bkd
