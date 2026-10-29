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

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "storage/index/inverted/query/spi/fake_index_source.h"

namespace doris::segment_v2 {

using index_query::PositionCursor;
using index_query::PostingsBlock;
using index_query::PostingsCursor;
using index_query::testing::FakeIndexSource;

namespace {

std::vector<uint32_t> all_docs(PostingsCursor& cursor) {
    std::vector<uint32_t> docs;
    PostingsBlock block;
    bool eof = false;
    EXPECT_TRUE(cursor.next_block(&block, &eof).ok());
    while (!eof) {
        for (uint64_t i = 0; i < block.size(); ++i) {
            docs.push_back(block.doc_at(i));
        }
        EXPECT_TRUE(cursor.next_block(&block, &eof).ok());
    }
    return docs;
}

} // namespace

// Postings list only the slice's documents, in segment rowids; the dictionary stays the group's.
TEST(GroupSliceSourceTest, ListsTheSliceInSegmentRowids) {
    auto inner = std::make_shared<FakeIndexSource>();
    inner->set_doc_count(100);
    inner->add("a", {3, 12, 29, 31});
    inner->add("b", {50});
    GroupSliceSource source(inner, {.offset = 10, .rows = 20, .group_rows = 100});
    EXPECT_EQ(source.doc_count(), 20);

    std::unique_ptr<PostingsCursor> cursor;
    ASSERT_TRUE(source.open_term("a", false, false, &cursor).ok());
    ASSERT_NE(cursor, nullptr);
    EXPECT_EQ(cursor->doc_freq(), 2);
    EXPECT_EQ(all_docs(*cursor), (std::vector<uint32_t> {2, 19}));

    // A term held only by other segments lists nothing here, but stays in the dictionary.
    ASSERT_TRUE(source.open_term("b", false, false, &cursor).ok());
    ASSERT_NE(cursor, nullptr);
    EXPECT_TRUE(all_docs(*cursor).empty());
    uint64_t df = 0;
    ASSERT_TRUE(source.doc_freq("b", &df).ok());
    EXPECT_EQ(df, 1);

    ASSERT_TRUE(source.open_term("absent", false, false, &cursor).ok());
    EXPECT_EQ(cursor, nullptr);
}

// More documents than one block: seeks and shallow seeks land on the block holding the target.
TEST(GroupSliceSourceTest, NavigatesBlocksOfALargeSlice) {
    auto inner = std::make_shared<FakeIndexSource>();
    inner->set_doc_count(2000);
    std::vector<uint32_t> group;
    for (uint32_t d = 0; d < 2000; d += 2) {
        group.push_back(d);
    }
    inner->add("t", group);
    // Group docids [100, 700): 300 even docs -> segment rowids 0, 2, ..., 598.
    GroupSliceSource source(inner, {.offset = 100, .rows = 600, .group_rows = 2000});
    std::unique_ptr<PostingsCursor> cursor;
    ASSERT_TRUE(source.open_term("t", false, false, &cursor).ok());
    EXPECT_EQ(cursor->doc_freq(), 300);

    PostingsBlock block;
    bool eof = false;
    ASSERT_TRUE(cursor->seek_block(300, &block, &eof).ok());
    ASSERT_FALSE(eof);
    // Rowid 300 is the 151st doc: block [128, 256) of the slice, rowids 256..510.
    EXPECT_EQ(block.size(), 128);
    EXPECT_EQ(block.doc_at(0), 256);
    EXPECT_EQ(cursor->current_block_bound().last_doc, 510);

    bool moved = false;
    ASSERT_TRUE(cursor->shallow_seek(400, &moved).ok());
    EXPECT_FALSE(moved);
    ASSERT_TRUE(cursor->shallow_seek(520, &moved).ok());
    EXPECT_TRUE(moved);
    EXPECT_EQ(cursor->current_block_bound().last_doc, 598);
    ASSERT_TRUE(cursor->next_block(&block, &eof).ok());
    EXPECT_EQ(block.size(), 44);
    EXPECT_EQ(block.doc_at(0), 512);
    ASSERT_TRUE(cursor->next_block(&block, &eof).ok());
    EXPECT_TRUE(eof);

    ASSERT_TRUE(cursor->seek_block(599, &block, &eof).ok());
    EXPECT_TRUE(eof);
    ASSERT_TRUE(cursor->rewind().ok());
    EXPECT_EQ(all_docs(*cursor).size(), 300);
}

// Positions, frequencies and norms follow their documents; docids passed in are shifted back.
TEST(GroupSliceSourceTest, KeepsPositionsAndTranslatesNorms) {
    auto inner = std::make_shared<FakeIndexSource>();
    inner->set_doc_count(100);
    inner->add("p", {{.doc = 5, .positions = {1}},
                     {.doc = 12, .positions = {0, 4}},
                     {.doc = 15, .positions = {7}},
                     {.doc = 40, .positions = {2}}});
    inner->norms = {{12, 9}, {15, 3}};
    inner->remove(15);
    GroupSliceSource source(inner, {.offset = 10, .rows = 20, .group_rows = 100});

    std::unique_ptr<PostingsCursor> cursor;
    ASSERT_TRUE(source.open_term("p", true, true, &cursor).ok());
    PostingsBlock block;
    bool eof = false;
    ASSERT_TRUE(cursor->next_block(&block, &eof).ok());
    ASSERT_EQ(block.size(), 2);
    EXPECT_EQ(block.doc_at(0), 2);
    EXPECT_EQ(block.doc_at(1), 5);
    EXPECT_EQ(block.freq_at(0), 2);
    EXPECT_EQ(block.freq_at(1), 1);

    std::vector<uint32_t> positions;
    ASSERT_TRUE(cursor->append_positions(0, 0, positions).ok());
    EXPECT_EQ(positions, (std::vector<uint32_t> {0, 4}));
    PositionCursor* position_cursor = nullptr;
    ASSERT_TRUE(cursor->open_positions(1, &position_cursor).ok());
    uint32_t position = 0;
    bool available = false;
    ASSERT_TRUE(position_cursor->next_position(&position, &available).ok());
    EXPECT_TRUE(available);
    EXPECT_EQ(position, 7);
    ASSERT_TRUE(position_cursor->next_position(&position, &available).ok());
    EXPECT_FALSE(available);

    std::vector<uint32_t> norms;
    const std::vector<uint32_t> rows = {2, 5, 7};
    ASSERT_TRUE(source.encoded_norms(rows, &norms).ok());
    EXPECT_EQ(norms, (std::vector<uint32_t> {9, 3, 1}));
    EXPECT_TRUE(source.is_live(2));
    EXPECT_FALSE(source.is_live(5));
}

} // namespace doris::segment_v2
