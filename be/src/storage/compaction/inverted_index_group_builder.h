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

#include <gen_cpp/olap_common.pb.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/status.h"
#include "io/fs/file_system.h"
#include "storage/rowset/rowset_fwd.h"
#include "storage/rowset/rowset_index_groups.h"

namespace doris {
class TabletSchema;
namespace segment_v2 {
class IndexFileWriter;
} // namespace segment_v2

// The segment group index files of a compaction output, opened once all its segments are written
// and before SNII index compaction runs, so that compaction merges its text indexes straight into
// the groups instead of into per-segment files that would be merged a second time. Indexes it
// writes are recorded with mark_written(); InvertedIndexGroupBuilder::build() adds the rest.
class PendingIndexGroups {
public:
    ~PendingIndexGroups();

    const RowsetIndexGroups& groups() const { return _groups; }
    segment_v2::IndexFileWriter* writer(int32_t group_id) const { return _writers[group_id].get(); }
    void mark_written(int64_t index_id) { _written.insert(index_id); }

private:
    friend class InvertedIndexGroupBuilder;
    PendingIndexGroups(std::vector<int32_t> counts, const std::vector<uint32_t>& segment_rows);

    std::vector<int32_t> _counts;
    RowsetIndexGroups _groups;
    io::FileSystemSPtr _fs;
    std::vector<std::string> _paths;
    std::vector<std::unique_ptr<segment_v2::IndexFileWriter>> _writers;
    std::set<int64_t> _written;
    bool _sealed = false;
};

// One index file of a rowset as a merge source: a segment's own file, or for a grouped rowset a
// group file covering `segment_count` consecutive positions from `first_pos`, whose docids are
// those segments' rows concatenated.
struct IndexSourceUnit {
    std::string index_path_prefix;
    InvertedIndexFileInfo file_info;
    int64_t first_pos = 0;
    int32_t segment_count = 1;
};

// The index files a compaction reads from `rowset`, in position order.
Result<std::vector<IndexSourceUnit>> index_source_units(Rowset* rowset);

// Turns a freshly built, not yet visible compaction output rowset into the segment group index
// layout: the SNII inverted and BKD indexes of consecutive segments are merged into
// {rowset_id}_g{n}.idx files, the per-segment index files are deleted and the rowset meta records
// the groups (RowsetMetaPB.inverted_index_group_segment_counts).
//
// Only a rowset whose every index can be grouped is converted (all-or-nothing, so no segment
// container has to be rewritten to drop part of its indexes); ANN indexes and indexes on variant
// subcolumns keep the rowset in the segment layout.
class InvertedIndexGroupBuilder {
public:
    // Greedy left-to-right cut of positions into groups: a group closes before it would exceed
    // max_group_bytes of summed per-segment index bytes or reach 2^32 rows. Every group holds at
    // least one segment. Example: rows [10,10,10], bytes [4,4,4], max 8 -> [2, 1].
    static std::vector<int32_t> plan_groups(const std::vector<uint32_t>& segment_rows,
                                            const std::vector<int64_t>& segment_index_bytes,
                                            int64_t max_group_bytes);

    // Opens the group files of a compaction output whose segments are all written, or returns
    // nullptr when the output will keep the segment layout (see build()). The group byte cap is
    // applied to `estimated_index_bytes` spread over the segments by row count, since the
    // per-segment index sizes are not known before the rowset is built.
    static Result<std::unique_ptr<PendingIndexGroups>> prepare(
            const TabletSchema& schema, const io::FileSystemSPtr& fs,
            const std::string& any_segment_path, const std::string& rowset_id,
            const std::vector<uint32_t>& segment_rows, int64_t estimated_index_bytes,
            int64_t tablet_id);

    // No-op unless config::enable_inverted_index_segment_group is on, the BE is not in cloud
    // mode, the rowset uses SNII and has at least two segments. With `pending`, the indexes it
    // already holds are kept and only the others are merged from the per-segment files; a
    // failure is then an error, because those indexes exist nowhere else. Without it a failed
    // merge keeps the segment layout and compaction goes on.
    static Status build(const RowsetSharedPtr& rowset, int64_t tablet_id,
                        std::unique_ptr<PendingIndexGroups> pending);
};

} // namespace doris
