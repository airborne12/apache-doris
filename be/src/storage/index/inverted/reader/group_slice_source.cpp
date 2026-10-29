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

#include "storage/index/inverted/reader/group_slice_source.h"

#include <algorithm>
#include <utility>

#include "common/check.h"
#include "storage/index/inverted/query/spi/position_cursor.h"
#include "storage/index/inverted/query/spi/postings_cursor.h"

namespace doris::segment_v2 {

namespace {

using index_query::BlockBound;
using index_query::PositionCursor;
using index_query::PostingsBlock;
using index_query::PostingsCursor;

// A slice's postings read once into memory and served in fixed blocks. Reading them up front
// costs what a per-segment index costs for the same segment: the inner cursor seeks to the
// slice's first window and stops after its last one, so the other segments' windows are not
// decoded.
// Example: kBlock 128, 300 docs in the slice -> blocks [0,128) [128,256) [256,300).
class SlicePostingsCursor final : public PostingsCursor, public PositionCursor {
public:
    static constexpr size_t kBlock = 128;

    static Status read(PostingsCursor& inner, const IndexGroupSlice& slice, bool positions,
                       bool scoring, std::unique_ptr<PostingsCursor>* out) {
        auto cursor = std::unique_ptr<SlicePostingsCursor>(new SlicePostingsCursor());
        cursor->_positions = positions;
        cursor->_scoring = scoring;
        const uint32_t lo = slice.offset;
        const uint64_t hi = static_cast<uint64_t>(slice.offset) + slice.rows;
        PostingsBlock block;
        bool eof = false;
        RETURN_IF_ERROR(inner.seek_block(lo, &block, &eof));
        std::vector<uint32_t> ordinals;
        index_query::PositionsBuffer buffer;
        index_query::BlockPositions block_positions;
        if (positions) {
            cursor->_pos_off.push_back(0);
        }
        while (!eof) {
            ordinals.clear();
            bool past = false;
            for (uint64_t i = 0; i < block.size(); ++i) {
                const uint32_t doc = block.doc_at(i);
                if (doc < lo) {
                    continue;
                }
                if (doc >= hi) {
                    past = true;
                    break;
                }
                ordinals.push_back(static_cast<uint32_t>(i));
                cursor->_docs.push_back(doc - lo);
                if (scoring) {
                    cursor->_freqs.push_back(block.freq_at(i));
                    cursor->_norms.push_back(block.norm_at(i));
                }
            }
            if (positions && !ordinals.empty()) {
                RETURN_IF_ERROR(inner.block_positions(ordinals, &buffer, &block_positions));
                for (size_t k = 0; k < ordinals.size(); ++k) {
                    const auto of = block_positions.of(k, ordinals);
                    cursor->_pos_flat.insert(cursor->_pos_flat.end(), of.begin(), of.end());
                    cursor->_pos_off.push_back(static_cast<uint32_t>(cursor->_pos_flat.size()));
                }
            }
            if (past) {
                break;
            }
            RETURN_IF_ERROR(inner.next_block(&block, &eof));
        }
        cursor->_blocks = (cursor->_docs.size() + kBlock - 1) / kBlock;
        *out = std::move(cursor);
        return Status::OK();
    }

    uint32_t doc_freq() const override { return static_cast<uint32_t>(_docs.size()); }

    Status next_block(PostingsBlock* block, bool* eof) override {
        *block = {};
        *eof = _next >= _blocks;
        if (*eof) {
            _current = kNone;
            return Status::OK();
        }
        _current = _next++;
        const size_t begin = _current * kBlock;
        const size_t size = std::min(kBlock, _docs.size() - begin);
        block->docs = std::span<const uint32_t>(_docs).subspan(begin, size);
        if (_scoring) {
            block->freqs = std::span<const uint32_t>(_freqs).subspan(begin, size);
            block->norms = std::span<const uint32_t>(_norms).subspan(begin, size);
        }
        return Status::OK();
    }

    Status seek_block(uint32_t target, PostingsBlock* block, bool* eof) override {
        _next = _block_of(target);
        return next_block(block, eof);
    }

    Status shallow_seek(uint32_t target, bool* moved) override {
        *moved = false;
        if (_current != kNone && target <= _last_doc(_current)) {
            return Status::OK();
        }
        const size_t next = _block_of(target);
        if (_current == kNone && next == _next) {
            return Status::OK();
        }
        _next = next;
        _current = kNone;
        *moved = true;
        return Status::OK();
    }

    BlockBound current_block_bound() const override {
        const size_t block = _current != kNone ? _current : _next;
        if (block >= _blocks) {
            return {};
        }
        return {.last_doc = _last_doc(block), .last_doc_known = true};
    }

