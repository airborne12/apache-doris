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

// LIKE / REGEXP 下推 gram 布尔查询的 evaluate_inverted_index 测试：用一个假的
// IndexIterator + 桩 InvertedIndexReader 验证「方案解析 -> 编译 -> 下发的
// InvertedIndexParam -> 近似标记」全链路，不依赖真实索引文件 / 真实 SNII reader。

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/status.h"
#include "core/column/column_const.h"
#include "core/column/column_string.h"
#include "core/data_type/data_type_string.h"
#include "exprs/function/like.h"
#include "gen_cpp/AgentService_types.h"
#include "runtime/exec_env.h"
#include "runtime/index_policy/index_policy_mgr.h"
#include "storage/index/index_iterator.h"
#include "storage/index/inverted/gram/gram_query.h"
#include "storage/index/inverted/inverted_index_iterator.h"
#include "storage/index/inverted/inverted_index_reader.h"
#include "storage/tablet/tablet_schema.h"

namespace doris {

// 桩 InvertedIndexReader：evaluate_gram_index 只需要它的 get_index_properties()
// （由基类 InvertedIndexReader 直接读 TabletIndex 的属性表提供，无需覆写）。其余纯
// 虚函数在本测试里永远不会被调用到——FakeGramIndexIterator::read_from_index 直接截获
// 下发的 InvertedIndexParam 并自行回填命中集合，不经过 reader->query()——因此给出最
// 简单的占位实现即可。
class StubGramIndexReader : public segment_v2::InvertedIndexReader {
public:
    explicit StubGramIndexReader(const TabletIndex& index_meta)
            : segment_v2::InvertedIndexReader(&index_meta, nullptr) {}
    ~StubGramIndexReader() override = default;

    segment_v2::InvertedIndexReaderType type() override {
        return segment_v2::InvertedIndexReaderType::FULLTEXT;
    }

    Status query(const segment_v2::IndexQueryContextPtr& context, const std::string& column_name,
                 const Field& query_value, segment_v2::InvertedIndexQueryType query_type,
                 std::shared_ptr<roaring::Roaring>& bit_map,
                 const InvertedIndexAnalyzerCtx* analyzer_ctx = nullptr) override {
        return Status::NotSupported("StubGramIndexReader::query is not supported");
    }

    Status try_query(const segment_v2::IndexQueryContextPtr& context,
                     const std::string& column_name, const Field& query_value,
                     segment_v2::InvertedIndexQueryType query_type, size_t* count) override {
        return Status::NotSupported("StubGramIndexReader::try_query is not supported");
    }

    Status new_iterator(std::unique_ptr<segment_v2::IndexIterator>* iterator) override {
        return Status::NotSupported("StubGramIndexReader::new_iterator is not supported");
    }
};

// 用注入的属性表构造一个最小可用的 TabletIndex：走真实的 TabletIndexPB ->
// init_from_pb 链路（与 snii_writer_test.cpp 的 init_*_index_meta 系列同构），
// 从而不必依赖 get_index_properties() 在非 BE_TEST 构建下不是虚函数这一细节。
TabletIndex build_gram_index_meta(const std::map<std::string, std::string>& properties) {
    TabletIndexPB index_pb;
    index_pb.set_index_type(IndexType::INVERTED);
    index_pb.set_index_id(1);
    index_pb.set_index_name("like_gram_index_test");
    index_pb.add_col_unique_id(0);
    for (const auto& [key, value] : properties) {
        index_pb.mutable_properties()->insert({key, value});
    }
    TabletIndex index_meta;
    index_meta.init_from_pb(index_pb);
    return index_meta;
}

// 假 IndexIterator：get_reader 返回桩 reader（供 resolve_gram_scheme 读属性）；
// read_from_index 直接截获下发的 InvertedIndexParam 并回填一个固定的命中集合，
// 不经过真实的 SNII/CLucene reader，从而只验证 evaluate_gram_index 自身的行为。
class FakeGramIndexIterator : public segment_v2::IndexIterator {
public:
    explicit FakeGramIndexIterator(std::map<std::string, std::string> properties)
            : _index_meta(build_gram_index_meta(properties)),
              _reader(std::make_shared<StubGramIndexReader>(_index_meta)) {}
    ~FakeGramIndexIterator() override = default;

