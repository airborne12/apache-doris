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

#include "inverted_index_iterator.h"

#include <memory>

#include "common/cast_set.h"
#include "olap/inverted_index_parser.h"
#include "olap/rowset/segment_v2/inverted_index_cache.h"
#include "olap/rowset/segment_v2/inverted_index_reader.h"

namespace doris::segment_v2 {

InvertedIndexIterator::InvertedIndexIterator() = default;

void InvertedIndexIterator::add_reader(InvertedIndexReaderType type,
                                       const InvertedIndexReaderPtr& reader) {
    std::string analyzer_key = build_analyzer_identity_from_properties(reader->get_index_properties());
    if (analyzer_key.empty()) {
        analyzer_key = INVERTED_INDEX_DEFAULT_ANALYZER_KEY;
    }
    _reader_entries.push_back(ReaderEntry {type, analyzer_key, reader});
}

Status InvertedIndexIterator::read_from_index(const IndexParam& param) {
    auto* i_param = std::get<InvertedIndexParam*>(param);
    if (i_param == nullptr) {
        return Status::Error<ErrorCode::INDEX_INVALID_PARAMETERS>("i_param is null");
    }
    DBUG_EXECUTE_IF("return_inverted_index_bypass", {
        return Status::Error<ErrorCode::INVERTED_INDEX_BYPASS>("inverted index bypass");
    });

    std::string analyzer_key = i_param->analyzer_key.empty()
                                       ? INVERTED_INDEX_DEFAULT_ANALYZER_KEY
                                       : i_param->analyzer_key;

    auto reader = DORIS_TRY(select_best_reader(i_param->column_type, i_param->query_type,
                                               analyzer_key));
    if (UNLIKELY(reader == nullptr)) {
        throw CLuceneError(CL_ERR_NullPointer, "bkd index reader is null", false);
    }
    auto* runtime_state = _context->runtime_state;
    if (!i_param->skip_try && reader->type() == InvertedIndexReaderType::BKD) {
        if (runtime_state != nullptr &&
            runtime_state->query_options().inverted_index_skip_threshold > 0 &&
            runtime_state->query_options().inverted_index_skip_threshold < 100) {
            auto query_bkd_limit_percent =
                    runtime_state->query_options().inverted_index_skip_threshold;
            size_t hit_count = 0;
            RETURN_IF_ERROR(try_read_from_inverted_index(reader, i_param->column_name,
                                                         i_param->query_value, i_param->query_type,
                                                         &hit_count));
            if (hit_count > i_param->num_rows * query_bkd_limit_percent / 100) {
                return Status::Error<ErrorCode::INVERTED_INDEX_BYPASS>(
                        "hit count: {}, bkd inverted reached limit {}% , segment num "
                        "rows:{}", // add blackspace after % to avoid log4j format bug
                        hit_count, query_bkd_limit_percent, i_param->num_rows);
            }
        }
    }

    auto execute_query = [&]() {
        return reader->query(_context, i_param->column_name, i_param->query_value,
                             i_param->query_type, i_param->roaring);
    };

    if (runtime_state->query_options().enable_profile) {
        InvertedIndexQueryStatistics query_stats;
        {
            SCOPED_RAW_TIMER(&query_stats.exec_time);
            RETURN_IF_ERROR(execute_query());
        }
        query_stats.column_name = i_param->column_name;
        query_stats.hit_rows = i_param->roaring->cardinality();
        _context->stats->inverted_index_stats.stats.emplace_back(query_stats);
    } else {
        RETURN_IF_ERROR(execute_query());
    }

    return Status::OK();
}

Status InvertedIndexIterator::read_null_bitmap(InvertedIndexQueryCacheHandle* cache_handle) {
    auto reader = DORIS_TRY(select_best_reader(INVERTED_INDEX_DEFAULT_ANALYZER_KEY));
    return reader->read_null_bitmap(_context, cache_handle, nullptr);
}

Result<bool> InvertedIndexIterator::has_null() {
    auto reader = DORIS_TRY(select_best_reader(INVERTED_INDEX_DEFAULT_ANALYZER_KEY));
    return reader->has_null();
}

Status InvertedIndexIterator::try_read_from_inverted_index(const InvertedIndexReaderPtr& reader,
                                                           const std::string& column_name,
                                                           const void* query_value,
                                                           InvertedIndexQueryType query_type,
                                                           size_t* count) {
    // NOTE: only bkd index support try read now.
    if (query_type == InvertedIndexQueryType::GREATER_EQUAL_QUERY ||
        query_type == InvertedIndexQueryType::GREATER_THAN_QUERY ||
        query_type == InvertedIndexQueryType::LESS_EQUAL_QUERY ||
        query_type == InvertedIndexQueryType::LESS_THAN_QUERY ||
        query_type == InvertedIndexQueryType::EQUAL_QUERY) {
        RETURN_IF_ERROR(reader->try_query(_context, column_name, query_value, query_type, count));
    }
    return Status::OK();
}

Result<InvertedIndexReaderPtr> InvertedIndexIterator::select_best_reader(
        const vectorized::DataTypePtr& column_type, InvertedIndexQueryType query_type,
        const std::string& analyzer_key) {
    if (_reader_entries.empty()) {
        return ResultError(Status::RuntimeError(
                "No available inverted index readers. Check if index is properly initialized."));
    }

    std::string normalized_key = analyzer_key.empty() ? INVERTED_INDEX_DEFAULT_ANALYZER_KEY
                                                      : analyzer_key;

    std::vector<const ReaderEntry*> candidates;
    candidates.reserve(_reader_entries.size());
    for (const auto& entry : _reader_entries) {
        if (entry.analyzer_key == normalized_key) {
            candidates.push_back(&entry);
        }
    }

    if (candidates.empty() && normalized_key != INVERTED_INDEX_DEFAULT_ANALYZER_KEY) {
        for (const auto& entry : _reader_entries) {
            if (entry.analyzer_key == INVERTED_INDEX_DEFAULT_ANALYZER_KEY) {
                candidates.push_back(&entry);
            }
        }
    }

    if (candidates.empty()) {
        for (const auto& entry : _reader_entries) {
            candidates.push_back(&entry);
        }
    }

    if (candidates.empty()) {
        return ResultError(Status::RuntimeError(
                "No available inverted index readers. Check if index is properly initialized."));
    }

    auto choose_entry = [&](const std::vector<const ReaderEntry*>& entries)
            -> const ReaderEntry* {
        if (entries.empty()) {
            return nullptr;
        }

        const auto field_type = column_type->get_storage_field_type();
        const bool is_string = is_string_type(field_type);
        if (is_string) {
            if (is_match_query(query_type)) {
                for (const auto* entry : entries) {
                    if (entry->type == InvertedIndexReaderType::FULLTEXT) {
                        return entry;
                    }
                }
            } else if (is_equal_query(query_type)) {
                for (const auto* entry : entries) {
                    if (entry->type == InvertedIndexReaderType::STRING_TYPE) {
                        return entry;
                    }
                }
            }
        }
        return entries.front();
    };

    const ReaderEntry* entry = choose_entry(candidates);
    if (entry == nullptr) {
        return ResultError(Status::RuntimeError("Index query type not supported"));
    }
    return entry->reader;
}

Result<InvertedIndexReaderPtr> InvertedIndexIterator::select_best_reader(
        const std::string& analyzer_key) {
    if (_reader_entries.empty()) {
        return ResultError(Status::RuntimeError(
                "No available inverted index readers. Check if index is properly initialized."));
    }
    std::string normalized_key = analyzer_key.empty() ? INVERTED_INDEX_DEFAULT_ANALYZER_KEY
                                                      : analyzer_key;
    for (const auto& entry : _reader_entries) {
        if (entry.analyzer_key == normalized_key) {
            return entry.reader;
        }
    }
    if (normalized_key != INVERTED_INDEX_DEFAULT_ANALYZER_KEY) {
        for (const auto& entry : _reader_entries) {
            if (entry.analyzer_key == INVERTED_INDEX_DEFAULT_ANALYZER_KEY) {
                return entry.reader;
            }
        }
    }
    return _reader_entries.front().reader;
}

IndexReaderPtr InvertedIndexIterator::get_reader(IndexReaderType type) const {
    const auto* inverted_type = std::get_if<InvertedIndexReaderType>(&type);
    if (inverted_type == nullptr) {
        return nullptr;
    }
    for (const auto& entry : _reader_entries) {
        if (entry.type == *inverted_type) {
            return entry.reader;
        }
    }
    return nullptr;
}

} // namespace doris::segment_v2