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

#include "storage/compaction/inverted_index_group_builder.h"

#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "common/check.h"
#include "common/config.h"
#include "common/logging.h"
#include "io/fs/file_system.h"
#include "io/fs/file_writer.h"
#include "storage/field_type.h"
#include "storage/index/index_file_reader.h"
#include "storage/index/index_file_writer.h"
#include "storage/index/inverted/common/inverted_index_desc.h"
#include "storage/index/inverted/snii/compaction/eligibility.h"
#include "storage/index/inverted/snii/compaction/rowid_conversion.h"
#include "storage/index/inverted/snii/compaction/snii_index_compaction.h"
#include "storage/index/inverted/snii/snii_bkd_index_writer.h"
#include "storage/index/inverted/snii/snii_bkd_searcher.h"
#include "storage/index/inverted/snii/writer/memory_reporter.h"
#include "storage/index/inverted/snii/writer/snii_build_memory_tracker.h"
#include "storage/rowset/rowset.h"
#include "storage/rowset/rowset_index_groups.h"
#include "storage/rowset/rowset_meta.h"
#include "storage/segment/segment_loader.h"
#include "storage/tablet/tablet_schema.h"
#include "storage/types.h"
#include "storage/utils.h"

namespace doris {
namespace {

// Same read-ahead budget as SNII index compaction (compaction.cpp).
constexpr size_t kGroupMergeReadAheadBudgetBytes = 64ULL << 20;

struct GroupedIndexes {
    std::vector<const TabletIndex*> text;
    std::vector<std::pair<const TabletIndex*, FieldType>> bkd;
};

// Classifies the rowset's indexes the way ColumnReader::_load_index reads SNII: text indexes
// through SniiIndexReader, numeric ones through SniiBkdIndexReader. Returns false when any index
// cannot be grouped, which keeps the whole rowset in the segment layout.
bool collect_grouped_indexes(const TabletSchema& schema, GroupedIndexes* out) {
    if (schema.has_ann_index()) {
        return false;
    }
    for (const TabletIndex* index : schema.inverted_indexes()) {
        if (index->col_unique_ids().empty() || !index->get_index_suffix().empty() ||
            !schema.has_column_unique_id(index->col_unique_ids()[0])) {
            return false;
        }
        const TabletColumn& column = schema.column_by_uid(index->col_unique_ids()[0]);
        // Variant subcolumns differ per segment, so a group could not hold one index for all.
        if (column.is_variant_type() || column.is_extracted_column()) {
            return false;
        }
        const FieldType type = column.type() == FieldType::OLAP_FIELD_TYPE_ARRAY
                                       ? column.get_sub_column(0).type()
                                       : column.type();
        if (is_string_type(type)) {
            out->text.push_back(index);
        } else if (field_is_numeric_type(type)) {
            out->bkd.emplace_back(index, type);
        } else {
            return false;
        }
    }
    return !out->text.empty() || !out->bkd.empty();
}

Status merge_text_index(const std::vector<std::unique_ptr<segment_v2::IndexFileReader>>& readers,
                        const TabletIndex* index_meta,
                        const snii::compaction::ValidatedRowIdConversion& conversion,
                        uint32_t group_rows,
                        const std::shared_ptr<snii::writer::MemoryReporter>& reporter,
                        segment_v2::IndexFileWriter* writer) {
    std::vector<std::unique_ptr<snii::reader::LogicalIndexReader>> sources;
    std::vector<const snii::reader::LogicalIndexReader*> plan_sources;
    std::vector<snii::compaction::PlainT2CompactionSource> eligibility_sources;
    for (const auto& reader : readers) {
        auto source = reader->open_snii_index(index_meta, nullptr,
                                              snii::reader::LogicalIndexOpenMode::kCompaction);
        if (!source.has_value()) {
            return source.error();
        }
        sources.push_back(std::move(source.value()));
        plan_sources.push_back(sources.back().get());
        eligibility_sources.push_back(
                {.reader = std::cref(*sources.back()), .index_meta = std::cref(*index_meta)});
    }
    snii::compaction::SniiCompactionEligibility eligibility;
    RETURN_IF_ERROR(snii::compaction::validate_snii_compaction_eligibility(
            eligibility_sources, *index_meta, &eligibility));
    std::unique_ptr<snii::compaction::SniiPlainT2MergePlan> plan;
    RETURN_IF_ERROR(snii::compaction::SniiPlainT2MergePlan::prepare(
            std::move(plan_sources), conversion, eligibility, kGroupMergeReadAheadBudgetBytes,
            reporter, &plan));
    snii::writer::SniiStreamedIndexSession* session = nullptr;
    RETURN_IF_ERROR(writer->add_snii_index_streamed(
            index_meta, group_rows, plan->take_destination_null_docids(0),
            plan->destination_writes_norms(), plan->destination_index_config(), reporter,
            &session));
    std::vector<snii::writer::SniiStreamedIndexSession*> sessions {session};
    return plan->execute(sessions);
}

Status merge_bkd_index(const std::vector<std::unique_ptr<segment_v2::IndexFileReader>>& readers,
                       const TabletIndex* index_meta, FieldType value_type,
                       const std::vector<uint32_t>& offsets, uint32_t group_rows,
                       segment_v2::IndexFileWriter* writer) {
    std::vector<std::unique_ptr<snii::bkd::BkdSearcher>> searchers;
    std::vector<const snii::bkd::BkdSearcher*> sources;
    for (const auto& reader : readers) {
        auto searcher = reader->open_snii_bkd_index(index_meta, nullptr);
        if (!searcher.has_value()) {
            return searcher.error();
        }
        searchers.push_back(std::move(searcher.value()));
        sources.push_back(searchers.back().get());
    }
    return segment_v2::merge_snii_bkd_index_into_group(sources, offsets, group_rows, value_type,
                                                       index_meta, writer);
}

// Writes group `group_id` (positions [first_pos, first_pos + count)) into `path`.
Status write_group(const io::FileSystemSPtr& fs, const std::string& rowset_id,
                   const std::string& prefix, int32_t group_id, int64_t tablet_id,
                   const std::vector<std::unique_ptr<segment_v2::IndexFileReader>>& readers,
                   const std::vector<uint32_t>& rows, const GroupedIndexes& indexes,
                   const std::shared_ptr<snii::writer::MemoryReporter>& reporter,
                   InvertedIndexFileInfo* file_info) {
    std::vector<uint32_t> offsets;
    uint32_t group_rows = 0;
    for (const uint32_t r : rows) {
        offsets.push_back(group_rows);
        group_rows += r;
    }
    // Identity-plus-offset conversion: no row moves inside a group, so source i's rowid r is
    // group docid offsets[i] + r. One destination (the group).
    snii::compaction::RowIdConversionMap map(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        map[i].resize(rows[i]);
        for (uint32_t r = 0; r < rows[i]; ++r) {
            map[i][r] = {0, offsets[i] + r};
        }
    }
    std::unique_ptr<snii::compaction::ValidatedRowIdConversion> conversion;
    const std::vector<uint32_t> destination_rows {group_rows};
    RETURN_IF_ERROR(snii::compaction::ValidatedRowIdConversion::create(&map, rows, destination_rows,
                                                                       &conversion));

    io::FileWriterPtr file_writer;
    RETURN_IF_ERROR(fs->create_file(
            segment_v2::InvertedIndexDescriptor::get_index_file_path_v2(prefix), &file_writer));
    // seg_id only names files in segment_v2::IndexFileWriter::get_index_file_names(), which groups never use.
    segment_v2::IndexFileWriter writer(fs, prefix, rowset_id, group_id,
                                       InvertedIndexStorageFormatPB::SNII, std::move(file_writer),
                                       true /* can_use_ram_dir */, tablet_id);
    for (const TabletIndex* index : indexes.text) {
        RETURN_IF_ERROR(
                merge_text_index(readers, index, *conversion, group_rows, reporter, &writer));
    }
    for (const auto& [index, type] : indexes.bkd) {
        RETURN_IF_ERROR(merge_bkd_index(readers, index, type, offsets, group_rows, &writer));
    }
    RETURN_IF_ERROR(writer.begin_close());
    RETURN_IF_ERROR(writer.finish_close());
    *file_info = *writer.get_index_file_info();
    return Status::OK();
}

} // namespace

std::vector<int32_t> InvertedIndexGroupBuilder::plan_groups(
        const std::vector<uint32_t>& segment_rows, const std::vector<int64_t>& segment_index_bytes,
        int64_t max_group_bytes) {
    DORIS_CHECK_EQ(segment_rows.size(), segment_index_bytes.size());
    std::vector<int32_t> counts;
    int32_t count = 0;
    uint64_t rows = 0;
    int64_t bytes = 0;
    for (size_t pos = 0; pos < segment_rows.size(); ++pos) {
        const bool over_rows = rows + segment_rows[pos] >= std::numeric_limits<uint32_t>::max();
        const bool over_bytes = bytes + segment_index_bytes[pos] > max_group_bytes;
        if (count > 0 && (over_rows || over_bytes)) {
            counts.push_back(count);
            count = 0;
            rows = 0;
            bytes = 0;
        }
        ++count;
        rows += segment_rows[pos];
        bytes += segment_index_bytes[pos];
    }
    if (count > 0) {
        counts.push_back(count);
    }
    return counts;
}

Status InvertedIndexGroupBuilder::build(const RowsetSharedPtr& rowset, int64_t tablet_id) {
    // Cloud mode is left out until the recycler and file cache warm-up know the group files:
    // until then a recycled rowset would leave its group files behind in the object store.
    if (!config::enable_inverted_index_segment_group || config::is_cloud_mode() ||
        rowset->num_segments() < 2) {
        return Status::OK();
    }
    const TabletSchemaSPtr schema = rowset->tablet_schema();
    if (schema->get_inverted_index_storage_format() != InvertedIndexStorageFormatPB::SNII) {
        return Status::OK();
    }
    GroupedIndexes indexes;
    if (!collect_grouped_indexes(*schema, &indexes)) {
        return Status::OK();
    }
    RowsetMetaSharedPtr meta = rowset->rowset_meta();
    DORIS_CHECK(!meta->has_inverted_index_groups());
    std::vector<uint32_t> segment_rows;
    meta->get_num_segment_rows(&segment_rows);
    DORIS_CHECK_EQ(static_cast<int64_t>(segment_rows.size()), rowset->num_segments());

    const io::FileSystemSPtr fs = meta->fs();
    std::vector<std::string> segment_prefixes;
    std::vector<int64_t> segment_index_bytes;
    std::vector<std::unique_ptr<segment_v2::IndexFileReader>> readers;
    for (auto segment : rowset->segments()) {
        auto path = segment.path();
        if (!path.has_value()) {
            return path.error();
        }
        segment_prefixes.emplace_back(
                segment_v2::InvertedIndexDescriptor::get_index_file_path_prefix(*path));
        const InvertedIndexFileInfo info = segment.inverted_index_file_info();
        segment_index_bytes.push_back(info.has_index_size() ? info.index_size() : 0);
        auto reader = std::make_unique<segment_v2::IndexFileReader>(
                fs, segment_prefixes.back(), InvertedIndexStorageFormatPB::SNII, info, tablet_id);
        RETURN_IF_ERROR(reader->init(config::inverted_index_read_buffer_size));
        readers.push_back(std::move(reader));
    }

    const std::vector<int32_t> counts =
            plan_groups(segment_rows, segment_index_bytes, config::inverted_index_group_max_bytes);
    const RowsetIndexGroups groups(counts, segment_rows);
    const std::string rowset_id = rowset->rowset_id().to_string();
    auto reporter = std::make_shared<snii::writer::MemoryReporter>(
            snii::writer::snii_build_consume_release(
                    snii::writer::BuildMemoryPopulation::kUnregistered),
            static_cast<size_t>(config::inverted_index_ram_buffer_size * 1024 * 1024),
            snii::writer::MemoryReporter::CapPolicy::kHardLimit,
            static_cast<uint64_t>(config::snii_postings_workspace_bytes));

    std::vector<std::string> group_paths;
    std::vector<InvertedIndexFileInfo> group_infos(counts.size());
    Status status;
    for (int32_t group_id = 0; group_id < groups.group_count() && status.ok(); ++group_id) {
        const auto first = groups.first_pos(group_id);
        const auto count = groups.segment_count(group_id);
        std::vector<std::unique_ptr<segment_v2::IndexFileReader>> group_readers;
        for (int64_t pos = first; pos < first + count; ++pos) {
            group_readers.push_back(std::move(readers[pos]));
        }
        const std::vector<uint32_t> rows(segment_rows.begin() + first,
                                         segment_rows.begin() + first + count);
        const std::string prefix = segment_v2::InvertedIndexDescriptor::get_index_group_path_prefix(
                segment_prefixes[first] +
                        std::string(segment_v2::InvertedIndexDescriptor::segment_suffix),
                rowset_id, group_id);
        group_paths.push_back(segment_v2::InvertedIndexDescriptor::get_index_file_path_v2(prefix));
        status = write_group(fs, rowset_id, prefix, group_id, tablet_id, group_readers, rows,
                             indexes, reporter, &group_infos[group_id]);
    }
    if (!status.ok()) {
        LOG(WARNING) << "keep segment index layout, segment group merge failed. tablet="
                     << tablet_id << ", rowset=" << rowset_id << ", status=" << status;
        for (const auto& path : group_paths) {
            static_cast<void>(fs->delete_file(path));
        }
        return Status::OK();
    }

    int64_t group_bytes = 0;
    for (const auto& info : group_infos) {
        group_bytes += info.index_size();
    }
    meta->set_inverted_index_groups(counts, group_infos);
    meta->set_index_disk_size(group_bytes);
    meta->set_total_disk_size(meta->data_disk_size() + group_bytes);
    // Any Segment object built from the old meta would look for the per-segment files deleted
    // below; the rowset is not visible yet, so dropping cached segments is all that is needed.
    SegmentLoader::instance()->erase_segments(*meta);
    for (const auto& prefix : segment_prefixes) {
        RETURN_IF_ERROR(fs->delete_file(
                segment_v2::InvertedIndexDescriptor::get_index_file_path_v2(prefix)));
    }
    LOG(INFO) << "built segment group index. tablet=" << tablet_id << ", rowset=" << rowset_id
              << ", segments=" << rowset->num_segments() << ", groups=" << counts.size()
              << ", group_index_bytes=" << group_bytes;
    return Status::OK();
}

} // namespace doris