    segment_v2::IndexReaderPtr get_reader(segment_v2::IndexReaderType reader_type) const override {
        if (std::holds_alternative<segment_v2::InvertedIndexReaderType>(reader_type) &&
            std::get<segment_v2::InvertedIndexReaderType>(reader_type) ==
                    segment_v2::InvertedIndexReaderType::FULLTEXT) {
            return _reader;
        }
        return nullptr;
    }

    Status read_from_index(const segment_v2::IndexParam& param) override {
        auto* p = std::get<segment_v2::InvertedIndexParam*>(param);
        last_param = *p;
        p->roaring->addMany(answer.size(), answer.data());
        return Status::OK();
    }

    Status read_null_bitmap(segment_v2::InvertedIndexQueryCacheHandle* cache_handle) override {
        return Status::OK();
    }

    Result<bool> has_null() override { return false; }

    // 上一次 read_from_index 收到的完整参数，供断言 query_type / query_value 用。
    segment_v2::InvertedIndexParam last_param;
    std::vector<uint32_t> answer {3, 5};

private:
    TabletIndex _index_meta;
    std::shared_ptr<StubGramIndexReader> _reader;
};

std::shared_ptr<FakeGramIndexIterator> make_gram_iterator(
        std::map<std::string, std::string> properties) {
    return std::make_shared<FakeGramIndexIterator>(std::move(properties));
}

// 构造一个常量字符串参数列（ColumnConst(ColumnString) + DataTypeString），
// 用于填充 evaluate_inverted_index 的 arguments 列表（pattern / 自定义 ESCAPE）。
ColumnWithTypeAndName const_string_arg(const std::string& value) {
    auto col = ColumnString::create();
    col->insert_data(value.data(), value.size());
    return {ColumnConst::create(std::move(col), 1), std::make_shared<DataTypeString>(), "arg"};
}

// 注入一对 gram 族策略（tokenizer type=ngram mode=dense + analyzer 引用它），手法
// 复制自 snii_writer_test.cpp 的 ScopedGramPolicies（替换再还原
// ExecEnv::_index_policy_mgr）；id/name 为本文件专属，避免与其他测试文件的策略
// 注册在同一进程内的策略管理器命名空间中打架（本类每次都换成自己的 IndexPolicyMgr
// 实例，所以其实不会真的冲突，但独立取名便于排查失败输出）。
class ScopedGramIndexPolicies {
public:
    ScopedGramIndexPolicies() {
        auto* exec_env = ExecEnv::GetInstance();
        _previous = exec_env->index_policy_mgr();
        exec_env->_index_policy_mgr = &_manager;

        TIndexPolicy tokenizer;
        tokenizer.id = 9101;
        tokenizer.name = "gram_dense_tokenizer";
        tokenizer.type = TIndexPolicyType::TOKENIZER;
        tokenizer.properties["type"] = "ngram";
        tokenizer.properties["mode"] = "dense";

        TIndexPolicy analyzer;
        analyzer.id = 9102;
        analyzer.name = analyzer_name();
        analyzer.type = TIndexPolicyType::ANALYZER;
        analyzer.properties["tokenizer"] = tokenizer.name;

        _manager.apply_policy_changes({tokenizer, analyzer}, {});
    }

    ~ScopedGramIndexPolicies() { ExecEnv::GetInstance()->_index_policy_mgr = _previous; }

    static std::string analyzer_name() { return "gram_dense"; }

private:
    IndexPolicyMgr _manager;
    IndexPolicyMgr* _previous = nullptr;
};

TEST(LikeGramIndexTest, RegexpCompilesAndSendsGramBooleanQuery) {
    ScopedGramIndexPolicies policies;
    auto it = make_gram_iterator({{"analyzer", ScopedGramIndexPolicies::analyzer_name()}});
    auto fn = std::make_shared<FunctionRegexpLike>();
    ColumnsWithTypeAndName args {const_string_arg("hello|world")};
    std::vector<IndexFieldNameAndTypePair> names {{"msg", std::make_shared<DataTypeString>()}};
    segment_v2::InvertedIndexResultBitmap result;

    ASSERT_TRUE(fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result).ok());

