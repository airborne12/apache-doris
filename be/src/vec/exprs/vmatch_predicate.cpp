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

#include "vec/exprs/vmatch_predicate.h"

#include <cstdint>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow-field"
#endif

#include <fmt/format.h>
#include <fmt/ranges.h> // IWYU pragma: keep
#include <gen_cpp/Exprs_types.h>
#include <glog/logging.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "common/status.h"
#include "olap/rowset/segment_v2/inverted_index/analyzer/analyzer.h"
#include "olap/rowset/segment_v2/inverted_index_reader.h"
#include "runtime/runtime_state.h"
#include "vec/core/block.h"
#include "vec/core/column_numbers.h"
#include "vec/core/column_with_type_and_name.h"
#include "vec/exprs/vexpr_context.h"
#include "vec/exprs/vslot_ref.h"
#include "vec/functions/match.h"
#include "vec/functions/simple_function_factory.h"

namespace doris {
class RowDescriptor;
class RuntimeState;
} // namespace doris

namespace doris::vectorized {
#include "common/compile_check_begin.h"

using namespace doris::segment_v2;

namespace {

inline std::string normalize_lower(const std::string& value) {
    std::string normalized(value);
    for (auto& ch : normalized) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return normalized;
}

template <typename T, typename = void>
struct match_has_analyzer_field : std::false_type {};

template <typename T>
struct match_has_analyzer_field<T, std::void_t<decltype(std::declval<const T&>().analyzer)>>
        : std::true_type {};

template <typename T, typename = void>
struct match_has_parser_field : std::false_type {};

template <typename T>
struct match_has_parser_field<T, std::void_t<decltype(std::declval<const T&>().parser)>>
        : std::true_type {};

template <typename T>
std::string extract_analyzer_name(const T& predicate) {
    if constexpr (match_has_analyzer_field<T>::value) {
        return predicate.analyzer;
    } else {
        return predicate.custom_analyzer;
    }
}

template <typename T>
bool analyzer_field_is_set(const T& predicate) {
    if constexpr (match_has_analyzer_field<T>::value) {
        return predicate.__isset.analyzer;
    } else {
        return !predicate.custom_analyzer.empty();
    }
}

template <typename T>
std::string extract_parser_name(const T& predicate) {
    if constexpr (match_has_parser_field<T>::value) {
        return predicate.parser;
    } else {
        return predicate.parser_type;
    }
}

template <typename T>
bool parser_field_is_set(const T& predicate) {
    if constexpr (match_has_parser_field<T>::value) {
        return predicate.__isset.parser;
    } else {
        return !predicate.parser_type.empty();
    }
}

inline bool looks_like_builtin_analyzer(const std::string& normalized_analyzer_name) {
    if (normalized_analyzer_name.empty()) {
        return false;
    }
    auto parser_type = get_inverted_index_parser_type_from_string(normalized_analyzer_name);
    return parser_type != InvertedIndexParserType::PARSER_UNKNOWN;
}

} // namespace

VMatchPredicate::VMatchPredicate(const TExprNode& node) : VExpr(node) {
    _inverted_index_ctx = std::make_shared<InvertedIndexCtx>();

    std::string analyzer_name;
    if (analyzer_field_is_set(node.match_predicate)) {
        analyzer_name = extract_analyzer_name(node.match_predicate);
    }
    std::string parser_name;
    if (parser_field_is_set(node.match_predicate)) {
        parser_name = extract_parser_name(node.match_predicate);
    }

    auto parser_type = get_inverted_index_parser_type_from_string(parser_name);
    std::string normalized_analyzer = normalize_lower(analyzer_name);
    if (parser_type == InvertedIndexParserType::PARSER_UNKNOWN) {
        parser_type = get_inverted_index_parser_type_from_string(normalized_analyzer);
    }

    const bool analyzerLooksBuiltin = looks_like_builtin_analyzer(normalized_analyzer);

    if (!analyzer_name.empty() && !analyzerLooksBuiltin) {
        _inverted_index_ctx->custom_analyzer = analyzer_name;
        _inverted_index_ctx->parser_type = InvertedIndexParserType::PARSER_NONE;
        _inverted_index_ctx->analyzer_key = analyzer_name;
    } else {
        _inverted_index_ctx->custom_analyzer.clear();
        if (parser_type == InvertedIndexParserType::PARSER_UNKNOWN) {
            parser_type = InvertedIndexParserType::PARSER_NONE;
        }
        _inverted_index_ctx->parser_type = parser_type;
        if (!analyzer_name.empty()) {
            _inverted_index_ctx->analyzer_key = analyzer_name;
        } else if (!parser_name.empty()) {
            _inverted_index_ctx->analyzer_key = parser_name;
        } else {
            _inverted_index_ctx->analyzer_key = INVERTED_INDEX_DEFAULT_ANALYZER_KEY;
        }
    }

    _inverted_index_ctx->parser_mode.clear();
    _inverted_index_ctx->support_phrase.clear();
    _inverted_index_ctx->char_filter_map.clear();
    _inverted_index_ctx->lower_case.clear();
    _inverted_index_ctx->stop_words.clear();

    _analyzer = inverted_index::InvertedIndexAnalyzer::create_analyzer(_inverted_index_ctx.get());
    _inverted_index_ctx->analyzer = _analyzer.get();
}

VMatchPredicate::~VMatchPredicate() = default;

Status VMatchPredicate::prepare(RuntimeState* state, const RowDescriptor& desc,
                                VExprContext* context) {
    RETURN_IF_ERROR_OR_PREPARED(VExpr::prepare(state, desc, context));

    ColumnsWithTypeAndName argument_template;
    argument_template.reserve(_children.size());
    std::vector<std::string_view> child_expr_name;
    for (auto child : _children) {
        argument_template.emplace_back(nullptr, child->data_type(), child->expr_name());
        child_expr_name.emplace_back(child->expr_name());
    }

    _function = SimpleFunctionFactory::instance().get_function(
            _fn.name.function_name, argument_template, _data_type,
            {.enable_decimal256 = state->enable_decimal256()});
    if (_function == nullptr) {
        std::string type_str;
        for (auto arg : argument_template) {
            type_str = type_str + " " + arg.type->get_name();
        }
        return Status::NotSupported(
                "Function {} is not implemented, input param type is {}, "
                "and return type is {}.",
                _fn.name.function_name, type_str, _data_type->get_name());
    }

    if (auto* match_fn = dynamic_cast<FunctionMatchBase*>(_function.get())) {
        match_fn->set_analyzer_identity(_inverted_index_ctx->analyzer_key);
        match_fn->ensure_analyzer_identity(_inverted_index_ctx.get());
    }

    VExpr::register_function_context(state, context);
    _expr_name = fmt::format("{}({})", _fn.name.function_name, child_expr_name);
    _function_name = _fn.name.function_name;
    _prepare_finished = true;
    return Status::OK();
}

Status VMatchPredicate::open(RuntimeState* state, VExprContext* context,
                             FunctionContext::FunctionStateScope scope) {
    DCHECK(_prepare_finished);
    for (auto& i : _children) {
        RETURN_IF_ERROR(i->open(state, context, scope));
    }
    RETURN_IF_ERROR(VExpr::init_function_context(state, context, scope, _function));
    if (scope == FunctionContext::THREAD_LOCAL || scope == FunctionContext::FRAGMENT_LOCAL) {
        context->fn_context(_fn_context_index)->set_function_state(scope, _inverted_index_ctx);
    }
    if (scope == FunctionContext::FRAGMENT_LOCAL) {
        RETURN_IF_ERROR(VExpr::get_const_col(context, nullptr));
    }
    _open_finished = true;
    return Status::OK();
}

void VMatchPredicate::close(VExprContext* context, FunctionContext::FunctionStateScope scope) {
    VExpr::close_function_context(context, scope, _function);
    VExpr::close(context, scope);
}

Status VMatchPredicate::evaluate_inverted_index(VExprContext* context, uint32_t segment_num_rows) {
    DCHECK_EQ(get_num_children(), 2);
    return _evaluate_inverted_index(context, _function, segment_num_rows);
}

Status VMatchPredicate::execute(VExprContext* context, Block* block, int* result_column_id) {
    DCHECK(_open_finished || _getting_const_col);
    if (fast_execute(context, block, result_column_id)) {
        return Status::OK();
    }
    DBUG_EXECUTE_IF("VMatchPredicate.execute", {
        return Status::Error<ErrorCode::INVERTED_INDEX_NOT_SUPPORTED>(
                "{} not support slow path, hit debug point.", _expr_name);
    });
    DBUG_EXECUTE_IF("VMatchPredicate.must_in_slow_path", {
        auto debug_col_name = DebugPoints::instance()->get_debug_param_or_default<std::string>(
                "VMatchPredicate.must_in_slow_path", "column_name", "");

        std::vector<std::string> column_names;
        boost::split(column_names, debug_col_name, boost::algorithm::is_any_of(","));

        auto* column_slot_ref = assert_cast<VSlotRef*>(get_child(0).get());
        std::string column_name = column_slot_ref->expr_name();
        auto it = std::ranges::find(column_names, column_name);
        if (it == column_names.end()) {
            return Status::Error<ErrorCode::INTERNAL_ERROR>(
                    "column {} should in slow path while VMatchPredicate::execute.", column_name);
        }
    })
    doris::vectorized::ColumnNumbers arguments(_children.size());
    for (int i = 0; i < _children.size(); ++i) {
        int column_id = -1;
        RETURN_IF_ERROR(_children[i]->execute(context, block, &column_id));
        arguments[i] = column_id;
    }
    // call function
    uint32_t num_columns_without_result = block->columns();
    // prepare a column to save result
    block->insert({nullptr, _data_type, _expr_name});
    RETURN_IF_ERROR(_function->execute(context->fn_context(_fn_context_index), *block, arguments,
                                       num_columns_without_result, block->rows(), false));
    *result_column_id = num_columns_without_result;
    return Status::OK();
}

const std::string& VMatchPredicate::expr_name() const {
    return _expr_name;
}

const std::string& VMatchPredicate::function_name() const {
    return _function_name;
}

std::string VMatchPredicate::debug_string() const {
    std::stringstream out;
    out << "MatchPredicate(" << children()[0]->debug_string() << ",[";
    uint16_t num_children = get_num_children();

    for (uint16_t i = 1; i < num_children; ++i) {
        out << (i == 1 ? "" : " ") << children()[i]->debug_string();
    }

    out << "])";
    return out.str();
}

#include "common/compile_check_end.h"
} // namespace doris::vectorized