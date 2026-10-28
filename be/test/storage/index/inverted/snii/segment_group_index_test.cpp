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

// Segment group index: the pieces that turn per-segment SNII indexes into one index over the
// concatenated rows of consecutive segments, and back.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <roaring/roaring.hh>
#include <string>
#include <utility>
#include <vector>

#include "storage/compaction/inverted_index_group_builder.h"
#include "storage/index/inverted/common/index_group_slice.h"
#include "storage/index/inverted/snii/bkd/bkd_builder.h"
#include "storage/index/inverted/snii/bkd/bkd_reader.h"
#include "storage/index/inverted/snii/bkd/group_merge_source.h"
#include "storage/index/inverted/snii/compaction/rowid_conversion.h"
#include "storage/index/inverted/snii/compaction/snii_index_compaction.h"
#include "storage/index/inverted/snii/encoding/byte_sink.h"
#include "storage/index/inverted/snii/query/count_query.h"
#include "storage/index/inverted/snii/reader/logical_index_reader.h"
#include "storage/index/inverted/snii/reader/snii_segment_reader.h"
#include "storage/index/inverted/snii/snii_query_test_util.h"
#include "storage/index/inverted/snii/writer/snii_compound_writer.h"
#include "storage/key_coder.h"
#include "storage/olap_common.h"
#include "storage/rowset/rowset_index_groups.h"