    Status rewind() override {
        _next = 0;
        _current = kNone;
        return Status::OK();
    }

    Status open_positions(uint32_t ordinal, PositionCursor** out) override {
        *out = nullptr;
        if (!_positions) {
            return Status::NotSupported("This posting type does not support positions");
        }
        DORIS_CHECK(_current != kNone);
        _position_doc = _current * kBlock + ordinal;
        DORIS_CHECK(_position_doc < _docs.size());
        _next_position = _pos_off[_position_doc];
        *out = this;
        return Status::OK();
    }

    Status next_position(uint32_t* position, bool* available) override {
        *available = _next_position < _pos_off[_position_doc + 1];
        if (*available) {
            *position = _pos_flat[_next_position++];
        }
        return Status::OK();
    }

    Status finish_doc() override { return Status::OK(); }

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);

    SlicePostingsCursor() = default;

    uint32_t _last_doc(size_t block) const {
        return _docs[std::min(_docs.size(), (block + 1) * kBlock) - 1];
    }

    // The first block whose last doc is at least `target`; _blocks when every doc is below it.
    size_t _block_of(uint32_t target) const {
        const auto it = std::lower_bound(_docs.begin(), _docs.end(), target);
        // Past the last doc the index is size(), which divides into the last partial block:
        // 300 docs, target past them -> 300 / 128 = block 2, so it must map to _blocks instead.
        if (it == _docs.end()) {
            return _blocks;
        }
        return static_cast<size_t>(it - _docs.begin()) / kBlock;
    }

    std::vector<uint32_t> _docs;
    std::vector<uint32_t> _freqs;
    std::vector<uint32_t> _norms;
    std::vector<uint32_t> _pos_flat;
    std::vector<uint32_t> _pos_off;
    bool _positions = false;
    bool _scoring = false;
    size_t _blocks = 0;
    size_t _next = 0;
    size_t _current = kNone;
    size_t _position_doc = 0;
    uint32_t _next_position = 0;
};

} // namespace

GroupSliceSource::GroupSliceSource(index_query::IndexSourcePtr inner, const IndexGroupSlice& slice)
        : _inner(std::move(inner)), _slice(slice) {
    DORIS_CHECK(_inner != nullptr);
}

Status GroupSliceSource::scoring_stats(index_query::IndexScoringStats* out) const {
    return _inner->scoring_stats(out);
}

Status GroupSliceSource::prepare_terms(std::span<const std::string> terms) {
    return _inner->prepare_terms(terms);
}

Status GroupSliceSource::open_term(std::string_view term, bool positions, bool scoring,
                                   std::unique_ptr<index_query::PostingsCursor>* out) {
    std::unique_ptr<PostingsCursor> inner;
    RETURN_IF_ERROR(_inner->open_term(term, positions, scoring, &inner));
    if (inner == nullptr) {
        *out = nullptr;
        return Status::OK();
    }
    return SlicePostingsCursor::read(*inner, _slice, positions, scoring, out);
}

Status GroupSliceSource::doc_freq(std::string_view term, uint64_t* out) {
    return _inner->doc_freq(term, out);
}

Status GroupSliceSource::scoring_doc_freq(std::string_view term, uint64_t* out) {
    return _inner->scoring_doc_freq(term, out);
}

Status GroupSliceSource::scoring_doc_freq_among(std::string_view term,
                                                std::span<const uint32_t> docs, uint64_t* out) {
    return _inner->scoring_doc_freq_among(term, _to_group(docs), out);
}

Status GroupSliceSource::expand_terms(index_query::TermPattern& pattern, int32_t max_expansions,
                                      std::vector<std::string>* out) {
    return _inner->expand_terms(pattern, max_expansions, out);
}

std::span<const float> GroupSliceSource::norm_lengths() const {
    return _inner->norm_lengths();
}

Status GroupSliceSource::encoded_norms(std::span<const uint32_t> docs, std::vector<uint32_t>* out) {
    return _inner->encoded_norms(_to_group(docs), out);
}

Status GroupSliceSource::may_hold(std::string_view term, bool* held) {
    return _inner->may_hold(term, held);
}

bool GroupSliceSource::is_live(uint32_t doc) const {
    return _inner->is_live(doc + _slice.offset);
}

std::vector<uint32_t> GroupSliceSource::_to_group(std::span<const uint32_t> docs) const {
    std::vector<uint32_t> group(docs.begin(), docs.end());
    for (uint32_t& doc : group) {
        DCHECK_LT(doc, _slice.rows);
        doc += _slice.offset;
    }
    return group;
}

} // namespace doris::segment_v2
