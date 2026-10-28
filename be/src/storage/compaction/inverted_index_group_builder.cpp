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

// Opens the compound file of one group. seg_id only names files in
// IndexFileWriter::get_index_file_names(), which groups never use.
Status open_group_writer(const io::FileSystemSPtr& fs, const std::string& prefix,
                         const std::string& rowset_id, int32_t group_id, int64_t tablet_id,
                         std::unique_ptr<segment_v2::IndexFileWriter>* out) {
    io::FileWriterPtr file_writer;
    RETURN_IF_ERROR(fs->create_file(
            segment_v2::InvertedIndexDescriptor::get_index_file_path_v2(prefix), &file_writer));
    *out = std::make_unique<segment_v2::IndexFileWriter>(
            fs, prefix, rowset_id, group_id, InvertedIndexStorageFormatPB::SNII,
            std::move(file_writer), true /* can_use_ram_dir */, tablet_id);
    return Status::OK();
}

// Merges the per-segment indexes of one group that `writer` does not hold yet (`written`) and
// seals the group file. `readers` are the group's segments in position order.
Status write_group(const std::vector<std::unique_ptr<segment_v2::IndexFileReader>>& readers,
                   const std::vector<uint32_t>& rows, const GroupedIndexes& indexes,
                   const std::set<int64_t>& written,
                   const std::shared_ptr<snii::writer::MemoryReporter>& reporter,
                   segment_v2::IndexFileWriter* writer, InvertedIndexFileInfo* file_info) {
    std::vector<uint32_t> offsets;
    uint32_t group_rows = 0;
    for (const uint32_t r : rows) {
        offsets.push_back(group_rows);
        group_rows += r;
    }
    std::vector<const TabletIndex*> text;
    for (const TabletIndex* index : indexes.text) {
        if (!written.contains(index->index_id())) {
            text.push_back(index);
        }
    }
    if (!text.empty()) {
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
        RETURN_IF_ERROR(snii::compaction::ValidatedRowIdConversion::create(
                &map, rows, destination_rows, &conversion));
        for (const TabletIndex* index : text) {
            RETURN_IF_ERROR(
                    merge_text_index(readers, index, *conversion, group_rows, reporter, writer));
        }
    }
    for (const auto& [index, type] : indexes.bkd) {
        DORIS_CHECK(!written.contains(index->index_id()));
        RETURN_IF_ERROR(merge_bkd_index(readers, index, type, offsets, group_rows, writer));
    }
    RETURN_IF_ERROR(writer->begin_close());
    RETURN_IF_ERROR(writer->finish_close());
    *file_info = *writer->get_index_file_info();
    return Status::OK();
}