namespace doris {
namespace {

using namespace doris::snii::snii_test; // NOLINT
using segment_v2::IndexGroupSlice;

TEST(SegmentGroupIndexTest, RowsetIndexGroupsMapsPositionsToGroupOffsets) {
    // The example of rowset_index_groups.h.
    const RowsetIndexGroups groups({3, 2}, {10, 20, 5, 7, 8});
    EXPECT_EQ(groups.group_count(), 2);
    const std::array<int32_t, 5> group = {0, 0, 0, 1, 1};
    const std::array<uint32_t, 5> offset = {0, 10, 30, 0, 7};
    for (int64_t pos = 0; pos < 5; ++pos) {
        EXPECT_EQ(groups.group_of(pos), group[pos]);
        EXPECT_EQ(groups.offset_of(pos), offset[pos]);
    }
    EXPECT_EQ(groups.group_rows(0), 35U);
    EXPECT_EQ(groups.group_rows(1), 15U);
    EXPECT_EQ(groups.first_pos(1), 3);
    EXPECT_EQ(groups.segment_count(0), 3);
}

TEST(SegmentGroupIndexTest, PlanGroupsCutsOnBytesAndOnTheDocidDomain) {
    // The example of the header: bytes [4, 4, 4] under 8 -> [2, 1].
    EXPECT_EQ(InvertedIndexGroupBuilder::plan_groups({10, 10, 10}, {4, 4, 4}, 8),
              (std::vector<int32_t> {2, 1}));
    // Everything fits: one group.
    EXPECT_EQ(InvertedIndexGroupBuilder::plan_groups({10, 10, 10}, {4, 4, 4}, 100),
              (std::vector<int32_t> {3}));
    // A segment bigger than the cap still forms its own group.
    EXPECT_EQ(InvertedIndexGroupBuilder::plan_groups({10, 10}, {20, 1}, 8),
              (std::vector<int32_t> {1, 1}));
    // 3e9 + 2e9 rows would not fit uint32 docids.
    EXPECT_EQ(InvertedIndexGroupBuilder::plan_groups({3000000000U, 2000000000U}, {1, 1}, 100),
              (std::vector<int32_t> {1, 1}));
}

TEST(SegmentGroupIndexTest, SliceTranslatesBetweenGroupAndSegmentRowids) {
    const IndexGroupSlice slice {.offset = 10, .rows = 20, .group_rows = 50};
    roaring::Roaring group;
    group.addMany(6, std::array<uint32_t, 6> {0, 9, 10, 14, 29, 30}.data());
    EXPECT_EQ(slice.to_segment(group), roaring::Roaring({0, 4, 19}));
    EXPECT_TRUE(slice.contains(10));
    EXPECT_TRUE(slice.contains(29));
    EXPECT_FALSE(slice.contains(30));
    EXPECT_FALSE(slice.contains(9));
    // Group-space bitmap with nothing in the slice.
    EXPECT_TRUE(slice.to_segment(roaring::Roaring({1, 40})).isEmpty());
}

// ---------------------------------------------------------------- text index group merge

constexpr uint64_t kIndexId = 7;
constexpr std::string_view kSuffix;

snii::writer::SniiIndexInput make_input(uint32_t doc_count, std::vector<uint32_t> nulls,
                                        std::vector<snii::writer::TermPostings> terms) {
    snii::writer::SniiIndexInput input;
    input.index_id = kIndexId;
    input.index_suffix = kSuffix;
    input.config = snii::format::IndexConfig::kDocsPositions;
    input.doc_count = doc_count;
    input.null_docids = std::move(nulls);
    input.terms = std::move(terms);
    return input;
}

struct Opened {
    MemoryFile file;
    snii::reader::SniiSegmentReader segment;
    snii::reader::LogicalIndexReader index;
};

void build(snii::writer::SniiIndexInput input, Opened* out,
           snii::reader::LogicalIndexOpenMode mode = snii::reader::LogicalIndexOpenMode::kQuery) {
    snii::writer::SniiCompoundWriter compound(&out->file);
    assert_ok(compound.add_logical_index(input));
    assert_ok(compound.finish());
    assert_ok(snii::reader::SniiSegmentReader::open(&out->file, &out->segment));
    assert_ok(out->segment.open_index(kIndexId, kSuffix, &out->index, mode));
}

// Two segments merged with the identity-plus-offset conversion the group builder uses must give
// exactly the bytes of an index built directly over the concatenated rows.
TEST(SegmentGroupIndexTest, TextGroupMergeIsByteIdenticalToConcatenatedRebuild) {
    Opened seg0;
    Opened seg1;
    build(make_input(3, {1},
                     {make_term("alpha", {{.docid = 0, .positions = {0}},
                                          {.docid = 2, .positions = {0, 3}}}),
                      make_term("beta", {{.docid = 2, .positions = {1}}})}),
          &seg0, snii::reader::LogicalIndexOpenMode::kCompaction);
    build(make_input(2, {},
                     {make_term("alpha", {{.docid = 1, .positions = {2}}}),
                      make_term("gamma", {{.docid = 0, .positions = {0}}})}),
          &seg1, snii::reader::LogicalIndexOpenMode::kCompaction);

    // offsets 0 and 3; one destination of 5 rows.
    const snii::compaction::RowIdConversionMap map = {{{0, 0}, {0, 1}, {0, 2}}, {{0, 3}, {0, 4}}};
    std::unique_ptr<snii::compaction::ValidatedRowIdConversion> conversion;
    assert_ok(snii::compaction::ValidatedRowIdConversion::create(
            &map, std::vector<uint32_t> {3, 2}, std::vector<uint32_t> {5}, &conversion));
    std::unique_ptr<snii::compaction::SniiPlainT2MergePlan> plan;
    assert_ok(snii::compaction::SniiPlainT2MergePlan::prepare({&seg0.index, &seg1.index},
                                                              *conversion, 1U << 20, &plan));
    EXPECT_EQ(plan->destination_null_docids(0), (std::vector<uint32_t> {1}));

    MemoryFile merged;
    snii::writer::SniiCompoundWriter compound(&merged);
    snii::writer::SniiStreamedIndexSession* session = nullptr;
    assert_ok(compound.begin_streamed_index(make_input(5, plan->destination_null_docids(0), {}),
                                            &session));
    std::array<snii::writer::SniiStreamedIndexSession*, 1> sessions = {session};
    assert_ok(plan->execute(sessions));
    assert_ok(compound.finish());

    Opened expected;
    build(make_input(5, {1},
                     {make_term("alpha", {{.docid = 0, .positions = {0}},
                                          {.docid = 2, .positions = {0, 3}},
                                          {.docid = 4, .positions = {2}}}),
                      make_term("beta", {{.docid = 2, .positions = {1}}}),
                      make_term("gamma", {{.docid = 3, .positions = {0}}})}),
          &expected);
    EXPECT_EQ(merged.data(), expected.file.data());
}

// A segment's share of a group term df: the window directory answers whole windows, only the
// boundary windows are decoded, and the result equals counting the decoded posting.
TEST(SegmentGroupIndexTest, TermDfInRangeMatchesDecodedPosting) {
    std::vector<PostingDoc> dense;
    std::vector<PostingDoc> sparse;
    for (uint32_t d = 0; d < 60000; ++d) {
        if (d % 3 != 1) {
            dense.push_back({.docid = d, .positions = {0}});
        }
        if (d % 997 == 0) {
            sparse.push_back({.docid = d, .positions = {1}});
        }
    }
    Opened index;
    build(make_input(60000, {}, {make_term("dense", dense), make_term("sparse", sparse)}), &index);
    const std::array<std::pair<uint32_t, uint64_t>, 6> ranges = {
            std::pair<uint32_t, uint64_t> {0, 60000},
            {0, 1},
            {1234, 1235},
            {5000, 37777},
            {59999, 60000},
            {30000, 30000}};
    for (const char* term : {"dense", "sparse", "absent"}) {
        std::vector<uint32_t> all;
        if (std::string(term) == "dense") {
            for (const auto& d : dense) all.push_back(d.docid);
        } else if (std::string(term) == "sparse") {
            for (const auto& d : sparse) all.push_back(d.docid);
        }
        for (const auto& [lo, hi] : ranges) {
            uint64_t got = 0;
            assert_ok(snii::query::count_only_term_df_in_range(index.index, term, lo, hi, &got));
            const auto expected = static_cast<uint64_t>(std::count_if(
                    all.begin(), all.end(), [&](uint32_t d) { return d >= lo && d < hi; }));
            EXPECT_EQ(got, expected) << term << " [" << lo << ", " << hi << ")";
        }
    }
}

// ---------------------------------------------------------------- BKD group merge

constexpr FieldType kBkdType = FieldType::OLAP_FIELD_TYPE_BIGINT;

struct Point {
    int64_t value = 0;
    uint32_t doc = 0;
};

std::string encode(int64_t value) {
    std::string out;
    get_key_coder(kBkdType)->full_encode_ascending(&value, &out);
    return out;
}

snii::Slice slice_of(const std::string& bytes) {
    return {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
}

struct BkdImage {
    MemoryFile file;
    std::unique_ptr<snii::bkd::BkdReader> reader;
};

snii::bkd::BkdBuilderOptions bkd_options() {
    snii::bkd::BkdBuilderOptions options;
    options.bytes_per_dim = sizeof(int64_t);
    options.field_type = kBkdType;
    options.points_per_leaf = 16; // many leaves for a few hundred points
    return options;
}

// bkd_data followed by bkd_index in one in-memory file, opened.
void open_image(const snii::ByteSink& index, BkdImage* image) {
    const uint64_t data_length = image->file.bytes_written();
    assert_ok(image->file.append(index.view()));
    snii::bkd::BkdSections sections;
    sections.data_offset = 0;
    sections.data_length = data_length;
    sections.index_offset = data_length;
    sections.index_length = index.size();
    assert_ok(snii::bkd::BkdReader::open(&image->file, sections, &image->reader));
}

void build_bkd(const std::vector<Point>& points, BkdImage* image) {
    std::unique_ptr<snii::bkd::BkdBuilder> builder;
    assert_ok(snii::bkd::BkdBuilder::create(bkd_options(), &builder));
    for (const Point& point : points) {
        const std::string value = encode(point.value);
        assert_ok(builder->add(point.doc, slice_of(value)));
    }
    snii::ByteSink index;
    snii::bkd::BkdStats stats;
    assert_ok(builder->finish(&image->file, &index, &stats));
    open_image(index, image);
}

std::vector<Point> random_points(uint32_t docs, uint64_t seed, bool with_repeats) {
    std::vector<Point> points;
    uint64_t state = seed;
    for (uint32_t doc = 0; doc < docs; ++doc) {
        // Every third document holds two values (an ARRAY row), every seventh none (NULL).
        const uint32_t values = doc % 7 == 0 ? 0 : (with_repeats && doc % 3 == 0 ? 2 : 1);
        for (uint32_t i = 0; i < values; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            points.push_back({static_cast<int64_t>(state % 200) - 100, doc});
        }
    }
    return points;
}

roaring::Roaring brute_force(const std::vector<Point>& points, int64_t lo, int64_t hi) {
    roaring::Roaring out;
    for (const Point& point : points) {
        if (point.value >= lo && point.value <= hi) {
            out.add(point.doc);
        }
    }
    return out;
}

TEST(SegmentGroupIndexTest, BkdGroupMergeAnswersRangesLikeTheConcatenatedRows) {
    const std::vector<uint32_t> rows = {150, 1, 260, 90};
    std::vector<BkdImage> images(rows.size());
    std::vector<const snii::bkd::BkdReader*> readers;
    std::vector<uint32_t> offsets;
    std::vector<Point> concatenated;
    uint32_t offset = 0;
    uint64_t expected_docs = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        // Segment 1 holds a single NULL row: an empty BKD.
        const auto points = random_points(rows[i], 1000 + i, true);
        build_bkd(points, &images[i]);
        readers.push_back(images[i].reader.get());
        offsets.push_back(offset);
        expected_docs += images[i].reader->doc_count();
        for (const Point& point : points) {
            concatenated.push_back({point.value, point.doc + offset});
        }
        offset += rows[i];
    }
    ASSERT_TRUE(images[1].reader->empty());

    snii::bkd::GroupBkdMergeSource source(readers, offsets, sizeof(int64_t));
    std::unique_ptr<snii::bkd::BkdBuilder> builder;
    assert_ok(snii::bkd::BkdBuilder::create(bkd_options(), &builder));
    BkdImage merged;
    snii::ByteSink index;
    snii::bkd::BkdStats stats;
    assert_ok(builder->finish_from_source(&source, static_cast<uint32_t>(expected_docs),
                                          &merged.file, &index, &stats));
    open_image(index, &merged);

    EXPECT_EQ(merged.reader->point_count(), concatenated.size());
    EXPECT_EQ(merged.reader->doc_count(), expected_docs);
    BkdImage rebuilt;
    std::ranges::sort(concatenated, [](const Point& a, const Point& b) { return a.doc < b.doc; });
    build_bkd(concatenated, &rebuilt);
    EXPECT_EQ(merged.reader->doc_count(), rebuilt.reader->doc_count());
    EXPECT_EQ(merged.reader->min_value().size(), sizeof(int64_t));

    const std::array<std::pair<int64_t, int64_t>, 6> ranges = {
            std::pair {-100, 100}, std::pair {0, 0},     std::pair {-5, 7},
            std::pair {50, 99},    std::pair {101, 200}, std::pair {-100, -100}};
    for (const auto& [lo, hi] : ranges) {
        const std::string l = encode(lo);
        const std::string h = encode(hi);
        roaring::Roaring got;
        assert_ok(merged.reader->range(slice_of(l), true, slice_of(h), true, &got));
        EXPECT_EQ(got, brute_force(concatenated, lo, hi)) << lo << ".." << hi;
    }
}

} // namespace
} // namespace doris