    EXPECT_EQ(it->last_param.query_type, segment_v2::InvertedIndexQueryType::GRAM_BOOLEAN_QUERY);
    segment_v2::gram::GramQuery q;
    ASSERT_TRUE(
            segment_v2::gram::GramQuery::parse(it->last_param.query_value.get<TYPE_STRING>(), &q)
                    .ok());
    EXPECT_EQ(q.to_debug_string(),
              "((\"ell\" & \"hel\" & \"llo\") | (\"orl\" & \"rld\" & \"wor\"))");
    EXPECT_TRUE(result.approximate());
    ASSERT_NE(result.get_data_bitmap(), nullptr);
    EXPECT_EQ(result.get_data_bitmap()->cardinality(), 2U);
}

TEST(LikeGramIndexTest, UnindexableOrNonGramProducesNoResult) {
    ScopedGramIndexPolicies policies;
    std::vector<IndexFieldNameAndTypePair> names {{"msg", std::make_shared<DataTypeString>()}};
    auto fn = std::make_shared<FunctionRegexpLike>();

    // gram 族索引，但模式串编译不出任何可裁剪的字面量（退化为 ALL）。
    {
        auto it = make_gram_iterator({{"analyzer", ScopedGramIndexPolicies::analyzer_name()}});
        segment_v2::InvertedIndexResultBitmap result;
        ColumnsWithTypeAndName args {const_string_arg("[0-9]{3}-[0-9]{4}")};
        ASSERT_TRUE(
                fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result).ok());
        EXPECT_TRUE(result.is_empty()); // 未产生结果（is_empty = 无位图，不是基数为零）
    }

    // 非 gram 族索引（内置 english parser，属性里没有 analyzer 字段）。
    {
        auto it = make_gram_iterator({{"parser", "english"}});
        segment_v2::InvertedIndexResultBitmap result;
        ColumnsWithTypeAndName args {const_string_arg("hello")};
        ASSERT_TRUE(
                fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result).ok());
        EXPECT_TRUE(result.is_empty());
    }
}

TEST(LikeGramIndexTest, LikeUsesLikeCompiler) {
    ScopedGramIndexPolicies policies;
    auto it = make_gram_iterator({{"analyzer", ScopedGramIndexPolicies::analyzer_name()}});
    auto fn = std::make_shared<FunctionLike>();
    std::vector<IndexFieldNameAndTypePair> names {{"msg", std::make_shared<DataTypeString>()}};
    segment_v2::InvertedIndexResultBitmap result;
    ColumnsWithTypeAndName args {const_string_arg("%abcd%")};

    ASSERT_TRUE(fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result).ok());

    segment_v2::gram::GramQuery q;
    ASSERT_TRUE(
            segment_v2::gram::GramQuery::parse(it->last_param.query_value.get<TYPE_STRING>(), &q)
                    .ok());
    EXPECT_EQ(q.to_debug_string(), "(\"abc\" & \"bcd\")");
    EXPECT_TRUE(result.approximate());
}

TEST(LikeGramIndexTest, CustomEscapeIsNotPushedDown) {
    ScopedGramIndexPolicies policies;
    auto it = make_gram_iterator({{"analyzer", ScopedGramIndexPolicies::analyzer_name()}});
    auto fn = std::make_shared<FunctionLike>();
    std::vector<IndexFieldNameAndTypePair> names {{"msg", std::make_shared<DataTypeString>()}};
    segment_v2::InvertedIndexResultBitmap result;
    // ESCAPE 子句不是默认的反斜杠：P0 不支持，必须不加速也不能报错。
    ColumnsWithTypeAndName args {const_string_arg("%abcd%"), const_string_arg("#")};

    ASSERT_TRUE(fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result).ok());

    EXPECT_TRUE(result.is_empty());
}

TEST(LikeGramIndexTest, ConfigDisabledSkipsPushDown) {
    ScopedGramIndexPolicies policies;
    auto it = make_gram_iterator({{"analyzer", ScopedGramIndexPolicies::analyzer_name()}});
    auto fn = std::make_shared<FunctionRegexpLike>();
    std::vector<IndexFieldNameAndTypePair> names {{"msg", std::make_shared<DataTypeString>()}};
    segment_v2::InvertedIndexResultBitmap result;
    ColumnsWithTypeAndName args {const_string_arg("hello|world")};

    const bool saved_enable = config::enable_gram_index_regexp;
    config::enable_gram_index_regexp = false;
    const Status status =
            fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result);
    config::enable_gram_index_regexp = saved_enable;

    ASSERT_TRUE(status.ok());
    EXPECT_TRUE(result.is_empty());
}

} // namespace doris
