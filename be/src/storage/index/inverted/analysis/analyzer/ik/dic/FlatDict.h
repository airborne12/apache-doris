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

#include <algorithm>
#include <cstdint>
#include <vector>

namespace doris::segment_v2 {

class DictSegment;

// Analyzer rewrite spike, not for merging. A read-only breadth-first copy of a DictSegment tree:
// a node's children are adjacent and sorted, and the root is a direct table over the BMP.
class FlatDict {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFU;

    void build(const DictSegment& root);

    uint32_t child(uint32_t node, int32_t ch) const {
        if (node == 0 && static_cast<uint32_t>(ch) < 0x10000U) {
            return _root[static_cast<uint32_t>(ch)] - 1;
        }
        const Node& parent = _nodes[node];
        const Node* first = _nodes.data() + parent.child_begin;
        const uint32_t count = parent.child_count;
        if (count <= 8) {
            for (uint32_t i = 0; i < count; ++i) {
                if (first[i].key == ch) {
                    return parent.child_begin + i;
                }
            }
            return kNone;
        }
        const Node* last = first + count;
        const Node* found = std::lower_bound(
                first, last, ch, [](const Node& entry, int32_t key) { return entry.key < key; });
        if (found == last || found->key != ch) {
            return kNone;
        }
        return static_cast<uint32_t>(found - _nodes.data());
    }

    bool is_word(uint32_t node) const { return _nodes[node].word_end != 0; }
    bool has_children(uint32_t node) const { return _nodes[node].child_count != 0; }

private:
    struct Node {
        int32_t key;
        uint32_t child_begin;
        uint32_t child_count : 31;
        uint32_t word_end : 1;
    };

    std::vector<uint32_t> _root; // 0x10000 entries, node + 1, 0 when missing
    std::vector<Node> _nodes;
};

} // namespace doris::segment_v2