bool should_group(const TabletSchema& schema, int64_t num_segments, GroupedIndexes* indexes) {
    // Cloud mode is left out until the recycler and file cache warm-up know the group files:
    // until then a recycled rowset would leave its group files behind in the object store.
    return config::enable_inverted_index_segment_group && !config::is_cloud_mode() &&
           num_segments >= 2 &&
           schema.get_inverted_index_storage_format() == InvertedIndexStorageFormatPB::SNII &&
           collect_grouped_indexes(schema, indexes);
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

Result<std::vector<IndexSourceUnit>> index_source_units(Rowset* rowset) {
    std::vector<IndexSourceUnit> units;
    const RowsetMetaSharedPtr meta = rowset->rowset_meta();
    if (!meta->has_inverted_index_groups()) {
        for (auto segment : rowset->segments()) {
            auto path = segment.path();
            if (!path.has_value()) {
                return ResultError(path.error());
            }
            units.push_back(
                    {.index_path_prefix = std::string {segment_v2::InvertedIndexDescriptor::
                                                               get_index_file_path_prefix(*path)},
                     .file_info = segment.inverted_index_file_info(),
                     .first_pos = cast_set<int64_t>(segment.pos()),
                     .segment_count = 1});
        }
        return units;
    }
    std::vector<uint32_t> segment_rows;
    meta->get_num_segment_rows(&segment_rows);
    const RowsetIndexGroups groups({meta->inverted_index_group_segment_counts().begin(),
                                    meta->inverted_index_group_segment_counts().end()},
                                   segment_rows);
    auto first_path = rowset->segment(0).path();
    if (!first_path.has_value()) {
        return ResultError(first_path.error());
    }
    for (int32_t group_id = 0; group_id < groups.group_count(); ++group_id) {
        units.push_back({.index_path_prefix =
                                 segment_v2::InvertedIndexDescriptor::get_index_group_path_prefix(
                                         *first_path, rowset->rowset_id().to_string(), group_id),
                         .file_info = meta->inverted_index_group_file_info(group_id),
                         .first_pos = groups.first_pos(group_id),
                         .segment_count = groups.segment_count(group_id)});
    }
    return units;
}

PendingIndexGroups::PendingIndexGroups(std::vector<int32_t> counts,
                                       const std::vector<uint32_t>& segment_rows)
        : _counts(std::move(counts)), _groups(_counts, segment_rows) {}

// A compaction that failed after prepare() never reaches build(): its group files are removed
// here, the per-segment files go with the abandoned output rowset.
PendingIndexGroups::~PendingIndexGroups() {
    if (!_sealed) {
        _writers.clear();
        for (const auto& path : _paths) {
            static_cast<void>(_fs->delete_file(path));
        }
    }
}

Result<std::unique_ptr<PendingIndexGroups>> InvertedIndexGroupBuilder::prepare(
        const TabletSchema& schema, const io::FileSystemSPtr& fs,
        const std::string& any_segment_path, const std::string& rowset_id,
        const std::vector<uint32_t>& segment_rows, int64_t estimated_index_bytes,
        int64_t tablet_id) {
    GroupedIndexes indexes;
    if (!should_group(schema, static_cast<int64_t>(segment_rows.size()), &indexes)) {
        return std::unique_ptr<PendingIndexGroups>();
    }
    uint64_t total_rows = 0;
    for (const uint32_t r : segment_rows) {
        total_rows += r;
    }
    std::vector<int64_t> estimated_bytes;
    for (const uint32_t r : segment_rows) {
        estimated_bytes.push_back(
                total_rows == 0 ? 0
                                : static_cast<int64_t>(static_cast<double>(estimated_index_bytes) *
                                                       r / static_cast<double>(total_rows)));
    }
    std::unique_ptr<PendingIndexGroups> pending(new PendingIndexGroups(
            plan_groups(segment_rows, estimated_bytes, config::inverted_index_group_max_bytes),
            segment_rows));
    pending->_fs = fs;
    for (int32_t group_id = 0; group_id < pending->_groups.group_count(); ++group_id) {
        const std::string prefix = segment_v2::InvertedIndexDescriptor::get_index_group_path_prefix(
                any_segment_path, rowset_id, group_id);
        std::unique_ptr<segment_v2::IndexFileWriter> writer;
        Status st = open_group_writer(fs, prefix, rowset_id, group_id, tablet_id, &writer);
        if (!st.ok()) {
            for (const auto& path : pending->_paths) {
                static_cast<void>(fs->delete_file(path));
            }
            return ResultError(std::move(st));
        }
        pending->_paths.push_back(
                segment_v2::InvertedIndexDescriptor::get_index_file_path_v2(prefix));
        pending->_writers.push_back(std::move(writer));
    }
    return pending;
}

Status InvertedIndexGroupBuilder::build(const RowsetSharedPtr& rowset, int64_t tablet_id,
                                        std::unique_ptr<PendingIndexGroups> pending) {
    const TabletSchemaSPtr schema = rowset->tablet_schema();
    GroupedIndexes indexes;
    if (!should_group(*schema, rowset->num_segments(), &indexes)) {
        // prepare() said yes for the same schema and segments; only a config flipped in between
        // can say no here, and the indexes it wrote exist nowhere else.
        if (pending != nullptr) {
            return Status::Error<ErrorCode::INVERTED_INDEX_COMPACTION_ERROR>(
                    "segment group index disabled while compaction was writing it");
        }
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

    const std::string rowset_id = rowset->rowset_id().to_string();
    if (pending == nullptr) {
        std::unique_ptr<PendingIndexGroups> fresh(
                new PendingIndexGroups(plan_groups(segment_rows, segment_index_bytes,
                                                   config::inverted_index_group_max_bytes),
                                       segment_rows));
        fresh->_fs = fs;
        for (int32_t group_id = 0; group_id < fresh->_groups.group_count(); ++group_id) {
            const std::string prefix =
                    segment_v2::InvertedIndexDescriptor::get_index_group_path_prefix(
                            segment_prefixes.front() +
                                    std::string(
                                            segment_v2::InvertedIndexDescriptor::segment_suffix),
                            rowset_id, group_id);
            std::unique_ptr<segment_v2::IndexFileWriter> writer;
            Status st = open_group_writer(fs, prefix, rowset_id, group_id, tablet_id, &writer);
            if (!st.ok()) {
                for (const auto& path : fresh->_paths) {
                    static_cast<void>(fs->delete_file(path));
                }
                LOG(WARNING) << "keep segment index layout, cannot create group file. tablet="
                             << tablet_id << ", rowset=" << rowset_id << ", status=" << st;
                return Status::OK();
            }
            fresh->_paths.push_back(
                    segment_v2::InvertedIndexDescriptor::get_index_file_path_v2(prefix));
            fresh->_writers.push_back(std::move(writer));
        }
        pending = std::move(fresh);
    } else {
        // The plan was cut from the same segment row counts before the rowset was built.
        const RowsetIndexGroups built(pending->_counts, segment_rows);
        for (int32_t group_id = 0; group_id < built.group_count(); ++group_id) {
            DORIS_CHECK_EQ(pending->_groups.group_rows(group_id), built.group_rows(group_id));
        }
    }
    const bool fallback_allowed = pending->_written.empty();
    const RowsetIndexGroups& groups = pending->_groups;
    auto reporter = std::make_shared<snii::writer::MemoryReporter>(
            snii::writer::snii_build_consume_release(
                    snii::writer::BuildMemoryPopulation::kUnregistered),
            static_cast<size_t>(config::inverted_index_ram_buffer_size * 1024 * 1024),
            snii::writer::MemoryReporter::CapPolicy::kHardLimit,
            static_cast<uint64_t>(config::snii_postings_workspace_bytes));

    std::vector<InvertedIndexFileInfo> group_infos(groups.group_count());
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
        status = write_group(group_readers, rows, indexes, pending->_written, reporter,
                             pending->_writers[group_id].get(), &group_infos[group_id]);
    }
    if (!status.ok()) {
        if (!fallback_allowed) {
            return status;
        }
        LOG(WARNING) << "keep segment index layout, segment group merge failed. tablet="
                     << tablet_id << ", rowset=" << rowset_id << ", status=" << status;
        return Status::OK();
    }

    int64_t group_bytes = 0;
    for (const auto& info : group_infos) {
        group_bytes += info.index_size();
    }
    pending->_sealed = true;
    meta->set_inverted_index_groups(pending->_counts, group_infos);
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
              << ", segments=" << rowset->num_segments() << ", groups=" << groups.group_count()
              << ", written_by_index_compaction=" << pending->_written.size()
              << ", group_index_bytes=" << group_bytes;
    return Status::OK();
}

} // namespace doris
