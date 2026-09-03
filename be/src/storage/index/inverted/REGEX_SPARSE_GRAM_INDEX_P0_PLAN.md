# Doris 正则索引（稀疏 gram 行级倒排）P0 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `REGEXP / RLIKE / LIKE` 在列上存在「以 `ngram` tokenizer（新增 mode=sparse|dense|auto）为分词器的 INVERTED 索引」时，自动编译成 gram 布尔查询走 V4 倒排取候选行位图，只读候选页，再由原表达式在候选 Block 上复验；结果与不走索引完全一致。

**Architecture:** 纯库代码（`GramScheme` / `GramExtractor` / `RegexGramCompiler` / `GramQuery`）先独立成型并用差分模糊测试证明「只漏杀不误杀」；写入侧把 `GramExtractor` 包装成索引策略框架里 `ngram` tokenizer 的新模式，复用 V4 SPIMI DOCS_ONLY 写路径；查询侧给 `like/regexp` 函数实现 `evaluate_inverted_index`，新增 `GRAM_BOOLEAN_QUERY` 查询类型由 V4 reader 以 roaring 位图求值，并在 `SegmentIterator` 引入「近似索引结果」语义保留表达式复验；FE 只做 tokenizer 参数校验与下推许可。

**Tech Stack:** C++20（BE，gtest UT，`run-be-ut.sh`），Java（FE，JUnit，`run-fe-ut.sh`），Groovy 回归（`run-regression-test.sh`），re2 2021-02-02，hyperscan 5.4.2，CRoaring，V4 SPIMI 倒排存储。

**Spec:** `be/src/storage/index/inverted/REGEX_SPARSE_GRAM_INDEX_DESIGN.md`（v0.1，含 §6.1.7 自适应、§6.2.1 统一接口、§6.6 写入成本）

## Global Constraints

- 语义硬约束：索引只能产生**超集**候选；任何索引侧失败（解析失败、参数缺失、格式不支持）只能导致「不加速」，绝不能改变查询结果；`NOT LIKE / NOT REGEXP` 永不裁剪。
- 存储格式：仅 V4（SPIMI）；索引属性 `support_phrase` 对 gram 族强制为 false（DOCS_ONLY）。
- 用户接口：不新增索引类型，不新增 tokenizer 类型，不动 `parser`；只在内置 `ngram` tokenizer 上新增属性 `mode`（`auto|sparse|dense`）、`density`（(0,1]，默认 0.25）、`stop_gram_df`（[0,1]，默认 0.10）；`min_gram`（默认 3）/`max_gram`（默认 16）沿用原名；`mode` 缺省时 `ngram` tokenizer 行为与现状完全一致。
- 大小写：`lower_case=true` 对 gram 族定义为「提取前对输入做 ASCII 折叠」，边界哈希在折叠后计算；`lowercase` token filter 与 `mode=sparse|auto` 组合由 FE 校验器拒绝。
- gram 单位：ASCII 段按字节切 gram；非 ASCII 码点各自成一个 1-gram；非法 UTF-8 字节按单字节 1-gram。
- 边界哈希 v1 固定为 `mix64((((uint64)a<<8)|b) ^ 0x5bd1e995) & 0xFFFF < density×65536`，`mix64` = splitmix64 finalizer，与原型 `tools/regex-ngram-model/ngram_model.cpp` 逐位一致；`hash_version` 写入段元数据。
- P0 简化（已在计划中标注）：`mode=auto` 在 P0 解析为 `sparse`（段级样本决策放 P1，Task 12 预留元数据字段）；两层 posting（稀有 gram 指纹表）、段级布隆、S3 批量取数、字面量预检均为 P1；P0 的复验直接用现有 `constant_regex_fn`（hyperscan/re2/memmem 快路径）。
- 编码风格：BE 遵循 `be/.clang-format`（提交前 `/be-code-style`）；FE 遵循 checkstyle（`/fe-code-style`）；所有新文件带 Apache License 头；注释用中文。
- 每个 Task 结束时 UT 必须绿，且提交为独立 commit；阶段 A 可作为第一个独立 PR。

---

## 文件结构总览

| 路径 | 职责 | 任务 |
|---|---|---|
| `be/src/storage/index/inverted/gram/gram_scheme.{h,cpp}` | 方案参数真源、属性解析、缓存 key | Task 1 |
| `be/src/storage/index/inverted/gram/gram_extractor.{h,cpp}` | 稠密/CDC 稀疏/按脚本自适应的 gram 提取 | Task 2 |
| `be/src/storage/index/inverted/gram/gram_query.{h,cpp}` | 布尔查询树、化简、文本序列化 | Task 3 |
| `be/src/storage/index/inverted/gram/regex_ast.{h,cpp}` | RE2 语法子集解析器 | Task 4 |
| `be/src/storage/index/inverted/gram/regex_gram_compiler.{h,cpp}` | Cox 五元组推导、LIKE 编译 | Task 5 |
| `be/test/storage/index/inverted/gram/*_test.cpp` | 阶段 A 全部 UT + 差分模糊测试 | Task 1–6 |
| `be/src/storage/index/inverted/analysis/tokenizer/ngram/gram_tokenizer.{h,cpp}` | `DorisTokenizer` 适配：把 `GramExtractor` 接进索引策略框架 | Task 7 |
| `be/src/storage/index/inverted/analysis/tokenizer/ngram/ngram_tokenizer_factory.cpp` | 读 `mode/density/stop_gram_df`，分派到 `GramTokenizer` | Task 7 |
| `be/src/storage/index/inverted/inverted_index_writer.cpp` | gram 族强制 DOCS_ONLY；段元数据写 `gram_scheme` | Task 8 |
| `be/src/storage/index/inverted/spimi/spimi_query_executor.{h,cpp}` | `gram_boolean(const GramQuery&)` 位图求值 | Task 9 |
| `be/src/storage/index/inverted/spimi/spimi_fulltext_index_reader.cpp` + 查询类型枚举/参数工厂 | 新查询类型 `GRAM_BOOLEAN_QUERY` 分发与缓存 key | Task 9 |
| `be/src/exprs/function/like.{h,cpp}` | `FunctionLikeBase::evaluate_inverted_index`：取方案、编译、下发查询、标记近似 | Task 10 |
| `be/src/exprs/vexpr.cpp`、`be/src/storage/segment/segment_iterator.cpp` | 近似索引结果：保留表达式做复验；profile 计数 | Task 11 |
| `gensrc/proto/*.proto` 或 V4 段元数据 | `gram_scheme` 持久化字段 | Task 12 |
| `fe/fe-core/.../indexpolicy/NGramTokenizerValidator.java` | 新参数校验与组合校验 | Task 13 |
| FE Nereids 下推许可 + `SessionVariable` | `regexp/rlike/like` 允许走索引；`enable_regex_gram_index` | Task 14 |
| `regression-test/suites/inverted_index_p0/gram/` | 端到端语义对照回归 | Task 15 |
| `tools/regex-ngram-model/`（已存在） | 基准与 golden 生成 | Task 16 |

阶段划分：A 纯库（Task 1–6）→ B 写入接入（Task 7–8, 12）→ C 查询接入（Task 9–11）→ D FE（Task 13–14）→ E 回归与验收（Task 15–16）。A 与 D 的 Task 13 互不依赖，可并行。

---


## 阶段 A：纯库代码（无存储/执行依赖，可独立成 PR）

阶段 A 的全部代码放在新目录 `be/src/storage/index/inverted/gram/`，测试放在 `be/test/storage/index/inverted/gram/`，命名空间 `doris::segment_v2::gram`。它只依赖 `common/status.h`、`gutil`、STL 与 re2（仅测试用）。

### Task 1: GramScheme —— 方案参数的唯一真源

**Files:**
- Create: `be/src/storage/index/inverted/gram/gram_scheme.h`
- Create: `be/src/storage/index/inverted/gram/gram_scheme.cpp`
- Test: `be/test/storage/index/inverted/gram/gram_scheme_test.cpp`

**Interfaces:**
- Produces（后续所有任务依赖）:
  ```cpp
  namespace doris::segment_v2::gram {
  enum class GramMode : uint8_t { DENSE = 1, SPARSE = 2 };  // auto 在写入侧解析成二者之一后再落盘
  struct GramScheme {
      GramMode mode = GramMode::SPARSE;
      uint32_t min_len = 3;            // n（字节）
      uint32_t max_len = 16;           // L（字节，仅 SPARSE）
      uint32_t density_permille = 250; // p×1000（仅 SPARSE）
      uint32_t stop_df_permille = 100; // τ×1000，0 = 不裁剪
      bool lower_case = false;
      uint32_t hash_version = 1;
      // 从 tokenizer/索引属性构造；缺省值同上；非法值返回 InvalidArgument
      static Status from_properties(const std::map<std::string, std::string>& props, GramScheme* out);
      // 写回属性（用于段元数据与缓存 key）
      std::map<std::string, std::string> to_properties() const;
      std::string cache_key() const;   // 形如 "gram:v1:sparse:3:16:250:100:lc0"
      bool operator==(const GramScheme& o) const;
  };
  }  // namespace
  ```

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/storage/index/inverted/gram/gram_scheme_test.cpp
#include "storage/index/inverted/gram/gram_scheme.h"

#include <gtest/gtest.h>

namespace doris::segment_v2::gram {

TEST(GramSchemeTest, DefaultsAndRoundTrip) {
    GramScheme s;
    EXPECT_EQ(s.mode, GramMode::SPARSE);
    EXPECT_EQ(s.min_len, 3u);
    EXPECT_EQ(s.max_len, 16u);
    EXPECT_EQ(s.density_permille, 250u);
    GramScheme back;
    ASSERT_TRUE(GramScheme::from_properties(s.to_properties(), &back).ok());
    EXPECT_TRUE(s == back);
    EXPECT_EQ(s.cache_key(), "gram:v1:sparse:3:16:250:100:lc0");
}

TEST(GramSchemeTest, ParsesTokenizerProperties) {
    std::map<std::string, std::string> props = {{"mode", "dense"},   {"min_gram", "4"},
                                                {"max_gram", "24"},  {"density", "0.33"},
                                                {"stop_gram_df", "0.25"}, {"lower_case", "true"}};
    GramScheme s;
    ASSERT_TRUE(GramScheme::from_properties(props, &s).ok());
    EXPECT_EQ(s.mode, GramMode::DENSE);
    EXPECT_EQ(s.min_len, 4u);
    EXPECT_EQ(s.max_len, 24u);
    EXPECT_EQ(s.density_permille, 330u);
    EXPECT_EQ(s.stop_df_permille, 250u);
    EXPECT_TRUE(s.lower_case);
}

TEST(GramSchemeTest, RejectsInvalid) {
    GramScheme s;
    EXPECT_FALSE(GramScheme::from_properties({{"mode", "fuzzy"}}, &s).ok());
    EXPECT_FALSE(GramScheme::from_properties({{"min_gram", "0"}}, &s).ok());
    EXPECT_FALSE(GramScheme::from_properties({{"min_gram", "8"}, {"max_gram", "4"}}, &s).ok());
    EXPECT_FALSE(GramScheme::from_properties({{"density", "0"}}, &s).ok());
    EXPECT_FALSE(GramScheme::from_properties({{"density", "1.5"}}, &s).ok());
    EXPECT_FALSE(GramScheme::from_properties({{"stop_gram_df", "-1"}}, &s).ok());
}

}  // namespace doris::segment_v2::gram
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=GramSchemeTest.*`（参数名以阶段 B 勘察结论为准；首次需 `--build`）
Expected: 编译失败，找不到 `gram_scheme.h`

- [ ] **Step 3: 最小实现**

```cpp
// be/src/storage/index/inverted/gram/gram_scheme.h
#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "common/status.h"

namespace doris::segment_v2::gram {

enum class GramMode : uint8_t { DENSE = 1, SPARSE = 2 };

struct GramScheme {
    GramMode mode = GramMode::SPARSE;
    uint32_t min_len = 3;
    uint32_t max_len = 16;
    uint32_t density_permille = 250;
    uint32_t stop_df_permille = 100;
    bool lower_case = false;
    uint32_t hash_version = 1;

    static Status from_properties(const std::map<std::string, std::string>& props, GramScheme* out);
    std::map<std::string, std::string> to_properties() const;
    std::string cache_key() const;
    bool operator==(const GramScheme& o) const {
        return mode == o.mode && min_len == o.min_len && max_len == o.max_len &&
               density_permille == o.density_permille && stop_df_permille == o.stop_df_permille &&
               lower_case == o.lower_case && hash_version == o.hash_version;
    }
};

}  // namespace doris::segment_v2::gram
```

```cpp
// be/src/storage/index/inverted/gram/gram_scheme.cpp
#include "storage/index/inverted/gram/gram_scheme.h"

#include <fmt/format.h>

#include <cmath>

namespace doris::segment_v2::gram {

namespace {
Status parse_uint(const std::string& key, const std::string& v, uint32_t lo, uint32_t hi, uint32_t* out) {
    char* end = nullptr;
    long x = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || x < (long)lo || x > (long)hi) {
        return Status::InvalidArgument("gram property {}={} out of range [{},{}]", key, v, lo, hi);
    }
    *out = (uint32_t)x;
    return Status::OK();
}
Status parse_permille(const std::string& key, const std::string& v, double lo, double hi, uint32_t* out) {
    char* end = nullptr;
    double x = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0' || !(x >= lo && x <= hi)) {
        return Status::InvalidArgument("gram property {}={} out of range [{},{}]", key, v, lo, hi);
    }
    *out = (uint32_t)std::lround(x * 1000.0);
    return Status::OK();
}
}  // namespace

Status GramScheme::from_properties(const std::map<std::string, std::string>& props, GramScheme* out) {
    GramScheme s;
    if (auto it = props.find("mode"); it != props.end()) {
        if (it->second == "sparse" || it->second == "auto") {
            s.mode = GramMode::SPARSE;  // auto 在写入侧按段样本解析；库级默认 sparse
        } else if (it->second == "dense") {
            s.mode = GramMode::DENSE;
        } else {
            return Status::InvalidArgument("gram property mode={} must be auto|sparse|dense", it->second);
        }
    }
    if (auto it = props.find("min_gram"); it != props.end()) {
        RETURN_IF_ERROR(parse_uint("min_gram", it->second, 1, 64, &s.min_len));
    }
    if (auto it = props.find("max_gram"); it != props.end()) {
        RETURN_IF_ERROR(parse_uint("max_gram", it->second, 1, 256, &s.max_len));
    }
    if (s.max_len < s.min_len) {
        return Status::InvalidArgument("gram property max_gram({}) < min_gram({})", s.max_len, s.min_len);
    }
    if (auto it = props.find("density"); it != props.end()) {
        RETURN_IF_ERROR(parse_permille("density", it->second, 0.001, 1.0, &s.density_permille));
    }
    if (auto it = props.find("stop_gram_df"); it != props.end()) {
        RETURN_IF_ERROR(parse_permille("stop_gram_df", it->second, 0.0, 1.0, &s.stop_df_permille));
    }
    if (auto it = props.find("lower_case"); it != props.end()) {
        s.lower_case = (it->second == "true" || it->second == "1");
    }
    if (auto it = props.find("hash_version"); it != props.end()) {
        RETURN_IF_ERROR(parse_uint("hash_version", it->second, 1, 1, &s.hash_version));
    }
    *out = s;
    return Status::OK();
}

std::map<std::string, std::string> GramScheme::to_properties() const {
    return {{"mode", mode == GramMode::DENSE ? "dense" : "sparse"},
            {"min_gram", std::to_string(min_len)},
            {"max_gram", std::to_string(max_len)},
            {"density", fmt::format("{:.3f}", density_permille / 1000.0)},
            {"stop_gram_df", fmt::format("{:.3f}", stop_df_permille / 1000.0)},
            {"lower_case", lower_case ? "true" : "false"},
            {"hash_version", std::to_string(hash_version)}};
}

std::string GramScheme::cache_key() const {
    return fmt::format("gram:v{}:{}:{}:{}:{}:{}:lc{}", hash_version,
                       mode == GramMode::DENSE ? "dense" : "sparse", min_len, max_len,
                       density_permille, stop_df_permille, lower_case ? 1 : 0);
}

}  // namespace doris::segment_v2::gram
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter=GramSchemeTest.*`
Expected: 3 个用例 PASS

- [ ] **Step 5: 提交**

```bash
git add be/src/storage/index/inverted/gram/gram_scheme.h be/src/storage/index/inverted/gram/gram_scheme.cpp be/test/storage/index/inverted/gram/gram_scheme_test.cpp
git commit -m "feat(be): gram index scheme definition (GramScheme) with property parsing"
```

---

### Task 2: GramExtractor —— 按脚本自适应的稠密 / CDC 稀疏 gram 提取

**Files:**
- Create: `be/src/storage/index/inverted/gram/gram_extractor.h`
- Create: `be/src/storage/index/inverted/gram/gram_extractor.cpp`
- Test: `be/test/storage/index/inverted/gram/gram_extractor_test.cpp`

**Interfaces:**
- Consumes: Task 1 `GramScheme`
- Produces:
  ```cpp
  class GramExtractor {
  public:
      explicit GramExtractor(const GramScheme& scheme);
      // 对一个列值提取 gram；返回的 string_view 指向提取器内部缓冲，下一次 extract 前有效。
      // 行内已去重且按出现顺序稳定。lower_case=true 时先对输入做 ASCII 折叠再切分（边界哈希在折叠后计算）。
      void extract(std::string_view value, std::vector<std::string_view>* out);
      // 查询侧使用：只返回「窗口完整落在 s 内」的 gram（与 extract 相同规则，供编译器折叠字面量）
      void grams_of_literal(std::string_view s, std::vector<std::string>* out) { /* 等价于 extract */ }
      const GramScheme& scheme() const;
      // 边界判定：字节对 (a,b) 是否为边界。65536 项位图，按 (hash_version, density) 构造。
      bool is_boundary(uint8_t a, uint8_t b) const;
  };
  ```

规则（与设计文档 §6.1.1–§6.1.3 逐条对应）：
1. 值先按 UTF-8 解码切成「ASCII 段」与「非 ASCII 码点」；非法 UTF-8 字节序列按单字节处理，作为非 ASCII 码点产出 1-gram。
2. 非 ASCII 码点 → 一个 gram = 该码点的 UTF-8 字节。
3. ASCII 段（长度 ≥ min_len）：DENSE → 每个位置一个 `min_len` 字节窗口；SPARSE → 设计文档 §6.1.2 的 CDC 规则（边界 k 起，延伸到首个使 `j+2−k ≥ min_len` 的边界 j，gram = `[k, j+2)`；`max_len` 内无此边界则取 `[k, k+max_len)`；剩余不足 `max_len` 不产出）。
4. 行内去重（保序）。
5. 边界哈希 v1：`mix64((((uint64)a << 8) | b) ^ 0x5bd1e995) & 0xFFFF < density_permille * 65536 / 1000`，`mix64` 为 splitmix64 finalizer（与原型 `tools/regex-ngram-model/ngram_model.cpp` 一致，golden 由原型生成）。

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/storage/index/inverted/gram/gram_extractor_test.cpp
#include "storage/index/inverted/gram/gram_extractor.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace doris::segment_v2::gram {

static std::vector<std::string> run(const GramScheme& s, std::string_view v) {
    GramExtractor ex(s);
    std::vector<std::string_view> out;
    ex.extract(v, &out);
    return {out.begin(), out.end()};
}

TEST(GramExtractorTest, DenseAsciiTrigrams) {
    GramScheme s;
    s.mode = GramMode::DENSE;
    s.min_len = 3;
    EXPECT_EQ(run(s, "abcde"), (std::vector<std::string> {"abc", "bcd", "cde"}));
    EXPECT_EQ(run(s, "ab"), (std::vector<std::string> {}));           // 短于 n 不产出
    EXPECT_EQ(run(s, "aaaa"), (std::vector<std::string> {"aaa"}));    // 行内去重
}

TEST(GramExtractorTest, NonAsciiCodepointsAreUnigrams) {
    GramScheme s;
    s.mode = GramMode::DENSE;
    // "手机ab微博" → 手, 机, 微, 博；ASCII 段 "ab" 短于 3 不产出
    EXPECT_EQ(run(s, "手机ab微博"), (std::vector<std::string> {"手", "机", "微", "博"}));
    // 非法 UTF-8 字节 0xFF 作为单字节 1-gram
    EXPECT_EQ(run(s, std::string("\xFF", 1)), (std::vector<std::string> {std::string("\xFF", 1)}));
}

TEST(GramExtractorTest, SparseIsLocalAndDeterministic) {
    GramScheme s;  // sparse p=0.25 n=3 L=16
    GramExtractor ex(s);
    // 局部性：任意子串的 gram 集合 ⊆ 全串的 gram 集合
    const std::string doc = "rpc error: code = Unavailable desc = error reading from server";
    std::vector<std::string_view> all;
    ex.extract(doc, &all);
    std::set<std::string> whole(all.begin(), all.end());
    for (size_t i = 0; i < doc.size(); i++) {
        for (size_t len = 3; i + len <= doc.size(); len++) {
            std::vector<std::string_view> sub;
            ex.extract(std::string_view(doc).substr(i, len), &sub);
            for (auto g : sub) {
                EXPECT_TRUE(whole.count(std::string(g))) << "gram '" << g << "' of substring [" << i
                                                        << "," << len << ") missing from whole";
            }
        }
    }
    // 密度：0.25 时 gram 数明显少于稠密 (len-2)
    EXPECT_LT(all.size(), (doc.size() - 2) / 2);
}

TEST(GramExtractorTest, SparseGoldenFromPrototype) {
    // golden 由 tools/regex-ngram-model/ngram_model.cpp --cdc --p 0.25 --maxlen 16 --n 3 生成
    GramScheme s;
    EXPECT_EQ(run(s, "rpc error: code = Unavailable"),
              (std::vector<std::string> {"or: co", "cod", "ode = U", " Unavai", "ailable"}));
}

TEST(GramExtractorTest, LowerCaseFoldsBeforeBoundaryHash) {
    GramScheme s;
    s.lower_case = true;
    EXPECT_EQ(run(s, "Code = Unavailable"), run(s, "code = unavailable"));
}

TEST(GramExtractorTest, BoundaryTableMatchesFormula) {
    GramScheme s;
    GramExtractor ex(s);
    size_t cnt = 0;
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b++) cnt += ex.is_boundary(a, b);
    // p=0.25 ± 2%
    EXPECT_NEAR(cnt / 65536.0, 0.25, 0.02);
}

}  // namespace doris::segment_v2::gram
```

注意：`SparseGoldenFromPrototype` 的期望值来自原型 `--explain 'rpc error: code = Unavailable'`（p=0.25，maxlen 24 与 16 对该串结果相同）；实现时若 gram 顺序按起点排序，请把期望值改为按起点顺序 `"cod","ode = U"," Unavai","ailable","or: co"`——以原型 `BENCH_EXTRACT` 之外的 `grams_of_cdc` 输出顺序（按起点）为准。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=GramExtractorTest.*`
Expected: 编译失败，找不到 `gram_extractor.h`

- [ ] **Step 3: 最小实现**

```cpp
// be/src/storage/index/inverted/gram/gram_extractor.h
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "storage/index/inverted/gram/gram_scheme.h"

namespace doris::segment_v2::gram {

class GramExtractor {
public:
    explicit GramExtractor(const GramScheme& scheme);
    void extract(std::string_view value, std::vector<std::string_view>* out);
    void grams_of_literal(std::string_view s, std::vector<std::string>* out);
    const GramScheme& scheme() const { return _scheme; }
    bool is_boundary(uint8_t a, uint8_t b) const {
        unsigned idx = ((unsigned)a << 8) | b;
        return (_boundary_bits[idx >> 3] >> (idx & 7)) & 1;
    }

private:
    void _build_boundary_table();
    void _ascii_segment(std::string_view seg, std::vector<std::string_view>* out);
    void _dedupe(std::vector<std::string_view>* out);

    GramScheme _scheme;
    std::vector<uint8_t> _boundary_bits;  // 65536 bit
    std::string _folded;                   // lower_case 时的折叠副本，输出 view 指向它
    std::vector<uint8_t> _is_boundary_at;  // 每个位置的边界标记（复用缓冲）
};

}  // namespace doris::segment_v2::gram
```

```cpp
// be/src/storage/index/inverted/gram/gram_extractor.cpp
#include "storage/index/inverted/gram/gram_extractor.h"

#include <algorithm>
#include <unordered_set>

namespace doris::segment_v2::gram {

namespace {
inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
inline int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}
// 返回从 p 开始的一个合法 UTF-8 码点的字节长度；非法则 1
inline size_t codepoint_len(const char* p, size_t remain) {
    int l = utf8_len((unsigned char)p[0]);
    if (l == 1 || (size_t)l > remain) return 1;
    for (int k = 1; k < l; k++) {
        if (((unsigned char)p[k] & 0xC0) != 0x80) return 1;
    }
    return l;
}
}  // namespace

GramExtractor::GramExtractor(const GramScheme& scheme) : _scheme(scheme) {
    _build_boundary_table();
}

void GramExtractor::_build_boundary_table() {
    _boundary_bits.assign(8192, 0);
    const uint64_t threshold = (uint64_t)_scheme.density_permille * 65536ULL / 1000ULL;
    for (unsigned idx = 0; idx < 65536; idx++) {
        uint64_t key = ((uint64_t)idx) ^ 0x5bd1e995ULL;  // idx = (a<<8)|b
        if ((mix64(key) & 0xFFFF) < threshold) {
            _boundary_bits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
        }
    }
}

void GramExtractor::_ascii_segment(std::string_view seg, std::vector<std::string_view>* out) {
    const size_t L = seg.size();
    const size_t n = _scheme.min_len;
    if (L < n) return;
    if (_scheme.mode == GramMode::DENSE) {
        for (size_t i = 0; i + n <= L; i++) out->push_back(seg.substr(i, n));
        return;
    }
    const size_t maxlen = _scheme.max_len;
    _is_boundary_at.assign(L, 0);
    for (size_t i = 0; i + 1 < L; i++) {
        _is_boundary_at[i] = is_boundary((uint8_t)seg[i], (uint8_t)seg[i + 1]);
    }
    for (size_t k = 0; k + 1 < L; k++) {
        if (!_is_boundary_at[k]) continue;
        size_t end = 0;
        for (size_t j = k + 1; j + 1 < L && j + 2 - k <= maxlen; j++) {
            if (_is_boundary_at[j] && j + 2 - k >= n) {
                end = j + 2;
                break;
            }
        }
        if (end == 0) {
            if (k + maxlen <= L) {
                end = k + maxlen;
            } else {
                continue;
            }
        }
        out->push_back(seg.substr(k, end - k));
    }
}

void GramExtractor::_dedupe(std::vector<std::string_view>* out) {
    std::unordered_set<std::string_view> seen;
    size_t w = 0;
    for (size_t r = 0; r < out->size(); r++) {
        if (seen.insert((*out)[r]).second) (*out)[w++] = (*out)[r];
    }
    out->resize(w);
}

void GramExtractor::extract(std::string_view value, std::vector<std::string_view>* out) {
    out->clear();
    if (_scheme.lower_case) {
        _folded.assign(value.data(), value.size());
        for (auto& ch : _folded) {
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        }
        value = _folded;
    }
    size_t i = 0;
    const size_t L = value.size();
    while (i < L) {
        if ((unsigned char)value[i] < 0x80) {
            size_t j = i;
            while (j < L && (unsigned char)value[j] < 0x80) j++;
            _ascii_segment(value.substr(i, j - i), out);
            i = j;
        } else {
            size_t l = codepoint_len(value.data() + i, L - i);
            out->push_back(value.substr(i, l));
            i += l;
        }
    }
    _dedupe(out);
}

void GramExtractor::grams_of_literal(std::string_view s, std::vector<std::string>* out) {
    std::vector<std::string_view> tmp;
    extract(s, &tmp);
    out->assign(tmp.begin(), tmp.end());
}

}  // namespace doris::segment_v2::gram
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter=GramExtractorTest.*`
Expected: 6 个用例 PASS（若 `SparseGoldenFromPrototype` 因顺序失败，按上文说明修正期望顺序，不改实现）

- [ ] **Step 5: 提交**

```bash
git add be/src/storage/index/inverted/gram/gram_extractor.h be/src/storage/index/inverted/gram/gram_extractor.cpp be/test/storage/index/inverted/gram/gram_extractor_test.cpp
git commit -m "feat(be): script-aware dense/sparse (CDC) gram extractor"
```

---

### Task 3: GramQuery —— 布尔查询树、化简与文本序列化

**Files:**
- Create: `be/src/storage/index/inverted/gram/gram_query.h`
- Create: `be/src/storage/index/inverted/gram/gram_query.cpp`
- Test: `be/test/storage/index/inverted/gram/gram_query_test.cpp`

**Interfaces:**
- Produces（编译器与查询执行器都依赖）:
  ```cpp
  struct GramQuery {
      enum class Op : uint8_t { ALL, NONE, AND, OR };
      Op op = Op::ALL;
      std::vector<std::string> grams;   // 本节点直接持有的 gram
      std::vector<GramQuery> subs;      // 子查询
      static GramQuery all();  static GramQuery none();
      static GramQuery of_gram(std::string g);            // 单 gram = AND{g}
      static GramQuery and_(GramQuery a, GramQuery b);    // 化简：扁平化、去重、吸收律、NONE/ALL 短路
      static GramQuery or_(GramQuery a, GramQuery b);
      bool is_all() const;  bool is_none() const;
      size_t leaf_count() const;
      // 文本格式（用于 InvertedIndexParam::query_value 与缓存 key）：
      //   ALL → "*", NONE → "!", AND → "&(" items ")", OR → "|(" items ")",
      //   gram 用 base64 编码避免分隔符冲突；items 以 ',' 分隔
      std::string serialize() const;
      static Status parse(std::string_view text, GramQuery* out);
      std::string to_debug_string() const;  // 可读形式，EXPLAIN 用："(\"abc\" & (\"de\" | \"fg\"))"
  };
  ```

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/storage/index/inverted/gram/gram_query_test.cpp
#include "storage/index/inverted/gram/gram_query.h"

#include <gtest/gtest.h>

namespace doris::segment_v2::gram {

TEST(GramQueryTest, AndOrShortCircuit) {
    EXPECT_TRUE(GramQuery::and_(GramQuery::all(), GramQuery::of_gram("abc")).op == GramQuery::Op::AND);
    EXPECT_TRUE(GramQuery::and_(GramQuery::none(), GramQuery::of_gram("abc")).is_none());
    EXPECT_TRUE(GramQuery::or_(GramQuery::all(), GramQuery::of_gram("abc")).is_all());
    EXPECT_EQ(GramQuery::or_(GramQuery::none(), GramQuery::of_gram("abc")).to_debug_string(), "(\"abc\")");
}

TEST(GramQueryTest, FlattenDedupeAbsorb) {
    auto q = GramQuery::and_(GramQuery::and_(GramQuery::of_gram("abc"), GramQuery::of_gram("bcd")),
                             GramQuery::of_gram("abc"));
    EXPECT_EQ(q.op, GramQuery::Op::AND);
    EXPECT_EQ(q.grams.size(), 2u);
    // AND 内已有 abc，则子 OR(abc|xyz) 恒真，被吸收
    auto r = GramQuery::and_(q, GramQuery::or_(GramQuery::of_gram("abc"), GramQuery::of_gram("xyz")));
    EXPECT_EQ(r.leaf_count(), 2u);
    // OR 内 AND 子集吸收超集：(a&b) | (a&b&c) → (a&b)
    auto s = GramQuery::or_(GramQuery::and_(GramQuery::of_gram("a"), GramQuery::of_gram("b")),
                            GramQuery::and_(GramQuery::and_(GramQuery::of_gram("a"), GramQuery::of_gram("b")),
                                            GramQuery::of_gram("c")));
    EXPECT_EQ(s.to_debug_string(), "(\"a\" & \"b\")");
}

TEST(GramQueryTest, SerializeRoundTrip) {
    auto q = GramQuery::and_(GramQuery::of_gram("or: co"),
                             GramQuery::or_(GramQuery::and_(GramQuery::of_gram("Una"), GramQuery::of_gram("abl")),
                                            GramQuery::of_gram("Int,ernal)")));
    std::string text = q.serialize();
    GramQuery back;
    ASSERT_TRUE(GramQuery::parse(text, &back).ok());
    EXPECT_EQ(back.serialize(), text);
    EXPECT_EQ(back.to_debug_string(), q.to_debug_string());
    EXPECT_EQ(GramQuery::all().serialize(), "*");
    EXPECT_EQ(GramQuery::none().serialize(), "!");
    GramQuery bad;
    EXPECT_FALSE(GramQuery::parse("&(", &bad).ok());
}

}  // namespace doris::segment_v2::gram
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=GramQueryTest.*`
Expected: 编译失败

- [ ] **Step 3: 最小实现**

```cpp
// be/src/storage/index/inverted/gram/gram_query.h
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace doris::segment_v2::gram {

struct GramQuery {
    enum class Op : uint8_t { ALL, NONE, AND, OR };
    Op op = Op::ALL;
    std::vector<std::string> grams;
    std::vector<GramQuery> subs;

    static GramQuery all() { return GramQuery {}; }
    static GramQuery none() { GramQuery q; q.op = Op::NONE; return q; }
    static GramQuery of_gram(std::string g) { GramQuery q; q.op = Op::AND; q.grams.push_back(std::move(g)); return q; }
    static GramQuery and_(GramQuery a, GramQuery b);
    static GramQuery or_(GramQuery a, GramQuery b);
    bool is_all() const { return op == Op::ALL; }
    bool is_none() const { return op == Op::NONE; }
    size_t leaf_count() const;
    std::string serialize() const;
    static Status parse(std::string_view text, GramQuery* out);
    std::string to_debug_string() const;
    std::string structural_key() const;  // 结构去重用
};

}  // namespace doris::segment_v2::gram
```

```cpp
// be/src/storage/index/inverted/gram/gram_query.cpp
#include "storage/index/inverted/gram/gram_query.h"

#include <algorithm>
#include <set>

#include "gutil/strings/escaping.h"  // Base64Escape / Base64Unescape（若路径不同，改用 util/base64.h 的 base64_encode/decode）

namespace doris::segment_v2::gram {

namespace {
void dedupe_grams(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}
void dedupe_subs(std::vector<GramQuery>& subs) {
    std::set<std::string> seen;
    std::vector<GramQuery> keep;
    for (auto& s : subs) {
        if (seen.insert(s.structural_key()).second) keep.push_back(std::move(s));
    }
    subs = std::move(keep);
}
bool has_gram(const std::vector<std::string>& sorted, const std::string& g) {
    return std::binary_search(sorted.begin(), sorted.end(), g);
}
// OR 内：子 AND A ⊆ 子 AND B 的 gram 集 → B 被 A 蕴含，删 B。先算标记再搬移，避免遍历中 move 掉被比较对象。
void or_absorb_subsets(std::vector<GramQuery>& subs) {
    std::vector<char> drop(subs.size(), 0);
    for (size_t i = 0; i < subs.size(); i++) {
        if (subs[i].op != GramQuery::Op::AND || !subs[i].subs.empty()) continue;
        for (size_t j = 0; j < subs.size() && !drop[i]; j++) {
            if (i == j || subs[j].op != GramQuery::Op::AND || !subs[j].subs.empty()) continue;
            if (subs[j].grams.size() > subs[i].grams.size() ||
                (subs[j].grams.size() == subs[i].grams.size() && j > i)) continue;
            if (std::includes(subs[i].grams.begin(), subs[i].grams.end(), subs[j].grams.begin(), subs[j].grams.end())) drop[i] = 1;
        }
    }
    std::vector<GramQuery> keep;
    for (size_t i = 0; i < subs.size(); i++) if (!drop[i]) keep.push_back(std::move(subs[i]));
    subs = std::move(keep);
}
}  // namespace

GramQuery GramQuery::and_(GramQuery a, GramQuery b) {
    if (a.is_none() || b.is_none()) return none();
    if (a.is_all()) return b;
    if (b.is_all()) return a;
    GramQuery r; r.op = Op::AND;
    for (GramQuery* x : {&a, &b}) {
        if (x->op == Op::AND) {
            r.grams.insert(r.grams.end(), x->grams.begin(), x->grams.end());
            for (auto& s : x->subs) r.subs.push_back(std::move(s));
        } else {
            r.subs.push_back(std::move(*x));
        }
    }
    dedupe_grams(r.grams);
    std::vector<GramQuery> keep;
    for (auto& s : r.subs) {
        bool absorbed = false;
        if (s.op == Op::OR) for (auto& g : s.grams) if (has_gram(r.grams, g)) { absorbed = true; break; }
        if (!absorbed) keep.push_back(std::move(s));
    }
    r.subs = std::move(keep);
    dedupe_subs(r.subs);
    if (r.grams.empty() && r.subs.size() == 1) return r.subs[0];
    return r;
}

GramQuery GramQuery::or_(GramQuery a, GramQuery b) {
    if (a.is_all() || b.is_all()) return all();
    if (a.is_none()) return b;
    if (b.is_none()) return a;
    GramQuery r; r.op = Op::OR;
    for (GramQuery* x : {&a, &b}) {
        if (x->op == Op::OR) {
            r.grams.insert(r.grams.end(), x->grams.begin(), x->grams.end());
            for (auto& s : x->subs) r.subs.push_back(std::move(s));
        } else if (x->op == Op::AND && x->grams.size() == 1 && x->subs.empty()) {
            r.grams.push_back(x->grams[0]);
        } else {
            r.subs.push_back(std::move(*x));
        }
    }
    dedupe_grams(r.grams);
    std::vector<GramQuery> keep;
    for (auto& s : r.subs) {
        bool absorbed = false;
        if (s.op == Op::AND) for (auto& g : s.grams) if (has_gram(r.grams, g)) { absorbed = true; break; }
        if (!absorbed) keep.push_back(std::move(s));
    }
    r.subs = std::move(keep);
    dedupe_subs(r.subs);
    or_absorb_subsets(r.subs);
    if (r.grams.size() == 1 && r.subs.empty()) return of_gram(r.grams[0]);
    if (r.grams.empty() && r.subs.size() == 1) return r.subs[0];
    return r;
}

size_t GramQuery::leaf_count() const {
    size_t c = grams.size();
    for (auto& s : subs) c += s.leaf_count();
    return c;
}

std::string GramQuery::structural_key() const { return serialize(); }

std::string GramQuery::serialize() const {
    if (op == Op::ALL) return "*";
    if (op == Op::NONE) return "!";
    std::string s = op == Op::AND ? "&(" : "|(";
    bool first = true;
    for (auto& g : grams) {
        if (!first) s += ',';
        first = false;
        std::string enc;
        strings::Base64Escape(g, &enc);
        s += enc;
    }
    std::vector<std::string> ks;
    for (auto& c : subs) ks.push_back(c.serialize());
    std::sort(ks.begin(), ks.end());
    for (auto& k : ks) {
        if (!first) s += ',';
        first = false;
        s += k;
    }
    return s + ")";
}

namespace {
Status parse_at(std::string_view t, size_t& i, GramQuery* out) {
    if (i >= t.size()) return Status::InvalidArgument("gram query truncated");
    if (t[i] == '*') { i++; *out = GramQuery::all(); return Status::OK(); }
    if (t[i] == '!') { i++; *out = GramQuery::none(); return Status::OK(); }
    if ((t[i] != '&' && t[i] != '|') || i + 1 >= t.size() || t[i + 1] != '(') {
        return Status::InvalidArgument("gram query bad token at {}", i);
    }
    GramQuery q; q.op = t[i] == '&' ? GramQuery::Op::AND : GramQuery::Op::OR;
    i += 2;
    while (i < t.size() && t[i] != ')') {
        if (t[i] == '&' || t[i] == '|' || t[i] == '*' || t[i] == '!') {
            GramQuery sub;
            RETURN_IF_ERROR(parse_at(t, i, &sub));
            q.subs.push_back(std::move(sub));
        } else {
            size_t j = i;
            while (j < t.size() && t[j] != ',' && t[j] != ')') j++;
            std::string dec;
            if (!strings::Base64Unescape(std::string(t.substr(i, j - i)), &dec)) {
                return Status::InvalidArgument("gram query bad base64 at {}", i);
            }
            q.grams.push_back(std::move(dec));
            i = j;
        }
        if (i < t.size() && t[i] == ',') i++;
    }
    if (i >= t.size() || t[i] != ')') return Status::InvalidArgument("gram query missing ')'");
    i++;
    *out = std::move(q);
    return Status::OK();
}
}  // namespace

Status GramQuery::parse(std::string_view text, GramQuery* out) {
    size_t i = 0;
    RETURN_IF_ERROR(parse_at(text, i, out));
    if (i != text.size()) return Status::InvalidArgument("gram query trailing input");
    return Status::OK();
}

std::string GramQuery::to_debug_string() const {
    if (op == Op::ALL) return "ALL";
    if (op == Op::NONE) return "NONE";
    std::string sep = op == Op::AND ? " & " : " | ";
    std::string s = "(";
    bool first = true;
    for (auto& g : grams) { if (!first) s += sep; first = false; s += "\"" + g + "\""; }
    for (auto& c : subs) { if (!first) s += sep; first = false; s += c.to_debug_string(); }
    return s + ")";
}

}  // namespace doris::segment_v2::gram
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter=GramQueryTest.*`
Expected: 3 个用例 PASS（`gutil/strings/escaping.h` 若不存在，改用 `util/base64.h` 的 `base64_encode/base64_decode`，接口对应替换）

- [ ] **Step 5: 提交**

```bash
git add be/src/storage/index/inverted/gram/gram_query.h be/src/storage/index/inverted/gram/gram_query.cpp be/test/storage/index/inverted/gram/gram_query_test.cpp
git commit -m "feat(be): gram boolean query tree with simplification and text serialization"
```

---

### Task 4: RegexAst —— RE2 语法子集的递归下降解析器

**Files:**
- Create: `be/src/storage/index/inverted/gram/regex_ast.h`
- Create: `be/src/storage/index/inverted/gram/regex_ast.cpp`
- Test: `be/test/storage/index/inverted/gram/regex_ast_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct RegexNode {
      enum class Type : uint8_t { EMPTY, LIT, CLASS, ANY, CAT, ALT, STAR, PLUS, QUEST, REPEAT };
      Type type = Type::EMPTY;
      std::string lit;                    // LIT：一个码点的 UTF-8
      std::vector<std::string> cls;       // CLASS：≤4 个码点的小类展开；空 + big_class=true 表示大类/取反类
      bool big_class = false;
      std::vector<std::unique_ptr<RegexNode>> kids;
      int rmin = 0, rmax = -1;            // REPEAT
  };
  // 解析失败返回 InvalidArgument（调用方据此保守回退 ALL）；支持的语法见设计文档 §6.3.2
  Status parse_regex(std::string_view pattern, std::unique_ptr<RegexNode>* root, bool* case_insensitive);
  ```

语法覆盖：字面量、转义（`\. \n \t \r \xHH \x{...} \Q..\E`）、类（`[...]`、取反、区间、POSIX 类、`\d \w \s \D \W \S \pL \p{..}`）、`.`、分组（捕获/`(?:`/`(?P<name>`/`(?<name>`）、标志 `(?i) (?s) (?m) (?U)` 与 `(?i:...)`、量词 `* + ? {m} {m,} {m,n}` 及懒惰后缀、锚点 `^ $ \b \B \A \z`、`|`。`(?i)` 标志下 ASCII 字母字面量产出 CLASS{c, C}。

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/storage/index/inverted/gram/regex_ast_test.cpp
#include "storage/index/inverted/gram/regex_ast.h"

#include <gtest/gtest.h>

namespace doris::segment_v2::gram {

static std::string dump(const RegexNode* n) {
    using T = RegexNode::Type;
    switch (n->type) {
    case T::EMPTY: return "e";
    case T::LIT: return "'" + n->lit + "'";
    case T::CLASS: {
        if (n->big_class) return "[big]";
        std::string s = "[";
        for (auto& c : n->cls) s += c;
        return s + "]";
    }
    case T::ANY: return ".";
    case T::CAT: { std::string s = "cat("; for (auto& k : n->kids) s += dump(k.get()) + ","; return s + ")"; }
    case T::ALT: { std::string s = "alt("; for (auto& k : n->kids) s += dump(k.get()) + ","; return s + ")"; }
    case T::STAR: return "star(" + dump(n->kids[0].get()) + ")";
    case T::PLUS: return "plus(" + dump(n->kids[0].get()) + ")";
    case T::QUEST: return "quest(" + dump(n->kids[0].get()) + ")";
    case T::REPEAT: return "rep(" + dump(n->kids[0].get()) + "," + std::to_string(n->rmin) + "," + std::to_string(n->rmax) + ")";
    }
    return "?";
}

static std::string parse_dump(const std::string& re) {
    std::unique_ptr<RegexNode> root;
    bool icase = false;
    Status st = parse_regex(re, &root, &icase);
    if (!st.ok()) return "ERR";
    return dump(root.get());
}

TEST(RegexAstTest, Basics) {
    EXPECT_EQ(parse_dump("abc"), "cat('a','b','c',)");
    EXPECT_EQ(parse_dump("a|bc"), "alt(cat('a',),cat('b','c',),)");
    EXPECT_EQ(parse_dump("a.*b"), "cat('a',star(.),'b',)");
    EXPECT_EQ(parse_dump("(ab)+c?"), "cat(plus(cat('a','b',)),quest('c'),)");
    EXPECT_EQ(parse_dump("x{2,3}"), "cat(rep('x',2,3),)");
    EXPECT_EQ(parse_dump("^\\d{3}-\\d{4}$"), "cat(e,rep([big],3,3),'-',rep([big],4,4),e,)");
}

TEST(RegexAstTest, ClassesAndEscapes) {
    EXPECT_EQ(parse_dump("[ab]"), "cat([ab],)");
    EXPECT_EQ(parse_dump("[a-z]"), "cat([big],)");
    EXPECT_EQ(parse_dump("[^a]"), "cat([big],)");
    EXPECT_EQ(parse_dump("\\.\\Qa.b\\E"), "cat('.',cat('a','.','b',),)");
    EXPECT_EQ(parse_dump("\\x41"), "cat('A',)");
    EXPECT_EQ(parse_dump("手机"), "cat('手','机',)");
}

TEST(RegexAstTest, FlagsAndErrors) {
    std::unique_ptr<RegexNode> root; bool icase = false;
    ASSERT_TRUE(parse_regex("(?i)ab", &root, &icase).ok());
    EXPECT_TRUE(icase);
    EXPECT_EQ(dump(root.get()), "cat([Aa],[Bb],)");
    EXPECT_EQ(parse_dump("(ab"), "ERR");
    EXPECT_EQ(parse_dump("*a"), "ERR");
    EXPECT_EQ(parse_dump("[ab"), "ERR");
    EXPECT_EQ(parse_dump("a\\"), "ERR");
}

}  // namespace doris::segment_v2::gram
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=RegexAstTest.*`
Expected: 编译失败

- [ ] **Step 3: 实现**

把原型 `tools/regex-ngram-model/ngram_model.cpp` 中的 `struct Parser`（`parse/parse_alt/parse_cat/parse_quant/parse_atom/parse_class/class_escape/make_lit/next_cp`，约 250 行）逐函数移植为 `regex_ast.cpp` 内的匿名命名空间类 `Parser`，改动仅三处：`Node` → `RegexNode`（字段同名）；`ok/err` 改为返回 `Status::InvalidArgument(err)`；UTF-8 解码/编码函数从原型 `decode_cps/encode_cp` 复制到匿名命名空间。头文件：

```cpp
// be/src/storage/index/inverted/gram/regex_ast.h
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace doris::segment_v2::gram {

struct RegexNode {
    enum class Type : uint8_t { EMPTY, LIT, CLASS, ANY, CAT, ALT, STAR, PLUS, QUEST, REPEAT };
    Type type = Type::EMPTY;
    std::string lit;
    std::vector<std::string> cls;
    bool big_class = false;
    std::vector<std::unique_ptr<RegexNode>> kids;
    int rmin = 0, rmax = -1;
};

Status parse_regex(std::string_view pattern, std::unique_ptr<RegexNode>* root, bool* case_insensitive);

}  // namespace doris::segment_v2::gram
```

`parse_regex` 的实现骨架（其余按原型逐行移植）：

```cpp
Status parse_regex(std::string_view pattern, std::unique_ptr<RegexNode>* root, bool* case_insensitive) {
    Parser p(pattern);
    std::unique_ptr<RegexNode> r = p.parse();
    if (!p.ok) return Status::InvalidArgument("regex parse error: {}", p.err);
    *case_insensitive = p.icase;
    *root = std::move(r);
    return Status::OK();
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter=RegexAstTest.*`
Expected: 3 个用例 PASS

- [ ] **Step 5: 提交**

```bash
git add be/src/storage/index/inverted/gram/regex_ast.h be/src/storage/index/inverted/gram/regex_ast.cpp be/test/storage/index/inverted/gram/regex_ast_test.cpp
git commit -m "feat(be): RE2-subset regex parser producing an AST for gram compilation"
```

---

### Task 5: RegexGramCompiler —— Cox 五元组推导 + LIKE 编译

**Files:**
- Create: `be/src/storage/index/inverted/gram/regex_gram_compiler.h`
- Create: `be/src/storage/index/inverted/gram/regex_gram_compiler.cpp`
- Test: `be/test/storage/index/inverted/gram/regex_gram_compiler_test.cpp`

**Interfaces:**
- Consumes: Task 2 `GramExtractor::grams_of_literal`，Task 3 `GramQuery`，Task 4 `parse_regex`
- Produces:
  ```cpp
  class RegexGramCompiler {
  public:
      explicit RegexGramCompiler(const GramScheme& scheme);
      // 任何解析/推导失败都返回 OK 且 *out = ALL（保守）；只有内部断言失败才返回非 OK
      Status compile_regexp(std::string_view pattern, GramQuery* out);
      // LIKE：% 与 _ 处切断为字面量段，\ 转义；每段 grams AND；ILIKE 语义由调用方在 scheme.lower_case 下决定
      Status compile_like(std::string_view like_pattern, GramQuery* out);
      static constexpr size_t kMaxSet = 20;
      static constexpr size_t kMaxExact = 7;
  };
  ```

算法：设计文档 §6.3.3 的表格逐行实现（`Info{can_empty, has_exact, exact, prefix, suffix, match}`、`concat_info/alt_info/plus_info/simplify/trim_set/demote`），与原型 `ngram_model.cpp` 中同名函数一一对应；`grams_of(s)` 改为调用 `GramExtractor::grams_of_literal`；裁剪保留长度 `keep = scheme.mode==SPARSE ? scheme.max_len : scheme.min_len-1`。`(?i)` 下若 `scheme.lower_case=false`，字面量按 CLASS{c,C} 展开（Cox 做法）；若 `scheme.lower_case=true`，先把模式里的 ASCII 字母折叠为小写再按普通字面量处理。

- [ ] **Step 1: 写失败测试（golden 来自原型 `--explain`，稠密 n=3 与稀疏 p=0.25/L=24）**

```cpp
// be/test/storage/index/inverted/gram/regex_gram_compiler_test.cpp
#include "storage/index/inverted/gram/regex_gram_compiler.h"

#include <gtest/gtest.h>

namespace doris::segment_v2::gram {

static std::string dense(const std::string& re) {
    GramScheme s; s.mode = GramMode::DENSE; s.min_len = 3;
    RegexGramCompiler c(s); GramQuery q;
    EXPECT_TRUE(c.compile_regexp(re, &q).ok());
    return q.to_debug_string();
}
static std::string sparse(const std::string& re) {
    GramScheme s; s.max_len = 24;
    RegexGramCompiler c(s); GramQuery q;
    EXPECT_TRUE(c.compile_regexp(re, &q).ok());
    return q.to_debug_string();
}

TEST(RegexGramCompilerTest, DenseGolden) {
    EXPECT_EQ(dense("abc"), "(\"abc\")");
    EXPECT_EQ(dense("a.*b"), "ALL");
    EXPECT_EQ(dense("\\d{3}-\\d{4}"), "ALL");
    EXPECT_EQ(dense("hello|world"), "((\"ell\" & \"hel\" & \"llo\") | (\"orl\" & \"rld\" & \"wor\"))");
    EXPECT_EQ(dense("(foo|bar)baz"), "((\"arb\" & \"bar\" & \"baz\" & \"rba\") | (\"baz\" & \"foo\" & \"oba\" & \"oob\"))");
    EXPECT_EQ(dense("conn(ection)? re(set|fused)"),
              "(\" re\" & \"con\" & \"onn\" & ((\"efu\" & \"fus\" & \"ref\" & \"sed\" & \"use\") | (\"ese\" & \"res\" & \"set\")))");
    EXPECT_EQ(dense("GET|POST"), "(\"GET\" | (\"OST\" & \"POS\"))");
    EXPECT_EQ(dense("a(b|cd)e"), "(\"abe\" | (\"acd\" & \"cde\"))");
    EXPECT_EQ(dense("[ab]cd"), "(\"acd\" | \"bcd\")");
    EXPECT_EQ(dense("(abc){2}"), "(\"abc\" & \"bca\" & \"cab\")");
    EXPECT_EQ(dense("error.*timeout"), "(\"eou\" & \"err\" & \"ime\" & \"meo\" & \"out\" & \"ror\" & \"rro\" & \"tim\")");
}

TEST(RegexGramCompilerTest, SparseGolden) {
    EXPECT_EQ(sparse("rpc error: code = Unavailable"), "(\"or: co\" & \" Unavai\" & \"ailable\" & \"cod\" & \"ode = U\")");
    EXPECT_EQ(sparse("error.*timeout"), "(\"timeo\")");
    EXPECT_EQ(sparse("GET|POST"), "ALL");
}

TEST(RegexGramCompilerTest, ParseErrorIsAll) {
    GramScheme s; RegexGramCompiler c(s); GramQuery q;
    ASSERT_TRUE(c.compile_regexp("(ab", &q).ok());
    EXPECT_TRUE(q.is_all());
}

TEST(RegexGramCompilerTest, CaseInsensitiveWithAndWithoutFolding) {
    GramScheme s; s.mode = GramMode::DENSE;
    RegexGramCompiler c(s); GramQuery q;
    ASSERT_TRUE(c.compile_regexp("(?i)abcd", &q).ok());
    EXPECT_EQ(q.to_debug_string(),
              "((\"ABC\" | \"ABc\" | \"AbC\" | \"Abc\" | \"aBC\" | \"aBc\" | \"abC\" | \"abc\") & "
              "(\"BCD\" | \"BCd\" | \"BcD\" | \"Bcd\" | \"bCD\" | \"bCd\" | \"bcD\" | \"bcd\"))");
    s.lower_case = true;
    RegexGramCompiler c2(s);
    ASSERT_TRUE(c2.compile_regexp("(?i)ABCD", &q).ok());
    EXPECT_EQ(q.to_debug_string(), "(\"abc\" & \"bcd\")");
}

TEST(RegexGramCompilerTest, Like) {
    GramScheme s; s.mode = GramMode::DENSE;
    RegexGramCompiler c(s); GramQuery q;
    ASSERT_TRUE(c.compile_like("%abcd%ef_gh%", &q).ok());
    EXPECT_EQ(q.to_debug_string(), "(\"abc\" & \"bcd\")");   // "ef" 与 "gh" 短于 3
    ASSERT_TRUE(c.compile_like("abc\\%def", &q).ok());        // 转义的 % 是字面量
    EXPECT_EQ(q.to_debug_string(), "(\"%de\" & \"abc\" & \"bc%\" & \"c%d\" & \"def\")");
    ASSERT_TRUE(c.compile_like("%", &q).ok());
    EXPECT_TRUE(q.is_all());
}

}  // namespace doris::segment_v2::gram
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=RegexGramCompilerTest.*`
Expected: 编译失败

- [ ] **Step 3: 实现**

移植原型 `ngram_model.cpp` 的 `Info / cross / uni / trim_set / demote / simplify / info_empty / info_any_char / info_any_match / concat_info / alt_info / plus_info / analyze / compile_regex_to_q`（约 200 行）到 `regex_gram_compiler.cpp`，映射规则：
- `Q` → `GramQuery`，`q_and/q_or/q_of_set/q_of_string` → `GramQuery::and_/or_` 与两个静态辅助：
  ```cpp
  GramQuery q_of_string(const std::string& s) {  // s 的 gram 之 AND；无 gram → ALL
      std::vector<std::string> g; _extractor.grams_of_literal(s, &g);
      if (g.empty()) return GramQuery::all();
      GramQuery q; q.op = GramQuery::Op::AND; q.grams = g; std::sort(q.grams.begin(), q.grams.end());
      q.grams.erase(std::unique(q.grams.begin(), q.grams.end()), q.grams.end()); return q;
  }
  GramQuery q_of_set(const std::set<std::string>& ss) {  // OR over strings
      if (ss.empty()) return GramQuery::none();
      GramQuery r = GramQuery::none();
      for (auto& s : ss) r = GramQuery::or_(std::move(r), q_of_string(s));
      return r;
  }
  ```
- `str_units/head_units/tail_units` 在字节模式下就是 `size/substr`（非 ASCII 码点是整体 1-gram，裁剪时不得切断码点：`head_units/tail_units` 需按码点边界回退，用 `codepoint_len` 判断）。
- `keep` 长度：`scheme.mode == SPARSE ? scheme.max_len : scheme.min_len - 1`。
- `compile_regexp`：`parse_regex` 失败 → `*out = ALL; return OK`；`lower_case && icase` → 先折叠模式中的 ASCII 字母；根节点最后 `match &= q_of_set(exact)`（有 exact）或 `match &= q_of_set(prefix) & q_of_set(suffix)`。
- `compile_like`：
  ```cpp
  Status RegexGramCompiler::compile_like(std::string_view p, GramQuery* out) {
      GramQuery q = GramQuery::all();
      std::string seg;
      auto flush = [&] { if (!seg.empty()) { q = GramQuery::and_(std::move(q), q_of_string(seg)); seg.clear(); } };
      for (size_t i = 0; i < p.size(); i++) {
          char c = p[i];
          if (c == '\\' && i + 1 < p.size()) { seg.push_back(p[++i]); continue; }
          if (c == '%' || c == '_') { flush(); continue; }
          seg.push_back(c);
      }
      flush();
      *out = std::move(q);
      return Status::OK();
  }
  ```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter=RegexGramCompilerTest.*`
Expected: 5 个用例 PASS。稀疏 golden 若与原型输出顺序不同（`GramQuery` 的 gram 已排序），按排序后的形式修正期望字符串。

- [ ] **Step 5: 提交**

```bash
git add be/src/storage/index/inverted/gram/regex_gram_compiler.h be/src/storage/index/inverted/gram/regex_gram_compiler.cpp be/test/storage/index/inverted/gram/regex_gram_compiler_test.cpp
git commit -m "feat(be): Cox-style regex/LIKE to gram boolean query compiler"
```

---

### Task 6: 差分模糊测试 —— 编译器只能漏杀不能误杀

**Files:**
- Test: `be/test/storage/index/inverted/gram/regex_gram_fuzz_test.cpp`

**Interfaces:**
- Consumes: Task 2、Task 5；re2（BE 已链接）

- [ ] **Step 1: 写测试（这一步本身就是交付物）**

```cpp
// be/test/storage/index/inverted/gram/regex_gram_fuzz_test.cpp
#include <gtest/gtest.h>
#include <re2/re2.h>

#include <random>
#include <set>

#include "storage/index/inverted/gram/gram_extractor.h"
#include "storage/index/inverted/gram/regex_gram_compiler.h"

namespace doris::segment_v2::gram {

namespace {
// 在一行的 gram 集合上求值查询
bool eval(const GramQuery& q, const std::set<std::string>& grams) {
    switch (q.op) {
    case GramQuery::Op::ALL: return true;
    case GramQuery::Op::NONE: return false;
    case GramQuery::Op::AND:
        for (auto& g : q.grams) if (!grams.count(g)) return false;
        for (auto& s : q.subs) if (!eval(s, grams)) return false;
        return true;
    case GramQuery::Op::OR:
        for (auto& g : q.grams) if (grams.count(g)) return true;
        for (auto& s : q.subs) if (eval(s, grams)) return true;
        return false;
    }
    return true;
}
const char* kAtoms[] = {"error", "code", "Unavailable", "timeout", "user_id=", "10.68.", "GET", "POST", "手机", "微博",
                        "[0-9]", "[a-z]", "\\d", ".", "a", "b", "c", " ", "=", ":"};
std::string random_regex(std::mt19937& rng, int depth) {
    std::uniform_int_distribution<int> pick(0, 19), shape(0, 9);
    std::string s;
    int parts = 1 + rng() % 3;
    for (int i = 0; i < parts; i++) {
        int sh = shape(rng);
        std::string atom = kAtoms[pick(rng)];
        if (depth > 0 && sh == 0) atom = "(" + random_regex(rng, depth - 1) + "|" + random_regex(rng, depth - 1) + ")";
        if (sh == 1) atom = "(" + atom + ")?";
        if (sh == 2) atom = "(" + atom + ")+";
        if (sh == 3) atom = "(" + atom + ")*";
        if (sh == 4) atom = "(" + atom + "){1,3}";
        s += atom;
    }
    return s;
}
std::string random_row(std::mt19937& rng) {
    static const char* kRows[] = {"rpc error: code = Unavailable desc = timeout", "user_id=abc GET /images/x.gif",
                                  "手机微博 POST 10.68.3.18:8080 error", "Convert conversion successful", "",
                                  "aaa bbb ccc", "code=Unavailable", "timeout after error error error"};
    std::string r = kRows[rng() % 8];
    if (rng() % 3 == 0) r += kAtoms[rng() % 10];
    return r;
}
}  // namespace

TEST(RegexGramFuzzTest, CompiledQueryIsSuperset) {
    std::mt19937 rng(20260903);
    for (GramMode mode : {GramMode::DENSE, GramMode::SPARSE}) {
        for (bool lc : {false, true}) {
            GramScheme s; s.mode = mode; s.lower_case = lc;
            GramExtractor ex(s);
            RegexGramCompiler comp(s);
            int compiled = 0, indexable = 0;
            for (int it = 0; it < 3000; it++) {
                std::string re = random_regex(rng, 2);
                RE2 rx(re, RE2::Quiet);
                if (!rx.ok()) continue;
                GramQuery q;
                ASSERT_TRUE(comp.compile_regexp(re, &q).ok());
                compiled++;
                if (!q.is_all()) indexable++;
                for (int r = 0; r < 20; r++) {
                    std::string row = random_row(rng);
                    bool truth = RE2::PartialMatch(row, rx);
                    std::vector<std::string_view> g;
                    ex.extract(row, &g);
                    std::set<std::string> grams(g.begin(), g.end());
                    bool cand = eval(q, grams);
                    ASSERT_TRUE(!truth || cand) << "FALSE NEGATIVE mode=" << (int)mode << " lc=" << lc << " re=" << re
                                                << " row=" << row << " q=" << q.to_debug_string();
                }
            }
            EXPECT_GT(compiled, 1000);
            EXPECT_GT(indexable, compiled / 4) << "编译器过于保守";
        }
    }
}

}  // namespace doris::segment_v2::gram
```

- [ ] **Step 2: 跑测试**

Run: `cd be && ./run-be-ut.sh --run --filter=RegexGramFuzzTest.*`
Expected: PASS。若出现 FALSE NEGATIVE，先把该 `re/row` 加为 `RegexGramCompilerTest` 的固定用例再修编译器（修法只能是「更保守」：对应节点返回 ALL/any_match），禁止改测试。

- [ ] **Step 3: 提交**

```bash
git add be/test/storage/index/inverted/gram/regex_gram_fuzz_test.cpp
git commit -m "test(be): differential fuzz test proving gram compiler never drops a matching row"
```


## 阶段 B：写入接入（索引策略框架 + SNII 写入器）

> 命名对照：设计文档写作时基于 `spimi-optimize` 分支，称存储格式为「V4 / SPIMI」；本计划面向 master，同一格式在 master 上叫 **SNII**（`InvertedIndexStorageFormatPB { V1=0; V2=1; V3=2; SNII=3 }`，`gensrc/proto/olap_file.proto:474-479`），代码在 `be/src/storage/index/snii/`。下文一律用 master 名字。tokenizer 目录为 `be/src/storage/index/inverted/tokenizer/`（不是 `analysis/tokenizer/`）。

### Task 7: GramTokenizer —— 把 GramExtractor 接进索引策略框架的 `ngram` tokenizer

**Files:**
- Create: `be/src/storage/index/inverted/tokenizer/ngram/gram_tokenizer.h`
- Create: `be/src/storage/index/inverted/tokenizer/ngram/gram_tokenizer.cpp`
- Modify: `be/src/storage/index/inverted/tokenizer/ngram/ngram_tokenizer_factory.h:34-65`（`create()`、成员）
- Modify: `be/src/storage/index/inverted/tokenizer/ngram/ngram_tokenizer_factory.cpp:26-38`（`initialize()`）
- Test: `be/test/storage/index/inverted/tokenizer/gram_tokenizer_test.cpp`

**Interfaces:**
- Consumes:
  - `DorisTokenizer`（`tokenizer/tokenizer.h:28-48`）：`void set_reader(const ReaderPtr& in)`、`void reset() override`、成员 `_in`；产出 term 用 `DorisTokenStream::set(Token* t, const std::string_view& term, int32_t pos = 1)`（`token_stream.h:51-61`，零拷贝，term 指针须到下一次 `next` 前有效）。
  - `NGramTokenizer::reset()` 的读全量输入方式（`ngram_tokenizer.cpp:83-94`）：`_in->read(&_char_buffer, 0, _in->size())` 得到 `const char* _char_buffer` 与长度。
  - `Settings::get_string/get_int/get_bool`（`setting.h:50-82`）；`AnalysisFactoryMgr::create<TokenizerFactory>("ngram", params)` 在 `analysis_factory_mgr.cpp:60-61` 已注册，**不改注册**。
  - Task 1 `GramScheme::from_properties`、Task 2 `GramExtractor`。
- Produces:
  ```cpp
  class GramTokenizer : public DorisTokenizer {
  public:
      explicit GramTokenizer(const gram::GramScheme& scheme);
      Token* next(Token* token) override;   // 依次产出本值的 gram，positionIncrement 恒 1；耗尽返回 nullptr
      void reset() override;                // 读全量输入并一次性提取
      const gram::GramScheme& scheme() const;
  };
  // NGramTokenizerFactory 新增：
  //   Settings 含 "mode" 时进入 gram 族：_gram_scheme 有值，create() 返回 GramTokenizer；
  //   mode 缺省时行为与现状逐字节一致（仍返回 NGramTokenizer）。
  std::optional<gram::GramScheme> NGramTokenizerFactory::gram_scheme() const;
  ```
- 参数映射（tokenizer 策略属性 → `GramScheme::from_properties`）：`mode`→mode（auto 在 P0 解析为 sparse）、`min_gram`→min_len（gram 族默认 3）、`max_gram`→max_len（默认 16）、`density`、`stop_gram_df`、`lower_case`（**gram 族的折叠开关放在 tokenizer 属性上**，因为折叠必须发生在边界哈希之前，而索引级 `lower_case` 对自定义 analyzer 不生效；设计文档 §6.2.1 相应改为「TOKENIZER 属性 lower_case」，见 Task 16 的文档同步）。

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/storage/index/inverted/tokenizer/gram_tokenizer_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/index/inverted/tokenizer/ngram/gram_tokenizer.h"
#include "storage/index/inverted/tokenizer/ngram/ngram_tokenizer_factory.h"
#include "storage/index/inverted/tokenizer/setting.h"

namespace doris::segment_v2 {

// 与 ngram_tokenizer_test.cpp:34-50 相同的驱动方式
static std::vector<std::string> tokenize(NGramTokenizerFactory& factory, const std::string& data) {
    auto tokenizer = factory.create();
    auto reader = std::make_shared<lucene::util::SStringReader<char>>();
    reader->init(data.data(), data.size(), false);
    tokenizer->set_reader(reader);
    tokenizer->reset();
    std::vector<std::string> out;
    Token t;
    while (tokenizer->next(&t)) {
        out.emplace_back(t.termBuffer<char>(), t.termLength<char>());
    }
    return out;
}

TEST(GramTokenizerTest, ModeAbsentKeepsLegacyBehaviour) {
    NGramTokenizerFactory factory;
    std::unordered_map<std::string, std::string> args {{"min_gram", "2"}, {"max_gram", "3"}};
    Settings settings(args);
    factory.initialize(settings);
    EXPECT_FALSE(factory.gram_scheme().has_value());
    EXPECT_EQ(tokenize(factory, "abc"), (std::vector<std::string> {"ab", "abc", "bc"}));
}

TEST(GramTokenizerTest, SparseModeProducesCdcGrams) {
    NGramTokenizerFactory factory;
    std::unordered_map<std::string, std::string> args {{"mode", "sparse"}, {"min_gram", "3"}, {"max_gram", "16"}, {"density", "0.25"}};
    Settings settings(args);
    factory.initialize(settings);
    ASSERT_TRUE(factory.gram_scheme().has_value());
    EXPECT_EQ(factory.gram_scheme()->mode, gram::GramMode::SPARSE);
    // golden 同 GramExtractorTest.SparseGoldenFromPrototype（按起点顺序）
    EXPECT_EQ(tokenize(factory, "rpc error: code = Unavailable"),
              (std::vector<std::string> {"or: co", "cod", "ode = U", " Unavai", "ailable"}));
    EXPECT_EQ(tokenize(factory, "手机ab微博"), (std::vector<std::string> {"手", "机", "微", "博"}));
    EXPECT_EQ(tokenize(factory, ""), (std::vector<std::string> {}));
}

TEST(GramTokenizerTest, DenseModeAndAutoAlias) {
    NGramTokenizerFactory factory;
    std::unordered_map<std::string, std::string> args {{"mode", "dense"}, {"min_gram", "3"}};
    factory.initialize(Settings(args));
    EXPECT_EQ(tokenize(factory, "abcd"), (std::vector<std::string> {"abc", "bcd"}));
    NGramTokenizerFactory f2;
    std::unordered_map<std::string, std::string> a2 {{"mode", "auto"}};
    f2.initialize(Settings(a2));
    EXPECT_EQ(f2.gram_scheme()->mode, gram::GramMode::SPARSE);  // P0：auto = sparse
}

TEST(GramTokenizerTest, GramModeSkipsLegacyMinMaxGapCheck) {
    // 现状 initialize 对 max_gram-min_gram>1 抛 INVALID_ARGUMENT（ngram_tokenizer_factory.cpp:29-36）；gram 族不受此限
    NGramTokenizerFactory factory;
    std::unordered_map<std::string, std::string> args {{"mode", "sparse"}, {"min_gram", "3"}, {"max_gram", "24"}};
    EXPECT_NO_THROW(factory.initialize(Settings(args)));
    std::unordered_map<std::string, std::string> bad {{"mode", "sparse"}, {"density", "2"}};
    EXPECT_THROW(NGramTokenizerFactory().initialize(Settings(bad)), Exception);
}

}  // namespace doris::segment_v2
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=GramTokenizerTest.*`
Expected: 编译失败（`gram_tokenizer.h` 不存在、`gram_scheme()` 未声明）

- [ ] **Step 3: 实现**

```cpp
// be/src/storage/index/inverted/tokenizer/ngram/gram_tokenizer.h
#pragma once
#include <string_view>
#include <vector>

#include "storage/index/inverted/gram/gram_extractor.h"
#include "storage/index/inverted/tokenizer/tokenizer.h"

namespace doris::segment_v2 {

// 把 gram::GramExtractor 适配为 DorisTokenizer：一个列值 = 一次 reset，一次性提取全部 gram
class GramTokenizer : public DorisTokenizer {
public:
    explicit GramTokenizer(const gram::GramScheme& scheme) : _extractor(scheme) {}
    Token* next(Token* token) override;
    void reset() override;
    const gram::GramScheme& scheme() const { return _extractor.scheme(); }

private:
    gram::GramExtractor _extractor;
    const char* _char_buffer = nullptr;
    int32_t _char_length = 0;
    std::vector<std::string_view> _grams;  // view 指向 _char_buffer 或提取器内部折叠副本
    size_t _next = 0;
};

}  // namespace doris::segment_v2
```

```cpp
// be/src/storage/index/inverted/tokenizer/ngram/gram_tokenizer.cpp
#include "storage/index/inverted/tokenizer/ngram/gram_tokenizer.h"

namespace doris::segment_v2 {

void GramTokenizer::reset() {
    DorisTokenizer::reset();
    _grams.clear();
    _next = 0;
    _char_buffer = nullptr;
    _char_length = 0;
    if (_in == nullptr || _in->size() == 0) return;
    // 与 NGramTokenizer::reset() 相同：一次读全量输入（ngram_tokenizer.cpp:83-94）
    _char_length = _in->read(&_char_buffer, 0, _in->size());
    if (_char_length <= 0 || _char_buffer == nullptr) return;
    _extractor.extract(std::string_view(_char_buffer, _char_length), &_grams);
}

Token* GramTokenizer::next(Token* token) {
    if (_next >= _grams.size()) return nullptr;
    set(token, _grams[_next++], 1);
    return token;
}

}  // namespace doris::segment_v2
```

工厂改动（`ngram_tokenizer_factory.h:34-65` 与 `.cpp:26-38`）：

```cpp
// ngram_tokenizer_factory.h 新增
#include <optional>
#include "storage/index/inverted/gram/gram_scheme.h"
    std::optional<gram::GramScheme> gram_scheme() const { return _gram_scheme; }
private:
    std::optional<gram::GramScheme> _gram_scheme;   // "mode" 存在时有值

// create() 开头插入：
    if (_gram_scheme.has_value()) {
        return std::make_shared<GramTokenizer>(*_gram_scheme);
    }
```

```cpp
// ngram_tokenizer_factory.cpp initialize() 开头插入（在读 min_gram/max_gram 之前）
    if (settings.has("mode")) {  // 若 Settings 无 has()，用 !settings.get_string("mode").empty()
        std::map<std::string, std::string> props;
        for (auto& [k, v] : settings.sorted_entries()) props.emplace(k, v);
        if (!props.count("min_gram")) props["min_gram"] = "3";   // gram 族默认值不同于 legacy 的 1/2
        if (!props.count("max_gram")) props["max_gram"] = "16";
        gram::GramScheme scheme;
        Status st = gram::GramScheme::from_properties(props, &scheme);
        if (!st.ok()) {
            throw Exception(ErrorCode::INVALID_ARGUMENT, "ngram tokenizer: {}", st.to_string());
        }
        _gram_scheme = scheme;
        return;   // 跳过 legacy 的 max-min>1 校验与 token_chars
    }
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter='GramTokenizerTest.*:NGramTokenizerTest.*:EdgeNGramTokenizerTest.*'`
Expected: 全部 PASS（legacy 用例不受影响）

- [ ] **Step 5: 提交**

```bash
git add be/src/storage/index/inverted/tokenizer/ngram/gram_tokenizer.h be/src/storage/index/inverted/tokenizer/ngram/gram_tokenizer.cpp be/src/storage/index/inverted/tokenizer/ngram/ngram_tokenizer_factory.h be/src/storage/index/inverted/tokenizer/ngram/ngram_tokenizer_factory.cpp be/test/storage/index/inverted/tokenizer/gram_tokenizer_test.cpp
git commit -m "feat(be): ngram tokenizer mode=sparse|dense|auto backed by GramExtractor"
```

---

### Task 8: gram 族识别 + SNII 写入器强制 docs-only

**Files:**
- Create: `be/src/storage/index/inverted/gram/gram_family.h`、`gram_family.cpp`
- Modify: `be/src/storage/index/inverted/analyzer/custom_analyzer.h`（`CustomAnalyzerProvider` 加 `gram_scheme()`）与 `analyzer/analyzer.h`（`AnalyzerProvider` 基类加虚函数，默认 `std::nullopt`）
- Modify: `be/src/storage/index/snii/snii_index_writer.cpp:71-77,116-132,180-182,237-242`
- Test: `be/test/storage/index/snii/snii_writer_test.cpp`（追加用例，沿用 `ScopedCommonGramsPolicies` 注入方式 `:71-101`）

**Interfaces:**
- Consumes:
  - `IndexPolicyMgr::build_analyzer_config_from_policy`（`runtime/index_policy/index_policy_mgr.cpp:218-264`）：tokenizer 策略 `properties["type"]` 是工厂名，其余键值进 `Settings`，经 `builder.with_tokenizer_config(type, settings)` 进入 `CustomAnalyzerConfig`。
  - `SniiIndexColumnWriter::init()`（`snii_index_writer.cpp:71-199`）：`_has_positions = get_parser_phrase_support_string_from_properties(...) == "true"` `:74-75`，`_config = _has_positions ? kDocsPositions : kDocsOnly` `:76-77`；`_analyzer = provider->get_analyzer(...)` `:180-182`；`consume_token` `:237-242`。
- Produces:
  ```cpp
  // analyzer/analyzer.h  AnalyzerProvider 基类
  virtual std::optional<gram::GramScheme> gram_scheme() const { return std::nullopt; }
  // analyzer/custom_analyzer.h  CustomAnalyzerProvider
  std::optional<gram::GramScheme> gram_scheme() const override;   // 由 config 的 tokenizer_config（type=="ngram" 且含 "mode"）在构造时算出
  // gram/gram_family.h
  namespace doris::segment_v2::gram {
  // 从索引属性解析 analyzer 名 → 策略 → 方案；内置 parser 或非 gram tokenizer 返回 nullopt
  std::optional<GramScheme> resolve_gram_scheme(const std::map<std::string, std::string>& index_properties,
                                                IndexPolicyMgr* mgr);
  }
  ```

- [ ] **Step 1: 写失败测试**

```cpp
// 追加到 be/test/storage/index/snii/snii_writer_test.cpp（沿用文件内 ScopedCommonGramsPolicies 与 make_meta 工具）
TEST_F(SniiWriterTest, GramTokenizerForcesDocsOnlyAndIsRecognised) {
    ScopedCommonGramsPolicies policies;   // :71-101 的注入器，改名为通用 ScopedIndexPolicies 亦可
    TIndexPolicy tokenizer;
    tokenizer.id = 9001; tokenizer.name = "gram_sparse"; tokenizer.type = TIndexPolicyType::TOKENIZER;
    tokenizer.properties["type"] = "ngram";
    tokenizer.properties["mode"] = "sparse";
    TIndexPolicy analyzer;
    analyzer.id = 9002; analyzer.name = "gram_sparse"; analyzer.type = TIndexPolicyType::ANALYZER;
    analyzer.properties["tokenizer"] = "gram_sparse";
    policies.manager().apply_policy_changes({tokenizer, analyzer}, {});

    std::map<std::string, std::string> props {{"analyzer", "gram_sparse"}, {"support_phrase", "true"}};  // 故意要位置
    TabletIndex index_meta = make_meta(props);
    auto scheme = gram::resolve_gram_scheme(index_meta.properties(), &policies.manager());
    ASSERT_TRUE(scheme.has_value());
    EXPECT_EQ(scheme->mode, gram::GramMode::SPARSE);

    SniiIndexColumnWriter writer(nullptr, &index_meta, FieldType::OLAP_FIELD_TYPE_VARCHAR);
    ASSERT_TRUE(writer.init().ok());
    EXPECT_EQ(writer.index_config_for_test(), IndexConfig::kDocsOnly);   // gram 族强制 docs-only，忽略 support_phrase

    std::vector<Slice> values {Slice("rpc error: code = Unavailable"), Slice("手机微博")};
    ASSERT_TRUE(writer.add_values("c", values.data(), values.size()).ok());
    auto postings = writer.term_buffer_for_test()->finalize_sorted();
    std::vector<std::string> terms;
    for (auto& p : postings) terms.push_back(std::string(p.term));
    std::sort(terms.begin(), terms.end());
    std::vector<std::string> expected {" Unavai", "ailable", "cod", "ode = U", "or: co", "博", "微", "手", "机"};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(terms, expected);
}

TEST_F(SniiWriterTest, NonGramAnalyzerHasNoScheme) {
    std::map<std::string, std::string> props {{"parser", "english"}};
    TabletIndex index_meta = make_meta(props);
    EXPECT_FALSE(gram::resolve_gram_scheme(index_meta.properties(), ExecEnv::GetInstance()->index_policy_mgr()).has_value());
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=SniiWriterTest.GramTokenizer*`
Expected: 编译失败（`gram_family.h`、`index_config_for_test` 不存在）

- [ ] **Step 3: 实现**

```cpp
// be/src/storage/index/inverted/gram/gram_family.h
#pragma once
#include <map>
#include <optional>
#include <string>

#include "storage/index/inverted/gram/gram_scheme.h"

namespace doris { class IndexPolicyMgr; }
namespace doris::segment_v2::gram {
std::optional<GramScheme> resolve_gram_scheme(const std::map<std::string, std::string>& index_properties,
                                              IndexPolicyMgr* mgr);
}
```

```cpp
// be/src/storage/index/inverted/gram/gram_family.cpp
#include "storage/index/inverted/gram/gram_family.h"

#include "runtime/index_policy/index_policy_mgr.h"
#include "storage/index/inverted/analyzer/analyzer.h"          // InvertedIndexAnalyzer::create_analyzer_provider
#include "storage/index/inverted/inverted_index_parser.h"      // get_analyzer_name_from_properties

namespace doris::segment_v2::gram {

std::optional<GramScheme> resolve_gram_scheme(const std::map<std::string, std::string>& props, IndexPolicyMgr* mgr) {
    std::string name = get_analyzer_name_from_properties(props);   // inverted_index_parser.cpp:179-192
    if (name.empty() || mgr == nullptr) return std::nullopt;
    AnalyzerProviderPtr provider = mgr->get_analyzer_provider_by_name(name);
    if (provider == nullptr) return std::nullopt;
    return provider->gram_scheme();
}

}  // namespace doris::segment_v2::gram
```

`CustomAnalyzerProvider::gram_scheme()`（`custom_analyzer.cpp`，构造时计算并缓存）：

```cpp
std::optional<gram::GramScheme> CustomAnalyzerProvider::gram_scheme() const { return _gram_scheme; }
// 构造函数末尾：
    const auto& tk = _config->tokenizer_config();          // 若访问器名不同，以 custom_analyzer.h 中 with_tokenizer_config 存入的成员为准
    if (tk.name == "ngram" && tk.params.get_string("mode") != "") {
        std::map<std::string, std::string> p;
        for (auto& [k, v] : tk.params.sorted_entries()) p.emplace(k, v);
        if (!p.count("min_gram")) p["min_gram"] = "3";
        if (!p.count("max_gram")) p["max_gram"] = "16";
        gram::GramScheme s;
        if (gram::GramScheme::from_properties(p, &s).ok()) _gram_scheme = s;
    }
```

`SniiIndexColumnWriter::init()` 改动（`snii_index_writer.cpp`）：

```cpp
// :74-77 之后，provider 创建（:130-132）之后插入：
    if (auto scheme = _analyzer_provider->gram_scheme(); scheme.has_value()) {
        _gram_scheme = scheme;                       // 新成员 std::optional<gram::GramScheme>
        if (_has_positions) {
            LOG(INFO) << "gram-family analyzer forces docs-only index, ignoring support_phrase for index "
                      << _index_meta->index_id();
            _has_positions = false;
            _config = IndexConfig::kDocsOnly;        // 且 SpimiTermBuffer 须以 has_positions=false 构造：把 :91-92 的构造移到此判断之后
        }
    }
// 测试访问器（snii_index_writer.h）：IndexConfig index_config_for_test() const { return _config; }
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter='SniiWriterTest.*'`
Expected: 新增 2 个用例与既有用例全部 PASS

- [ ] **Step 5: 提交**

```bash
git add be/src/storage/index/inverted/gram/gram_family.h be/src/storage/index/inverted/gram/gram_family.cpp be/src/storage/index/inverted/analyzer/analyzer.h be/src/storage/index/inverted/analyzer/custom_analyzer.h be/src/storage/index/inverted/analyzer/custom_analyzer.cpp be/src/storage/index/snii/snii_index_writer.h be/src/storage/index/snii/snii_index_writer.cpp be/test/storage/index/snii/snii_writer_test.cpp
git commit -m "feat(be): recognise gram-family analyzers and force docs-only SNII index for them"
```

---

### Task 12: 段级元数据预留 `gram_scheme`（P1 auto 模式用，P0 只做字段与编解码）

**Files:**
- Modify: `gensrc/proto/snii.proto:76-89`（`SniiCoreMetadataPB` 追加字段 6 + 新 message）
- Modify: `be/src/storage/index/snii/format/core_metadata.h:51-57`、`core_metadata.cpp:163-260`
- Modify: `be/src/storage/index/snii/reader/logical_index_reader.h:182-187`（访问器）
- Test: `be/test/storage/index/snii/format/core_metadata_test.cpp`（若不存在则新建，模板为 `encode_common_grams/decode_common_grams` 的既有用例）

**Interfaces:**
- Consumes: `encode_core_metadata(const CoreMetadata&, ByteSink*)` / `decode_core_metadata(Slice, CoreMetadata*)`（`core_metadata.h`），`CommonGramsSegmentMetadata` 的编解码范式（`core_metadata.cpp:109-161`）。
- Produces:
  ```protobuf
  // gensrc/proto/snii.proto
  message SniiGramSchemePB {
      optional uint32 mode = 1;              // 1 dense, 2 sparse
      optional uint32 min_len = 2;
      optional uint32 max_len = 3;
      optional uint32 density_permille = 4;
      optional uint32 stop_df_permille = 5;
      optional bool lower_case = 6;
      optional uint32 hash_version = 7;
  }
  // SniiCoreMetadataPB: optional SniiGramSchemePB gram_scheme = 6;   // 只追加不插入（:84-89 约定）
  ```
  ```cpp
  // core_metadata.h
  struct CoreMetadata { ...; std::optional<gram::GramScheme> gram_scheme; };
  // logical_index_reader.h
  const std::optional<gram::GramScheme>& gram_scheme() const;   // 来自 core metadata；P0 写入侧不填，读侧为 nullopt
  ```

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/storage/index/snii/format/core_metadata_test.cpp（追加）
TEST(CoreMetadataTest, GramSchemeRoundTrip) {
    CoreMetadata core;
    core.index_config = IndexConfig::kDocsOnly;
    gram::GramScheme s; s.mode = gram::GramMode::DENSE; s.min_len = 3; s.max_len = 3; s.density_permille = 1000;
    s.stop_df_permille = 250; s.lower_case = true; s.hash_version = 1;
    core.gram_scheme = s;
    std::string bytes;
    StringByteSink sink(&bytes);                     // 以文件内既有 ByteSink 实现为准
    ASSERT_TRUE(encode_core_metadata(core, &sink).ok());
    CoreMetadata back;
    ASSERT_TRUE(decode_core_metadata(Slice(bytes), &back).ok());
    ASSERT_TRUE(back.gram_scheme.has_value());
    EXPECT_TRUE(*back.gram_scheme == s);
    CoreMetadata none; none.index_config = IndexConfig::kDocsOnly;
    bytes.clear();
    ASSERT_TRUE(encode_core_metadata(none, &sink).ok());
    ASSERT_TRUE(decode_core_metadata(Slice(bytes), &back).ok());
    EXPECT_FALSE(back.gram_scheme.has_value());
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=CoreMetadataTest.GramSchemeRoundTrip`
Expected: 编译失败（字段不存在）

- [ ] **Step 3: 实现**

```cpp
// core_metadata.cpp encode_core_metadata（:242-248 写 common_grams 之后）
    if (core.gram_scheme.has_value()) {
        auto* g = pb.mutable_gram_scheme();
        g->set_mode(static_cast<uint32_t>(core.gram_scheme->mode));
        g->set_min_len(core.gram_scheme->min_len);
        g->set_max_len(core.gram_scheme->max_len);
        g->set_density_permille(core.gram_scheme->density_permille);
        g->set_stop_df_permille(core.gram_scheme->stop_df_permille);
        g->set_lower_case(core.gram_scheme->lower_case);
        g->set_hash_version(core.gram_scheme->hash_version);
    }
// decode_core_pb（:191-195 之后）
    if (pb.has_gram_scheme()) {
        const auto& g = pb.gram_scheme();
        gram::GramScheme s;
        if (g.mode() != 1 && g.mode() != 2) return Status::Corruption("snii core metadata: bad gram mode {}", g.mode());
        s.mode = static_cast<gram::GramMode>(g.mode());
        s.min_len = g.min_len(); s.max_len = g.max_len(); s.density_permille = g.density_permille();
        s.stop_df_permille = g.stop_df_permille(); s.lower_case = g.lower_case(); s.hash_version = g.hash_version();
        core->gram_scheme = s;
    }
```

`LogicalIndexReader::gram_scheme()` 直接返回 `core_metadata().gram_scheme`（参照 `:182-184 common_grams_metadata()`）。写入侧（`logical_index_writer.cpp:944` 附近）P0 保持 `core.gram_scheme = std::nullopt`。

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter='CoreMetadataTest.*:SniiWriterGoldenBytesTest.*'`
Expected: PASS；golden bytes 用例不变（字段未写入时编码字节完全相同）

- [ ] **Step 5: 提交**

```bash
git add gensrc/proto/snii.proto be/src/storage/index/snii/format/core_metadata.h be/src/storage/index/snii/format/core_metadata.cpp be/src/storage/index/snii/reader/logical_index_reader.h be/test/storage/index/snii/format/core_metadata_test.cpp
git commit -m "feat(be): reserve per-segment gram_scheme in SNII core metadata"
```


## 阶段 C：查询接入（SNII 查询执行器 → like/regexp 函数 → 近似索引语义）

> 事实（勘察结论，实施时以此为准）：master 上 FE 不做任何「函数是否允许上索引」的标记，`WHERE` 里的 LIKE/REGEXP 被翻译成 `FunctionCallExpr("like"/"regexp")` 原样进入 `TPlanNode.conjuncts`；BE `SegmentIterator::_apply_index_expr`（`segment_iterator.cpp:1243-1366`）对**每个** common expr 调 `evaluate_inverted_index`。因此 P0 的下推许可不需要 FE 改动，只需 BE 函数实现 `IFunctionBase::evaluate_inverted_index`（`be/src/exprs/function/function.h:219-226`）。`FunctionLike/FunctionRegexpLike` 目前**完全没有**该实现。

### Task 9: GRAM_BOOLEAN_QUERY —— SNII 上的 gram 布尔查询执行

**Files:**
- Modify: `be/src/storage/index/inverted/inverted_index_query_type.h:68-86`（枚举）、`:99-106`（`is_match_query`）、`:108-`（`query_type_to_string`）
- Create: `be/src/storage/index/snii/query/gram_boolean_query.h`、`gram_boolean_query.cpp`
- Modify: `be/src/storage/index/snii/snii_index_reader.cpp:348-433`（`execute_snii_query` 分发）、`:466-470`（原始串不分词的条件）
- Test: `be/test/storage/index/snii/query/gram_boolean_query_test.cpp`

**Interfaces:**
- Consumes:
  - `reader::LogicalIndexReader::lookup(std::string_view term, bool* found, format::DictEntry* entry, uint64_t* frq_base, uint64_t* prx_base, DictBlockCache* = nullptr) const`（`snii/reader/logical_index_reader.h:106-107`），`DictEntry.df`（`dict_entry.h:88`）；
  - `internal::read_docid_posting(const LogicalIndexReader&, const format::DictEntry&, uint64_t frq_base, uint64_t prx_base, DocIdSink*)`（`snii/query/docid_posting_reader.h:42-43`）；
  - `DocIdSink { append_sorted(std::span<const uint32_t>); append_range(uint32_t, uint64_t); dedups(); }`（`snii/query/docid_sink.h:32-46`）；`snii_index_reader.cpp:357` 的 `RoaringDocIdSink` 在匿名命名空间，**不可复用**，本任务自带一个；
  - Task 3 `GramQuery`。
- Produces:
  ```cpp
  namespace doris::segment_v2::snii::query {
  // 可注入的 posting 源：生产用 LogicalIndexReader 适配，测试用 map
  class GramPostingSource {
  public:
      virtual ~GramPostingSource() = default;
      // 词典查找：found=false 表示 gram 不存在（→ NONE）
      virtual Status df(std::string_view gram, bool* found, uint64_t* df) = 0;
      virtual Status postings(std::string_view gram, roaring::Roaring* out) = 0;
  };
  class LogicalIndexPostingSource final : public GramPostingSource {
  public:
      explicit LogicalIndexPostingSource(const reader::LogicalIndexReader& idx);
      Status df(std::string_view, bool*, uint64_t*) override;      // lookup + entry.df
      Status postings(std::string_view, roaring::Roaring*) override; // lookup + read_docid_posting 到 RoaringSink
  };
  // 求值：ALL → [0,num_docs)；NONE → 空；AND 按 df 升序求交、空即停；OR 求并
  Status gram_boolean_query(GramPostingSource& src, const gram::GramQuery& q, uint32_t num_docs, roaring::Roaring* out);
  }
  // inverted_index_query_type.h
  enum class InvertedIndexQueryType { ..., SEARCH_DSL_QUERY = 15, GRAM_BOOLEAN_QUERY = 16 };
  // is_match_query(GRAM_BOOLEAN_QUERY) == true（让 selector 选 FULLTEXT reader）；query_type_to_string → "gram_boolean"
  ```

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/storage/index/snii/query/gram_boolean_query_test.cpp
#include "storage/index/snii/query/gram_boolean_query.h"

#include <gtest/gtest.h>

#include <map>
#include <vector>

#include "storage/index/inverted/gram/gram_query.h"

namespace doris::segment_v2::snii::query {

class MapPostingSource final : public GramPostingSource {
public:
    std::map<std::string, std::vector<uint32_t>> lists;
    int lookups = 0;
    Status df(std::string_view g, bool* found, uint64_t* df) override {
        lookups++;
        auto it = lists.find(std::string(g));
        *found = it != lists.end();
        *df = *found ? it->second.size() : 0;
        return Status::OK();
    }
    Status postings(std::string_view g, roaring::Roaring* out) override {
        auto it = lists.find(std::string(g));
        if (it == lists.end()) return Status::OK();
        out->addMany(it->second.size(), it->second.data());
        return Status::OK();
    }
};

static std::vector<uint32_t> to_vec(const roaring::Roaring& r) { return {r.begin(), r.end()}; }
using gram::GramQuery;

TEST(GramBooleanQueryTest, AndOrAllNone) {
    MapPostingSource src;
    src.lists["abc"] = {1, 2, 3, 7};
    src.lists["bcd"] = {2, 3, 9};
    src.lists["xyz"] = {7};
    roaring::Roaring out;
    ASSERT_TRUE(gram_boolean_query(src, GramQuery::and_(GramQuery::of_gram("abc"), GramQuery::of_gram("bcd")), 10, &out).ok());
    EXPECT_EQ(to_vec(out), (std::vector<uint32_t> {2, 3}));
    out = roaring::Roaring();
    ASSERT_TRUE(gram_boolean_query(src, GramQuery::or_(GramQuery::of_gram("bcd"), GramQuery::of_gram("xyz")), 10, &out).ok());
    EXPECT_EQ(to_vec(out), (std::vector<uint32_t> {2, 3, 7, 9}));
    out = roaring::Roaring();
    ASSERT_TRUE(gram_boolean_query(src, GramQuery::all(), 4, &out).ok());
    EXPECT_EQ(to_vec(out), (std::vector<uint32_t> {0, 1, 2, 3}));
    out = roaring::Roaring();
    ASSERT_TRUE(gram_boolean_query(src, GramQuery::none(), 4, &out).ok());
    EXPECT_TRUE(out.isEmpty());
}

TEST(GramBooleanQueryTest, MissingGramIsNoneAndEarlyExit) {
    MapPostingSource src;
    src.lists["abc"] = {1, 2, 3};
    roaring::Roaring out;
    auto q = GramQuery::and_(GramQuery::of_gram("abc"), GramQuery::of_gram("nope"));
    ASSERT_TRUE(gram_boolean_query(src, q, 10, &out).ok());
    EXPECT_TRUE(out.isEmpty());
    // 缺失 gram 只走 df 查找，不读任何 posting
    EXPECT_EQ(src.lookups, 2);
}

TEST(GramBooleanQueryTest, NestedAndOfOr) {
    MapPostingSource src;
    src.lists["a"] = {1, 2, 3, 4};
    src.lists["b"] = {2};
    src.lists["c"] = {4, 5};
    roaring::Roaring out;
    auto q = GramQuery::and_(GramQuery::of_gram("a"), GramQuery::or_(GramQuery::of_gram("b"), GramQuery::of_gram("c")));
    ASSERT_TRUE(gram_boolean_query(src, q, 10, &out).ok());
    EXPECT_EQ(to_vec(out), (std::vector<uint32_t> {2, 4}));
}

}  // namespace doris::segment_v2::snii::query
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=GramBooleanQueryTest.*`
Expected: 编译失败

- [ ] **Step 3: 实现**

```cpp
// be/src/storage/index/snii/query/gram_boolean_query.h
#pragma once
#include <roaring/roaring.hh>

#include <string_view>

#include "common/status.h"
#include "storage/index/inverted/gram/gram_query.h"
#include "storage/index/snii/reader/logical_index_reader.h"

namespace doris::segment_v2::snii::query {

class GramPostingSource {
public:
    virtual ~GramPostingSource() = default;
    virtual Status df(std::string_view gram, bool* found, uint64_t* df) = 0;
    virtual Status postings(std::string_view gram, roaring::Roaring* out) = 0;
};

class LogicalIndexPostingSource final : public GramPostingSource {
public:
    explicit LogicalIndexPostingSource(const reader::LogicalIndexReader& idx) : _idx(idx) {}
    Status df(std::string_view gram, bool* found, uint64_t* df) override;
    Status postings(std::string_view gram, roaring::Roaring* out) override;

private:
    const reader::LogicalIndexReader& _idx;
};

Status gram_boolean_query(GramPostingSource& src, const gram::GramQuery& q, uint32_t num_docs, roaring::Roaring* out);

}  // namespace doris::segment_v2::snii::query
```

```cpp
// be/src/storage/index/snii/query/gram_boolean_query.cpp
#include "storage/index/snii/query/gram_boolean_query.h"

#include <algorithm>

#include "storage/index/snii/query/docid_posting_reader.h"
#include "storage/index/snii/query/docid_sink.h"

namespace doris::segment_v2::snii::query {

namespace {
// 与 snii_index_reader.cpp:103-129 的匿名 RoaringDocIdSink 等价（那个不可见）
class RoaringSink final : public DocIdSink {
public:
    explicit RoaringSink(roaring::Roaring* r) : _r(r) {}
    Status append_sorted(std::span<const uint32_t> ids) override {
        _r->addMany(ids.size(), ids.data());
        return Status::OK();
    }
    Status append_range(uint32_t start, uint64_t count) override {
        _r->addRange(start, start + count);
        return Status::OK();
    }
    bool dedups() const override { return true; }

private:
    roaring::Roaring* _r;
};

Status eval(GramPostingSource& src, const gram::GramQuery& q, uint32_t num_docs, roaring::Roaring* out) {
    using Op = gram::GramQuery::Op;
    if (q.op == Op::ALL) { out->addRange(0, num_docs); return Status::OK(); }
    if (q.op == Op::NONE) { return Status::OK(); }
    if (q.op == Op::AND) {
        // 先只查 df：缺失即 NONE；按 df 升序求交
        std::vector<std::pair<uint64_t, const std::string*>> order;
        for (auto& g : q.grams) {
            bool found = false; uint64_t df = 0;
            RETURN_IF_ERROR(src.df(g, &found, &df));
            if (!found) return Status::OK();
            order.emplace_back(df, &g);
        }
        std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first < b.first; });
        bool have = false;
        roaring::Roaring acc;
        for (auto& [df, g] : order) {
            roaring::Roaring cur;
            RETURN_IF_ERROR(src.postings(*g, &cur));
            if (!have) { acc = std::move(cur); have = true; } else { acc &= cur; }
            if (acc.isEmpty()) return Status::OK();
        }
        for (auto& s : q.subs) {
            roaring::Roaring cur;
            RETURN_IF_ERROR(eval(src, s, num_docs, &cur));
            if (!have) { acc = std::move(cur); have = true; } else { acc &= cur; }
            if (acc.isEmpty()) return Status::OK();
        }
        if (!have) { out->addRange(0, num_docs); return Status::OK(); }
        *out |= acc;
        return Status::OK();
    }
    // OR
    for (auto& g : q.grams) RETURN_IF_ERROR(src.postings(g, out));
    for (auto& s : q.subs) { roaring::Roaring cur; RETURN_IF_ERROR(eval(src, s, num_docs, &cur)); *out |= cur; }
    return Status::OK();
}
}  // namespace

Status LogicalIndexPostingSource::df(std::string_view gram, bool* found, uint64_t* df) {
    format::DictEntry entry; uint64_t frq = 0, prx = 0;
    RETURN_IF_ERROR(_idx.lookup(gram, found, &entry, &frq, &prx));
    *df = *found ? entry.df : 0;
    return Status::OK();
}

Status LogicalIndexPostingSource::postings(std::string_view gram, roaring::Roaring* out) {
    bool found = false; format::DictEntry entry; uint64_t frq = 0, prx = 0;
    RETURN_IF_ERROR(_idx.lookup(gram, &found, &entry, &frq, &prx));
    if (!found) return Status::OK();
    RoaringSink sink(out);
    return internal::read_docid_posting(_idx, entry, frq, prx, &sink);
}

Status gram_boolean_query(GramPostingSource& src, const gram::GramQuery& q, uint32_t num_docs, roaring::Roaring* out) {
    return eval(src, q, num_docs, out);
}

}  // namespace doris::segment_v2::snii::query
```

枚举与分发改动：

```cpp
// inverted_index_query_type.h:86 后追加
    GRAM_BOOLEAN_QUERY = 16,
// is_match_query（:99-106）加入 GRAM_BOOLEAN_QUERY；query_type_to_string（:108-）加 case → "gram_boolean"
```

```cpp
// snii_index_reader.cpp: :466-470 「原始 pattern 不经分词」的条件里加入 GRAM_BOOLEAN_QUERY（与 MATCH_REGEXP_QUERY 同款）
// execute_snii_query（:348-433）在 MATCH_REGEXP_QUERY 分支后追加：
        case InvertedIndexQueryType::GRAM_BOOLEAN_QUERY: {
            gram::GramQuery q;
            RETURN_IF_ERROR(gram::GramQuery::parse(search_str, &q));   // search_str 为 GramQuery::serialize() 文本
            LogicalIndexPostingSource src(logical_reader);
            return gram_boolean_query(src, q, _rows_of_segment, result->bitmap.get());   // _rows_of_segment 即 create_shared 传入的 rows_of_segment
        }
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter='GramBooleanQueryTest.*:InvertedIndexQueryType*'`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add be/src/storage/index/inverted/inverted_index_query_type.h be/src/storage/index/snii/query/gram_boolean_query.h be/src/storage/index/snii/query/gram_boolean_query.cpp be/src/storage/index/snii/snii_index_reader.cpp be/test/storage/index/snii/query/gram_boolean_query_test.cpp
git commit -m "feat(be): GRAM_BOOLEAN_QUERY evaluation on SNII index"
```

---

### Task 10: FunctionLike / FunctionRegexpLike 的 `evaluate_inverted_index`

**Files:**
- Modify: `be/src/exprs/function/like.h:279`（`FunctionLikeBase`）、`:378-410`（`FunctionLike`）、`:412-422`（`FunctionRegexpLike`）
- Modify: `be/src/exprs/function/like.cpp`（新增实现）
- Modify: `be/src/storage/index/inverted/inverted_index_reader.h:80`（`InvertedIndexResultBitmap` 加 `approximate` 标记）
- Modify: `be/src/common/config.h/.cpp`（`enable_gram_index_regexp`，默认 true）
- Test: `be/test/exprs/function/like_gram_index_test.cpp`

**Interfaces:**
- Consumes:
  - `IFunctionBase::evaluate_inverted_index(const ColumnsWithTypeAndName& arguments, const std::vector<IndexFieldNameAndTypePair>& data_type_with_names, std::vector<segment_v2::IndexIterator*> iterators, uint32_t num_rows, const InvertedIndexAnalyzerCtx* analyzer_ctx, segment_v2::InvertedIndexResultBitmap& bitmap_result) const`（`function.h:219-226`）；模板 `FunctionMatchBase::evaluate_inverted_index`（`match.cpp:49-112`）。
  - `IndexIterator::get_reader(IndexReaderType)`（`index_iterator.h:53`）→ `InvertedIndexReader::get_index_properties()`（`inverted_index_reader.h:247-249`）；`InvertedIndexParam`（`inverted_index_iterator.h:30-44`）；`IndexIterator::read_from_index(const IndexParam&)`（`:54`）。
  - Task 8 `gram::resolve_gram_scheme`，Task 5 `RegexGramCompiler`，Task 9 `GRAM_BOOLEAN_QUERY`。
- Produces:
  ```cpp
  // inverted_index_reader.h  InvertedIndexResultBitmap 新增
  void set_approximate(bool v);  bool approximate() const;   // true = 超集候选，表达式必须复验
  // like.h  FunctionLikeBase 新增（protected）
  enum class GramCompileKind { LIKE, REGEXP };
  Status evaluate_gram_index(GramCompileKind kind, const ColumnsWithTypeAndName& arguments,
                             const std::vector<IndexFieldNameAndTypePair>& data_type_with_names,
                             std::vector<segment_v2::IndexIterator*> iterators, uint32_t num_rows,
                             segment_v2::InvertedIndexResultBitmap& bitmap_result) const;
  // FunctionLike / FunctionRegexpLike 覆写 evaluate_inverted_index → evaluate_gram_index(LIKE / REGEXP, ...)
  ```

行为规约：
1. `config::enable_gram_index_regexp` 为 false、`iterators.size() != 1`、iterator 为空、`arguments` 为空或模式为 NULL → 直接 `return OK`（不写 `bitmap_result`）。
2. LIKE 带自定义 ESCAPE（`arguments.size() == 2` 且转义符不是 `\`）→ `return OK`（P0 不支持）。
3. 从 iterator 的 FULLTEXT reader 取索引属性 → `resolve_gram_scheme`；无方案（非 gram 族 / CLucene 格式）→ `return OK`。
4. 编译 → `ALL` → `return OK`（并累加 `gram_index_uncompilable` 计数，Task 11）。
5. 发起 `GRAM_BOOLEAN_QUERY`；`read_from_index` 返回 `INVERTED_INDEX_NOT_SUPPORTED / NOT_IMPLEMENTED_ERROR / INVERTED_INDEX_EVALUATE_SKIPPED` → `return OK`；其余错误原样返回。
6. 成功：`bitmap_result = InvertedIndexResultBitmap(param.roaring, nullptr); bitmap_result.set_approximate(true);`（不做 `mask_out_null`：NULL 行没有 gram，天然不在候选中）。

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/exprs/function/like_gram_index_test.cpp
// 用一个假的 IndexIterator 验证：方案解析 → 编译 → 下发的 InvertedIndexParam 内容 → 近似标记；不依赖真实索引文件
#include <gtest/gtest.h>

#include "exprs/function/like.h"
#include "storage/index/inverted/gram/gram_query.h"
#include "storage/index/inverted/inverted_index_iterator.h"

namespace doris {

class FakeGramIterator : public segment_v2::IndexIterator {
public:
    std::map<std::string, std::string> props;   // get_index_properties 返回值来源
    segment_v2::InvertedIndexParam last;
    std::vector<uint32_t> answer {3, 5};
    Status read_from_index(const segment_v2::IndexParam& param) override {
        auto* p = std::get<segment_v2::InvertedIndexParam*>(param);
        last = *p;
        p->roaring->addMany(answer.size(), answer.data());
        return Status::OK();
    }
    // 其余纯虚函数按 index_iterator.h 提供空实现；get_reader 返回一个只实现 get_index_properties 的桩 reader
};

static std::shared_ptr<FakeGramIterator> make_iter(std::map<std::string, std::string> props) {
    auto it = std::make_shared<FakeGramIterator>();
    it->props = std::move(props);
    return it;
}

// 注册 gram 族 analyzer 策略，供 resolve_gram_scheme 使用（同 snii_writer_test.cpp:71-101 的注入方式）
static void install_gram_policy(const std::string& name, const std::string& mode) { /* TIndexPolicy tokenizer{type=ngram, mode} + analyzer{tokenizer=name} → apply_policy_changes */ }

TEST(LikeGramIndexTest, RegexpCompilesAndSendsGramBooleanQuery) {
    install_gram_policy("gram_dense", "dense");
    auto it = make_iter({{"analyzer", "gram_dense"}});
    auto fn = std::make_shared<FunctionRegexpLike>();
    ColumnsWithTypeAndName args {{ColumnString::create_const("hello|world"), std::make_shared<DataTypeString>(), "pattern"}};
    std::vector<IndexFieldNameAndTypePair> names {{"msg", std::make_shared<DataTypeString>()}};
    segment_v2::InvertedIndexResultBitmap result;
    ASSERT_TRUE(fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result).ok());
    EXPECT_EQ(it->last.query_type, segment_v2::InvertedIndexQueryType::GRAM_BOOLEAN_QUERY);
    segment_v2::gram::GramQuery q;
    ASSERT_TRUE(segment_v2::gram::GramQuery::parse(it->last.query_value.get<String>(), &q).ok());
    EXPECT_EQ(q.to_debug_string(), "((\"ell\" & \"hel\" & \"llo\") | (\"orl\" & \"rld\" & \"wor\"))");
    EXPECT_TRUE(result.approximate());
    EXPECT_EQ(result.get_data_bitmap()->cardinality(), 2u);
}

TEST(LikeGramIndexTest, UnindexableOrNonGramProducesNoResult) {
    install_gram_policy("gram_dense", "dense");
    auto it = make_iter({{"analyzer", "gram_dense"}});
    auto fn = std::make_shared<FunctionRegexpLike>();
    std::vector<IndexFieldNameAndTypePair> names {{"msg", std::make_shared<DataTypeString>()}};
    segment_v2::InvertedIndexResultBitmap result;
    ColumnsWithTypeAndName args {{ColumnString::create_const("[0-9]{3}-[0-9]{4}"), std::make_shared<DataTypeString>(), "p"}};
    ASSERT_TRUE(fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result).ok());
    EXPECT_TRUE(result.is_empty());          // 未产生结果（is_empty = 无位图）
    auto it2 = make_iter({{"parser", "english"}});
    ColumnsWithTypeAndName args2 {{ColumnString::create_const("hello"), std::make_shared<DataTypeString>(), "p"}};
    ASSERT_TRUE(fn->evaluate_inverted_index(args2, names, {it2.get()}, 100, nullptr, result).ok());
    EXPECT_TRUE(result.is_empty());
}

TEST(LikeGramIndexTest, LikeUsesLikeCompiler) {
    install_gram_policy("gram_dense", "dense");
    auto it = make_iter({{"analyzer", "gram_dense"}});
    auto fn = std::make_shared<FunctionLike>();
    std::vector<IndexFieldNameAndTypePair> names {{"msg", std::make_shared<DataTypeString>()}};
    segment_v2::InvertedIndexResultBitmap result;
    ColumnsWithTypeAndName args {{ColumnString::create_const("%abcd%"), std::make_shared<DataTypeString>(), "p"}};
    ASSERT_TRUE(fn->evaluate_inverted_index(args, names, {it.get()}, 100, nullptr, result).ok());
    segment_v2::gram::GramQuery q;
    ASSERT_TRUE(segment_v2::gram::GramQuery::parse(it->last.query_value.get<String>(), &q).ok());
    EXPECT_EQ(q.to_debug_string(), "(\"abc\" & \"bcd\")");
}

}  // namespace doris
```

（`FakeGramIterator` 的桩 reader：派生 `segment_v2::InvertedIndexReader`，覆写 `get_index_properties()` 返回 `props`，其余纯虚函数返回 `Status::NotSupported`；`install_gram_policy` 按 `snii_writer_test.cpp:71-101` 的 `ScopedCommonGramsPolicies` 复制，改为通用名 `ScopedIndexPolicies` 后共用。`result.is_empty()` 的含义按 `inverted_index_reader.h:80` 的定义核对：它判断的是位图指针为空，不是基数为零。）

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=LikeGramIndexTest.*`
Expected: 编译失败（`evaluate_inverted_index` 未覆写、`approximate()` 不存在、`GRAM_BOOLEAN_QUERY` 已由 Task 9 提供）

- [ ] **Step 3: 实现**

```cpp
// inverted_index_reader.h  InvertedIndexResultBitmap 内新增
    void set_approximate(bool v) { _approximate = v; }
    bool approximate() const { return _approximate; }
private:
    bool _approximate = false;   // 拷贝/赋值随成员默认复制
```

```cpp
// config.h / config.cpp
DECLARE_mBool(enable_gram_index_regexp);
DEFINE_mBool(enable_gram_index_regexp, "true");   // LIKE/REGEXP 是否尝试 gram 族倒排索引（总开关）
```

```cpp
// like.h  FunctionLikeBase（:279）内新增
protected:
    enum class GramCompileKind { LIKE, REGEXP };
    Status evaluate_gram_index(GramCompileKind kind, const ColumnsWithTypeAndName& arguments,
                               const std::vector<IndexFieldNameAndTypePair>& data_type_with_names,
                               std::vector<segment_v2::IndexIterator*> iterators, uint32_t num_rows,
                               segment_v2::InvertedIndexResultBitmap& bitmap_result) const;
// FunctionLike（:378）与 FunctionRegexpLike（:412）各加：
    Status evaluate_inverted_index(const ColumnsWithTypeAndName& arguments,
                                   const std::vector<IndexFieldNameAndTypePair>& data_type_with_names,
                                   std::vector<segment_v2::IndexIterator*> iterators, uint32_t num_rows,
                                   const InvertedIndexAnalyzerCtx* analyzer_ctx,
                                   segment_v2::InvertedIndexResultBitmap& bitmap_result) const override {
        return evaluate_gram_index(GramCompileKind::LIKE /* 或 REGEXP */, arguments, data_type_with_names, iterators, num_rows, bitmap_result);
    }
```

```cpp
// like.cpp 新增
#include "common/config.h"
#include "runtime/exec_env.h"
#include "storage/index/inverted/gram/gram_family.h"
#include "storage/index/inverted/gram/regex_gram_compiler.h"
#include "storage/index/inverted/inverted_index_iterator.h"
#include "storage/index/inverted/inverted_index_reader.h"

Status FunctionLikeBase::evaluate_gram_index(GramCompileKind kind, const ColumnsWithTypeAndName& arguments,
                                             const std::vector<IndexFieldNameAndTypePair>& data_type_with_names,
                                             std::vector<segment_v2::IndexIterator*> iterators, uint32_t num_rows,
                                             segment_v2::InvertedIndexResultBitmap& bitmap_result) const {
    if (!config::enable_gram_index_regexp || iterators.size() != 1 || iterators[0] == nullptr || arguments.empty() ||
        data_type_with_names.size() != 1) {
        return Status::OK();
    }
    if (kind == GramCompileKind::LIKE && arguments.size() >= 2) {
        Field esc; arguments[1].column->get(0, esc);
        if (esc.is_null() || esc.get<String>() != "\\") return Status::OK();   // P0：只支持默认转义
    }
    Field pattern_field; arguments[0].column->get(0, pattern_field);
    if (pattern_field.is_null()) return Status::OK();
    const std::string pattern = pattern_field.get<String>();

    auto* iter = iterators[0];
    auto reader = iter->get_reader(segment_v2::InvertedIndexReaderType::FULLTEXT);
    if (reader == nullptr) return Status::OK();
    auto* inverted_reader = dynamic_cast<segment_v2::InvertedIndexReader*>(reader.get());
    if (inverted_reader == nullptr) return Status::OK();
    auto scheme = segment_v2::gram::resolve_gram_scheme(inverted_reader->get_index_properties(),
                                                        ExecEnv::GetInstance()->index_policy_mgr());
    if (!scheme.has_value()) return Status::OK();

    segment_v2::gram::RegexGramCompiler compiler(*scheme);
    segment_v2::gram::GramQuery q;
    RETURN_IF_ERROR(kind == GramCompileKind::LIKE ? compiler.compile_like(pattern, &q) : compiler.compile_regexp(pattern, &q));
    if (q.is_all()) return Status::OK();

    segment_v2::InvertedIndexParam param;
    param.column_name = data_type_with_names[0].first;
    param.column_type = data_type_with_names[0].second;
    param.query_value = Field::create_field<TYPE_STRING>(q.serialize());
    param.query_type = segment_v2::InvertedIndexQueryType::GRAM_BOOLEAN_QUERY;
    param.num_rows = num_rows;
    param.roaring = std::make_shared<roaring::Roaring>();
    Status st = iter->read_from_index(&param);
    if (st.is<ErrorCode::INVERTED_INDEX_NOT_SUPPORTED>() || st.is<ErrorCode::NOT_IMPLEMENTED_ERROR>() ||
        st.is<ErrorCode::INVERTED_INDEX_EVALUATE_SKIPPED>()) {
        return Status::OK();   // CLucene 格式或旧段：不加速
    }
    RETURN_IF_ERROR(st);
    segment_v2::InvertedIndexResultBitmap result(param.roaring, nullptr);
    result.set_approximate(true);
    bitmap_result = result;
    return Status::OK();
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter='LikeGramIndexTest.*:FunctionLikeTest.*'`
Expected: PASS；既有 like 函数 UT 不受影响

- [ ] **Step 5: 提交**

```bash
git add be/src/exprs/function/like.h be/src/exprs/function/like.cpp be/src/storage/index/inverted/inverted_index_reader.h be/src/common/config.h be/src/common/config.cpp be/test/exprs/function/like_gram_index_test.cpp
git commit -m "feat(be): like/regexp evaluate_inverted_index via gram boolean query (approximate result)"
```

---

### Task 11: 近似索引语义 —— 位图裁剪但保留表达式复验 + profile 计数

**Files:**
- Modify: `be/src/exprs/vexpr_context.h:63-185`（`IndexExecContext` 新增近似结果表）
- Modify: `be/src/exprs/vexpr.cpp:1029-1034`（结果分流）
- Modify: `be/src/storage/segment/segment_iterator.cpp:1277-1284`（近似分支）
- Modify: `be/src/storage/olap_common.h:330`、`be/src/exec/operator/olap_scan_operator.h:268`、`olap_scan_operator.cpp:294`、`be/src/exec/scan/olap_scanner.cpp:885`（计数器）
- Test: `be/test/exprs/index_exec_context_test.cpp`（近似表行为）；端到端语义由 Task 15 回归覆盖

**Interfaces:**
- Consumes（勘察行号）:
  - `IndexExecContext::set_index_result_for_expr / get_index_result_for_expr / set_true_for_index_status`（`vexpr_context.h:101-137`）；`_index_result_bitmap`（`:172`）、`_index_result_column`（`:175`，被 `vexpr.cpp:1088-1099` 的 fast_execute 劫持读取——**近似结果绝不能进入**）。
  - `vexpr.cpp:1029-1034`：`if (!result_bitmap.is_empty()) { set_index_result_for_expr(this, result_bitmap); for cid: set_true_for_index_status(this, cid); }`。
  - `segment_iterator.cpp:1277-1284`（消费精确结果并 `consumed_by_index.push_back`），`:1359-1363`（`erase_if` 移除），`:847-858`（二次清扫，只看 `all_expr_inverted_index_evaluated()`），`:865-870`（列 `need_read_data=false` 判定，依赖 `_common_expr_index_exec_status`）。
- Produces:
  ```cpp
  // vexpr_context.h IndexExecContext
  void set_approx_index_result_for_expr(const VExpr* expr, segment_v2::InvertedIndexResultBitmap bitmap);
  const segment_v2::InvertedIndexResultBitmap* get_approx_index_result_for_expr(const VExpr* expr) const;
  // olap_common.h OlapReaderStatistics
  int64_t rows_gram_index_filtered = 0;      // 近似索引裁掉的行数
  int64_t gram_index_candidate_rows = 0;     // 近似索引给出的候选行数
  // profile: RowsGramIndexFiltered / GramIndexCandidateRows
  ```

不变量（写进代码注释与测试）：近似结果 (a) 不进入 `_index_result_bitmap/_index_result_column`（否则 fast_execute 会跳过函数执行，直接把候选当结果）；(b) 不调用 `set_true_for_index_status`（否则列会被标成不读数据，复验无列可读）；(c) 只在 `expr_ctx->root()` 就是该函数本身时应用到 `_row_bitmap`（顶层 AND 语境；被 NOT/OR 包裹时 `VCompoundPred` 看不到近似表，自然不生效）。

- [ ] **Step 1: 写失败测试**

```cpp
// be/test/exprs/index_exec_context_test.cpp
#include <gtest/gtest.h>

#include "exprs/vexpr_context.h"
#include "storage/index/inverted/inverted_index_reader.h"

namespace doris {

TEST(IndexExecContextTest, ApproxResultIsIsolatedFromExactTables) {
    std::vector<std::unique_ptr<segment_v2::IndexIterator>> iters;
    std::vector<IndexFieldNameAndTypePair> names;
    std::unordered_map<const VExpr*, std::unordered_map<int32_t, bool>> status;
    IndexExecContext ctx(iters, names, status /* 其余参数按 vexpr_context.h:63 构造签名补齐 */);
    const VExpr* key = reinterpret_cast<const VExpr*>(0x1);
    auto bm = std::make_shared<roaring::Roaring>();
    bm->add(7);
    segment_v2::InvertedIndexResultBitmap r(bm, nullptr);
    r.set_approximate(true);
    ctx.set_approx_index_result_for_expr(key, r);
    ASSERT_NE(ctx.get_approx_index_result_for_expr(key), nullptr);
    EXPECT_TRUE(ctx.get_approx_index_result_for_expr(key)->approximate());
    EXPECT_FALSE(ctx.has_index_result_for_expr(key));          // 精确表里没有
    EXPECT_TRUE(ctx.get_index_result_bitmap().empty());        // 不会被物化成 index result column
    EXPECT_TRUE(status.empty());                               // 未标记列状态
}

}  // namespace doris
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd be && ./run-be-ut.sh --run --filter=IndexExecContextTest.*`
Expected: 编译失败（新方法不存在）

- [ ] **Step 3: 实现**

```cpp
// vexpr_context.h IndexExecContext（:101 附近）
    void set_approx_index_result_for_expr(const VExpr* expr, segment_v2::InvertedIndexResultBitmap bitmap) {
        _approx_index_result_bitmap[expr] = std::move(bitmap);
    }
    const segment_v2::InvertedIndexResultBitmap* get_approx_index_result_for_expr(const VExpr* expr) const {
        auto it = _approx_index_result_bitmap.find(expr);
        return it == _approx_index_result_bitmap.end() ? nullptr : &it->second;
    }
private:
    // 近似（超集）结果：只用于裁 _row_bitmap，表达式仍会在候选行上执行；与 _index_result_bitmap 严格隔离
    std::unordered_map<const VExpr*, segment_v2::InvertedIndexResultBitmap> _approx_index_result_bitmap;
```

```cpp
// vexpr.cpp:1029-1034 改为
    if (!result_bitmap.is_empty()) {
        if (result_bitmap.approximate()) {
            index_context->set_approx_index_result_for_expr(this, result_bitmap);   // 不标列状态、不进精确表
        } else {
            index_context->set_index_result_for_expr(this, result_bitmap);
            for (int cid : column_ids) index_context->set_true_for_index_status(this, cid);
        }
    }
```

```cpp
// segment_iterator.cpp:1284 之后（与精确分支并列的 else-if）
        } else if (const auto* approx = expr_ctx->get_index_context()->get_approx_index_result_for_expr(expr_ctx->root().get());
                   approx != nullptr) {
            const uint64_t before = _row_bitmap.cardinality();
            _row_bitmap &= *approx->get_data_bitmap();
            _opts.stats->gram_index_candidate_rows += _row_bitmap.cardinality();
            _opts.stats->rows_gram_index_filtered += before - _row_bitmap.cardinality();
            // 不 push consumed_by_index：表达式留在 _common_expr_ctxs_push_down，于 _execute_common_expr（:3159）复验
        }
```

计数器三段式（照 `InvertedIndexConjunctsShortCircuited` 复制）：`olap_common.h:330` 后加两个字段；`olap_scan_operator.h:268` 后加 `RuntimeProfile::Counter* _gram_index_filter_counter = nullptr; RuntimeProfile::Counter* _gram_index_candidate_counter = nullptr;`；`olap_scan_operator.cpp:294` 后 `ADD_COUNTER_WITH_LEVEL(_segment_profile, "RowsGramIndexFiltered", TUnit::UNIT, 1)` 与 `"GramIndexCandidateRows"`；`olap_scanner.cpp:885` 后两条 `COUNTER_UPDATE`。

- [ ] **Step 4: 跑测试确认通过**

Run: `cd be && ./run-be-ut.sh --run --filter='IndexExecContextTest.*:SegmentIteratorTest.*'`
Expected: PASS；`SegmentIteratorTest` 既有用例不变

- [ ] **Step 5: 提交**

```bash
git add be/src/exprs/vexpr_context.h be/src/exprs/vexpr.cpp be/src/storage/segment/segment_iterator.cpp be/src/storage/olap_common.h be/src/exec/operator/olap_scan_operator.h be/src/exec/operator/olap_scan_operator.cpp be/src/exec/scan/olap_scanner.cpp be/test/exprs/index_exec_context_test.cpp
git commit -m "feat(be): approximate index results prune row bitmap while keeping the expression for re-check"
```


## 阶段 D：FE（参数校验、analyzer 图谱约束、索引属性默认值）

> 事实：策略属性经 `PushIndexPolicyTask.toThrift()`（`fe/fe-core/.../task/PushIndexPolicyTask.java:44-53`）以 `map<string,string>` 原样下发到 BE（`TIndexPolicy.properties`，`gensrc/thrift/AgentService.thrift:164-169`），新参数**零 thrift 改动**；FE 无「函数允许上索引」标记，因此阶段 D 不涉及 Nereids 规则。FE 已允许同列多个 INVERTED 索引（`InvertedIndexUtil.canHaveMultipleInvertedIndexes` `:398-415`，按 analyzer identity 去重），设计文档开放问题 Q4 无需改动即成立。

### Task 13: NGramTokenizerValidator 新参数

**Files:**
- Modify: `fe/fe-core/src/main/java/org/apache/doris/indexpolicy/NGramTokenizerValidator.java:30-38`（白名单）、`:46-105`（`validateSpecific`）
- Test: `fe/fe-core/src/test/java/org/apache/doris/indexpolicy/PolicyValidatorTests.java`（追加，模板 `:127-155`）

**Interfaces:**
- Consumes: `BasePolicyValidator.validate(Map<String,String>)`（`BasePolicyValidator.java:25-51`：白名单外的 key 抛 `"<type> does not support parameter '<key>'. Allowed parameters: ..."`）；分发点 `IndexPolicyMgr.validateTokenizerProperties`（`:455-496`，`case "ngram"` `:465-466`，无需改）。
- Produces: 白名单新增 `mode`、`density`、`stop_gram_df`、`lower_case`；校验规则：
  - `mode` ∈ {`auto`,`sparse`,`dense`}；
  - `density`、`stop_gram_df`、`lower_case` 仅在 `mode` 存在时允许；`density` ∈ (0, 1]，`stop_gram_df` ∈ [0, 1]，`lower_case` ∈ {true,false}；
  - `mode` 存在时：`min_gram` 默认 3、`max_gram` 默认 16，只要求 `min_gram ≤ max_gram`（legacy 的 `max_gram - min_gram ≤ 1` 约束不适用）；`token_chars/custom_token_chars` 与 `mode` 互斥（gram 族按脚本切分，不支持字符类过滤）。

- [ ] **Step 1: 写失败测试**

```java
// 追加到 PolicyValidatorTests.java
@Test
public void testNGramValidator_GramModeSparse() throws DdlException {
    NGramTokenizerValidator validator = new NGramTokenizerValidator();
    Map<String, String> props = new HashMap<>();
    props.put("type", "ngram");
    props.put("mode", "sparse");
    props.put("min_gram", "3");
    props.put("max_gram", "16");
    props.put("density", "0.25");
    props.put("stop_gram_df", "0.10");
    props.put("lower_case", "true");
    validator.validate(props);   // 不抛
}

@Test
public void testNGramValidator_GramModeRejectsBadValues() {
    NGramTokenizerValidator validator = new NGramTokenizerValidator();
    Map<String, String> bad = new HashMap<>();
    bad.put("type", "ngram");
    bad.put("mode", "fuzzy");
    DdlException e1 = Assertions.assertThrows(DdlException.class, () -> validator.validate(bad));
    Assertions.assertTrue(e1.getMessage().contains("mode must be one of"));

    Map<String, String> noMode = new HashMap<>();
    noMode.put("type", "ngram");
    noMode.put("density", "0.25");
    DdlException e2 = Assertions.assertThrows(DdlException.class, () -> validator.validate(noMode));
    Assertions.assertTrue(e2.getMessage().contains("requires mode"));

    Map<String, String> badDensity = new HashMap<>();
    badDensity.put("type", "ngram");
    badDensity.put("mode", "sparse");
    badDensity.put("density", "1.5");
    Assertions.assertTrue(Assertions.assertThrows(DdlException.class, () -> validator.validate(badDensity))
            .getMessage().contains("density must be"));

    Map<String, String> tokenChars = new HashMap<>();
    tokenChars.put("type", "ngram");
    tokenChars.put("mode", "dense");
    tokenChars.put("token_chars", "letter");
    Assertions.assertTrue(Assertions.assertThrows(DdlException.class, () -> validator.validate(tokenChars))
            .getMessage().contains("token_chars cannot be used"));

    Map<String, String> wideGap = new HashMap<>();   // mode 存在时允许 max-min>1
    wideGap.put("type", "ngram");
    wideGap.put("mode", "sparse");
    wideGap.put("min_gram", "3");
    wideGap.put("max_gram", "24");
    Assertions.assertDoesNotThrow(() -> validator.validate(wideGap));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `sh run-fe-ut.sh --run org.apache.doris.indexpolicy.PolicyValidatorTests`
Expected: 新用例失败（`mode` 不在白名单：`ngram tokenizer does not support parameter 'mode'`）

- [ ] **Step 3: 实现**

```java
// NGramTokenizerValidator.java
private static final Set<String> ALLOWED_PROPS = ImmutableSet.of(
        "type", "min_gram", "max_gram", "token_chars", "custom_token_chars",
        "mode", "density", "stop_gram_df", "lower_case");
private static final Set<String> VALID_MODES = ImmutableSet.of("auto", "sparse", "dense");

@Override
protected void validateSpecific(Map<String, String> props) throws DdlException {
    String mode = props.get("mode");
    if (mode != null) {
        validateGramMode(props, mode);
        return;   // gram 族有独立规则，不再走 legacy 校验
    }
    for (String k : new String[] {"density", "stop_gram_df", "lower_case"}) {
        if (props.containsKey(k)) {
            throw new DdlException("ngram tokenizer parameter '" + k + "' requires mode = auto|sparse|dense");
        }
    }
    // ... 原有 :47-104 的 legacy 校验保持不变 ...
}

private void validateGramMode(Map<String, String> props, String mode) throws DdlException {
    if (!VALID_MODES.contains(mode.toLowerCase())) {
        throw new DdlException("ngram tokenizer mode must be one of " + VALID_MODES + ", got: " + mode);
    }
    int minGram = parsePositiveInt(props, "min_gram", 3);
    int maxGram = parsePositiveInt(props, "max_gram", 16);
    if (minGram > maxGram) {
        throw new DdlException("min_gram (" + minGram + ") must be <= max_gram (" + maxGram + ")");
    }
    if (props.containsKey("density")) {
        double d = parseDouble(props.get("density"), "density");
        if (!(d > 0.0 && d <= 1.0)) {
            throw new DdlException("density must be in (0, 1], got: " + props.get("density"));
        }
    }
    if (props.containsKey("stop_gram_df")) {
        double t = parseDouble(props.get("stop_gram_df"), "stop_gram_df");
        if (!(t >= 0.0 && t <= 1.0)) {
            throw new DdlException("stop_gram_df must be in [0, 1], got: " + props.get("stop_gram_df"));
        }
    }
    if (props.containsKey("lower_case") && !props.get("lower_case").matches("true|false")) {
        throw new DdlException("lower_case must be true or false, got: " + props.get("lower_case"));
    }
    if (props.containsKey("token_chars") || props.containsKey("custom_token_chars")) {
        throw new DdlException("token_chars cannot be used together with mode (gram tokenizer splits by script)");
    }
}

private static int parsePositiveInt(Map<String, String> props, String key, int dflt) throws DdlException {
    if (!props.containsKey(key)) {
        return dflt;
    }
    try {
        int v = Integer.parseInt(props.get(key));
        if (v <= 0) {
            throw new DdlException(key + " must be a positive integer, got: " + props.get(key));
        }
        return v;
    } catch (NumberFormatException e) {
        throw new DdlException(key + " must be a positive integer, got: " + props.get(key));
    }
}

private static double parseDouble(String v, String key) throws DdlException {
    try {
        return Double.parseDouble(v);
    } catch (NumberFormatException e) {
        throw new DdlException(key + " must be a number, got: " + v);
    }
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: `sh run-fe-ut.sh --run org.apache.doris.indexpolicy.PolicyValidatorTests`
Expected: 全部 PASS（含原有 ngram 用例 `:127-155`）

- [ ] **Step 5: 提交**

```bash
git add fe/fe-core/src/main/java/org/apache/doris/indexpolicy/NGramTokenizerValidator.java fe/fe-core/src/test/java/org/apache/doris/indexpolicy/PolicyValidatorTests.java
git commit -m "feat(fe): ngram tokenizer mode/density/stop_gram_df/lower_case validation"
```

---

### Task 14: analyzer 图谱约束与索引属性约束（gram 族）

**Files:**
- Modify: `fe/fe-core/src/main/java/org/apache/doris/indexpolicy/IndexPolicyMgr.java:125-137,291-340`（仿 `validateAnalyzerUsesCommonGrams / validateAnalyzerGraphLocked` 新增 `resolveGramTokenizerMode`）
- Modify: `fe/fe-core/src/main/java/org/apache/doris/analysis/InvertedIndexUtil.java:348-372`（`checkAnalyzerName` 仿 common_grams 加 gram 族约束）
- Test: `fe/fe-core/src/test/java/org/apache/doris/indexpolicy/GramDdlValidationTest.java`（模板 `CommonGramsDdlValidationTest.java:29-40`）

**Interfaces:**
- Consumes: `IndexPolicyMgr.resolveAnalyzerComponentLocked`（`:356`，返回 `componentType/referenceName/properties`）、`validateAnalyzerGraphLocked`（`:291-340`，解析 `tokenizer`/`token_filter` 链）、`IndexPolicy.COMMON_GRAMS_TYPE` 的判定写法（`:307`）；`InvertedIndexUtil.checkAnalyzerName`（`:348-372`：common_grams 要求 char family、SNII、`support_phrase=true` 的范例）；`Index.java:76-86`（有 analyzer 时 `support_phrase` 默认 true）。
- Produces:
  ```java
  // IndexPolicyMgr
  /** analyzer 的 tokenizer 是 ngram 且带 mode 时返回 mode（小写），否则 Optional.empty() */
  public Optional<String> resolveGramTokenizerMode(String analyzerName);
  // 创建 ANALYZER 策略时（validatePolicyProperties :222-243 → 图谱校验）：tokenizer 为 ngram(mode=sparse|auto) 且 token_filter 含 lowercase → DdlException
  // InvertedIndexUtil.checkAnalyzerName：analyzer 为 gram 族时
  //   - 存储格式必须为 SNII，否则 "gram tokenizer (mode=...) requires inverted_index_storage_format = SNII"
  //   - properties 里 support_phrase 显式为 true → "gram tokenizer index does not support phrase (support_phrase must be false)"
  //   - 未显式给出时写入 properties.put("support_phrase", "false")（覆盖 Index.java 的默认 true）
  ```

- [ ] **Step 1: 写失败测试**

```java
// fe/fe-core/src/test/java/org/apache/doris/indexpolicy/GramDdlValidationTest.java
public class GramDdlValidationTest {
    private IndexPolicyMgr manager;

    @BeforeEach
    public void setUp() {
        manager = new IndexPolicyMgr();
        manager.replayCreateIndexPolicy(policy(1L, "gram_sparse_tok", IndexPolicyTypeEnum.TOKENIZER,
                Map.of("type", "ngram", "mode", "sparse")));
        manager.replayCreateIndexPolicy(policy(2L, "gram_sparse", IndexPolicyTypeEnum.ANALYZER,
                Map.of("tokenizer", "gram_sparse_tok")));
        manager.replayCreateIndexPolicy(policy(3L, "plain_tok", IndexPolicyTypeEnum.TOKENIZER,
                Map.of("type", "ngram", "min_gram", "2", "max_gram", "3")));
        manager.replayCreateIndexPolicy(policy(4L, "plain", IndexPolicyTypeEnum.ANALYZER,
                Map.of("tokenizer", "plain_tok")));
    }

    @Test
    public void testResolveGramMode() {
        Assertions.assertEquals(Optional.of("sparse"), manager.resolveGramTokenizerMode("gram_sparse"));
        Assertions.assertEquals(Optional.empty(), manager.resolveGramTokenizerMode("plain"));
        Assertions.assertEquals(Optional.empty(), manager.resolveGramTokenizerMode("english"));
    }

    @Test
    public void testLowercaseFilterRejectedWithSparseMode() {
        Map<String, String> props = new HashMap<>();
        props.put("tokenizer", "gram_sparse_tok");
        props.put("token_filter", "lowercase");
        DdlException e = Assertions.assertThrows(DdlException.class,
                () -> manager.validatePolicyProperties("bad_analyzer", IndexPolicyTypeEnum.ANALYZER, props));
        Assertions.assertTrue(e.getMessage().contains("lowercase token filter cannot be combined"));
    }

    @Test
    public void testIndexPropertiesForGramAnalyzer() throws Exception {
        // 模拟 InvertedIndexUtil.checkAnalyzerName 的调用：需 Env 持有本 manager（用 CheckScoreUsageTest.java:66-77 的 Mockito 方式注入）
        Map<String, String> props = new HashMap<>();
        props.put("analyzer", "gram_sparse");
        InvertedIndexUtil.checkAnalyzerName(props, TInvertedIndexFileStorageFormat.SNII);
        Assertions.assertEquals("false", props.get("support_phrase"));

        Map<String, String> phrase = new HashMap<>();
        phrase.put("analyzer", "gram_sparse");
        phrase.put("support_phrase", "true");
        Assertions.assertTrue(Assertions.assertThrows(AnalysisException.class,
                () -> InvertedIndexUtil.checkAnalyzerName(phrase, TInvertedIndexFileStorageFormat.SNII))
                .getMessage().contains("does not support phrase"));

        Map<String, String> v2 = new HashMap<>();
        v2.put("analyzer", "gram_sparse");
        Assertions.assertTrue(Assertions.assertThrows(AnalysisException.class,
                () -> InvertedIndexUtil.checkAnalyzerName(v2, TInvertedIndexFileStorageFormat.V2))
                .getMessage().contains("requires inverted_index_storage_format = SNII"));
    }

    private static IndexPolicy policy(long id, String name, IndexPolicyTypeEnum type, Map<String, String> props) {
        return IndexPolicy.create(name, type, new HashMap<>(props));   // 以 IndexPolicy.create 的真实签名为准（IndexPolicy.java:96+）
    }
}
```

（`checkAnalyzerName` 的现有签名以 `InvertedIndexUtil.java:348` 为准；若它不接收存储格式参数，则从调用方 `IndexDefinition.checkColumn :261-268` 传入，测试相应调整。）

- [ ] **Step 2: 跑测试确认失败**

Run: `sh run-fe-ut.sh --run org.apache.doris.indexpolicy.GramDdlValidationTest`
Expected: 编译失败（`resolveGramTokenizerMode` 不存在）

- [ ] **Step 3: 实现**

```java
// IndexPolicyMgr.java（仿 validateAnalyzerUsesCommonGrams :125-137）
public Optional<String> resolveGramTokenizerMode(String analyzerName) {
    if (analyzerName == null || IndexPolicy.BUILTIN_ANALYZERS.contains(normalizeKey(analyzerName))) {
        return Optional.empty();
    }
    readLock();
    try {
        IndexPolicy analyzer = nameToIndexPolicy.get(normalizeKey(analyzerName));
        if (analyzer == null || analyzer.getType() != IndexPolicyTypeEnum.ANALYZER) {
            return Optional.empty();
        }
        String tokenizerRef = analyzer.getProperties().get(IndexPolicy.PROP_TOKENIZER);
        AnalyzerComponent tokenizer = resolveAnalyzerComponentLocked(tokenizerRef, IndexPolicyTypeEnum.TOKENIZER); // :356
        if (tokenizer == null || !"ngram".equals(tokenizer.componentType)) {
            return Optional.empty();
        }
        String mode = tokenizer.properties.get("mode");
        return mode == null ? Optional.empty() : Optional.of(mode.toLowerCase());
    } finally {
        readUnlock();
    }
}

// validateAnalyzerGraphLocked（:291-340）在解析完 tokenizer 与 token_filter 链后追加：
if ("ngram".equals(tokenizer.componentType)) {
    String mode = tokenizer.properties.get("mode");
    if (mode != null && !"dense".equalsIgnoreCase(mode)) {
        for (AnalyzerComponent filter : tokenFilters) {
            if ("lowercase".equals(filter.componentType)) {
                throw new DdlException("lowercase token filter cannot be combined with ngram tokenizer mode=" + mode
                        + "; use the tokenizer's own lower_case=true (folding must happen before gram boundaries)");
            }
        }
    }
}
```

```java
// InvertedIndexUtil.checkAnalyzerName（:348-372，仿 common_grams 分支）
Optional<String> gramMode = Env.getCurrentEnv().getIndexPolicyMgr().resolveGramTokenizerMode(analyzerName);
if (gramMode.isPresent()) {
    if (storageFormat != TInvertedIndexFileStorageFormat.SNII) {
        throw new AnalysisException("gram tokenizer (mode=" + gramMode.get()
                + ") requires inverted_index_storage_format = SNII");
    }
    String phrase = properties.get(InvertedIndexProperties.INVERTED_INDEX_PARSER_PHRASE_SUPPORT_KEY);
    if ("true".equalsIgnoreCase(phrase)) {
        throw new AnalysisException("gram tokenizer index does not support phrase (support_phrase must be false)");
    }
    properties.put(InvertedIndexProperties.INVERTED_INDEX_PARSER_PHRASE_SUPPORT_KEY, "false");
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: `sh run-fe-ut.sh --run org.apache.doris.indexpolicy.GramDdlValidationTest,org.apache.doris.indexpolicy.CommonGramsDdlValidationTest`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add fe/fe-core/src/main/java/org/apache/doris/indexpolicy/IndexPolicyMgr.java fe/fe-core/src/main/java/org/apache/doris/analysis/InvertedIndexUtil.java fe/fe-core/src/test/java/org/apache/doris/indexpolicy/GramDdlValidationTest.java
git commit -m "feat(fe): gram-family analyzer constraints (SNII only, docs-only, no lowercase filter with sparse mode)"
```


## 阶段 E：回归与验收

### Task 15: 端到端语义对照回归

**Files:**
- Create: `regression-test/suites/inverted_index_p0/gram/test_gram_regexp_like.groovy`
- Create: `regression-test/data/inverted_index_p0/gram/test_gram_regexp_like.out`（用 `-genOut` 生成后人工核对）

**Interfaces:**
- Consumes: `CREATE INVERTED INDEX TOKENIZER/ANALYZER`（范例 `analyzer/test_custom_analyzer.groovy:24-51`）；SNII 建表与同列双索引范例 `storage_format/test_common_grams_snii.groovy:185-190`；索引开关 `SET enable_inverted_index_query=false`（`test_inverted_is_null.groovy:67-71`）；`order_qt_` 比对（`Suite.groovy:1934-1937`）。
- 覆盖矩阵（设计文档 §9.1 第 4 条）：NULL / 空串 / 短值 / CJK / 大小写（`(?i)` 与 `lower_case=true` 的 tokenizer）/ 交替 / 可选组 / 无字面量模式 / `NOT REGEXP` / `LIKE` 通配 / 多 segment（两次 INSERT 之间 `sync`）/ 与语言分词索引同列共存 / delete 后查询。

- [ ] **Step 1: 写用例**

```groovy
// regression-test/suites/inverted_index_p0/gram/test_gram_regexp_like.groovy
suite("test_gram_regexp_like", "p0") {
    def tbl = "t_gram_regexp_like"
    sql "DROP TABLE IF EXISTS ${tbl}"
    sql """CREATE INVERTED INDEX TOKENIZER IF NOT EXISTS gram_sparse_tok
           PROPERTIES ("type" = "ngram", "mode" = "sparse", "min_gram" = "3", "max_gram" = "16", "density" = "0.25")"""
    sql """CREATE INVERTED INDEX ANALYZER IF NOT EXISTS gram_sparse
           PROPERTIES ("tokenizer" = "gram_sparse_tok")"""
    sql """CREATE INVERTED INDEX TOKENIZER IF NOT EXISTS gram_dense_lc_tok
           PROPERTIES ("type" = "ngram", "mode" = "dense", "min_gram" = "3", "lower_case" = "true")"""
    sql """CREATE INVERTED INDEX ANALYZER IF NOT EXISTS gram_dense_lc
           PROPERTIES ("tokenizer" = "gram_dense_lc_tok")"""

    sql """CREATE TABLE ${tbl} (
             id INT,
             msg VARCHAR(512),
             INDEX idx_msg_gram (msg) USING INVERTED PROPERTIES ("analyzer" = "gram_sparse"),
             INDEX idx_msg_lc   (msg) USING INVERTED PROPERTIES ("analyzer" = "gram_dense_lc"),
             INDEX idx_msg_en   (msg) USING INVERTED PROPERTIES ("parser" = "english")
           ) DUPLICATE KEY(id) DISTRIBUTED BY HASH(id) BUCKETS 1
           PROPERTIES ("replication_num" = "1", "inverted_index_storage_format" = "SNII")"""

    sql """INSERT INTO ${tbl} VALUES
        (1, 'rpc error: code = Unavailable desc = error reading from server'),
        (2, 'user_id="eacb47f6-967d-11f0-b88d-8eb93cba8bdb" user_currency="USD"'),
        (3, 'Convert conversion successful'),
        (4, '手机微博 POST 10.68.3.18:8080 error'),
        (5, NULL),
        (6, ''),
        (7, 'ab'),
        (8, 'GET /images/x.gif HTTP/1.0'),
        (9, 'CODE = UNAVAILABLE'),
        (10, 'context deadline exceeded'),
        (11, 'failed to charge card: rpc error'),
        (12, 'timeout after error error error')"""
    sql "sync"
    // 第二个 segment / rowset
    sql """INSERT INTO ${tbl} VALUES
        (13, 'rpc error: code = Internal desc = boom'),
        (14, '微博手机'),
        (15, 'abc'),
        (16, 'Sending Quote: 12.5')"""
    sql "sync"

    def patterns = [
        "rpc error: code = Unavailable",
        "error.*timeout",
        "code = (Unavailable|Internal)",
        "user_id=\"[0-9a-f]{8}-",
        "conn(ection)? re(set|fused)",
        "GET|POST",
        "[0-9]{3}-[0-9]{4}",
        "(?i)unavailable",
        "手机微博",
        "微博",
        "^abc\$",
        "a.*b",
        "Sending Quote: [0-9]+\\\\.[0-9]+",
        "failed to (convert|charge)",
    ]
    def likes = ["%rpc error%", "%Unavail%", "%手机%", "ab%", "%", "%x.gif%", "%code = _navailable%"]

    def runAll = { boolean useIndex ->
        sql "SET enable_inverted_index_query=${useIndex}"
        patterns.eachWithIndex { p, i ->
            order_qt_regexp_idx_${useIndex}_${i} "SELECT id FROM ${tbl} WHERE msg REGEXP '${p}'"
            order_qt_rlike_idx_${useIndex}_${i} "SELECT id FROM ${tbl} WHERE msg RLIKE '${p}'"
            order_qt_notregexp_idx_${useIndex}_${i} "SELECT id FROM ${tbl} WHERE NOT (msg REGEXP '${p}')"
            order_qt_regexp_and_idx_${useIndex}_${i} "SELECT id FROM ${tbl} WHERE msg REGEXP '${p}' AND id > 3"
        }
        likes.eachWithIndex { p, i ->
            order_qt_like_idx_${useIndex}_${i} "SELECT id FROM ${tbl} WHERE msg LIKE '${p}'"
            order_qt_notlike_idx_${useIndex}_${i} "SELECT id FROM ${tbl} WHERE msg NOT LIKE '${p}'"
        }
        order_qt_count_idx_${useIndex} "SELECT count(*) FROM ${tbl} WHERE msg REGEXP 'error'"
    }
    runAll(true)
    runAll(false)

    // 走索引与不走索引的结果必须逐条相同：再用程序化比对兜底（不依赖 .out 目测）
    sql "SET enable_inverted_index_query=true"
    def withIdx = sql "SELECT id FROM ${tbl} WHERE msg REGEXP 'code = (Unavailable|Internal)' ORDER BY id"
    sql "SET enable_inverted_index_query=false"
    def noIdx = sql "SELECT id FROM ${tbl} WHERE msg REGEXP 'code = (Unavailable|Internal)' ORDER BY id"
    assertEquals(noIdx, withIdx)

    // 删除后查询
    sql "SET enable_inverted_index_query=true"
    sql "DELETE FROM ${tbl} WHERE id = 1"
    order_qt_after_delete "SELECT id FROM ${tbl} WHERE msg REGEXP 'rpc error' ORDER BY id"

    // profile 证明索引真的参与了裁剪（RowsGramIndexFiltered > 0）
    def profileSql = "SELECT id FROM ${tbl} WHERE msg REGEXP 'context deadline exceeded'"
    profile("gram_regexp_profile") {
        run { sql "/* gram_regexp_profile */ ${profileSql}" }
        check { profileString, exception ->
            assertTrue(profileString.contains("RowsGramIndexFiltered"))
            def m = (profileString =~ /RowsGramIndexFiltered:\s*([0-9.]+)/)
            assertTrue(m.find() && Double.parseDouble(m.group(1)) > 0)
        }
    }
}
```

（`profile { run / check }` 的写法以 `test_common_grams_snii.groovy:20` 引用的 `ProfileAction` 为准；`order_qt_` 动态 tag 若不被 groovy 元编程接受，改为显式列出 tag 名。）

- [ ] **Step 2: 生成 .out 并核对**

Run: `./run-regression-test.sh --run -s test_gram_regexp_like -genOut`
Expected: 生成 `regression-test/data/inverted_index_p0/gram/test_gram_regexp_like.out`；人工核对每组 `idx_true_i` 与 `idx_false_i` 段落逐字节相同（可用 `awk` 抽段 diff）。任何不同都是 BUG（假阴性），先把该模式加进 Task 6 的模糊测试固定用例再修。

- [ ] **Step 3: 再跑一遍确认稳定通过**

Run: `./run-regression-test.sh --run -s test_gram_regexp_like`
Expected: PASS

- [ ] **Step 4: 提交**

```bash
git add regression-test/suites/inverted_index_p0/gram/test_gram_regexp_like.groovy regression-test/data/inverted_index_p0/gram/test_gram_regexp_like.out
git commit -m "test(regression): gram index REGEXP/LIKE semantics parity with and without index"
```

---

### Task 16: 验收基准 B1–B3 与文档同步

**Files:**
- Create: `tools/regex-ngram-model/e2e/bench_gram_regexp.sh`（驱动脚本）
- Modify: `be/src/storage/index/inverted/REGEX_SPARSE_GRAM_INDEX_DESIGN.md`（§2.5/§6.2 加 SNII 命名对照；§6.2.1 `lower_case` 改为 tokenizer 属性；§9.2 填入实测）

**Interfaces:**
- Consumes: 现有 E2E 驱动与语料（记忆 `project_e2e_benchmark_setup`：textbench 30M `textbench_body_30m.txt`、weibo `weibo_corpus.txt`、httplogs `documents-*.json`，stream load 直连 BE `28241` 且带 `-H "Expect:"`；重启后 `wait_loadready`）；工作负载 `tools/regex-ngram-model/q_*.txt`。
- Produces: `tools/regex-ngram-model/results/e2e_B1_textbench.md`、`e2e_B2_weibo.md`、`e2e_B3_httplogs.md`，每份含：建表 DDL、索引体积（`SHOW DATA` 的 index size / data size）、每条正则 on/off 各 5 轮取中位的耗时与结果行数、p50 加速比。

- [ ] **Step 1: 写驱动脚本**

```bash
#!/bin/bash
# tools/regex-ngram-model/e2e/bench_gram_regexp.sh <table> <query_file> <rounds>
# 对每条正则：SET enable_inverted_index_query=true/false 各跑 <rounds> 轮，记录 mysql 客户端耗时中位与行数
set -euo pipefail
TABLE=$1; QF=$2; ROUNDS=${3:-5}
MYSQL="mysql -h127.0.0.1 -P29231 -uroot --batch --raw"     # FE query 端口以部署为准（记忆 reference_doris_deploy_dirs）
median() { sort -n | awk '{a[NR]=$1} END {print (NR%2)? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }
printf "%-60s %10s %10s %8s %8s\n" regex t_on_ms t_off_ms rows speedup
while IFS= read -r re; do
  [[ -z "$re" || "$re" == \#* ]] && continue
  esc=${re//\'/\'\'}
  for mode in true false; do
    for ((r=0; r<ROUNDS; r++)); do
      s=$(date +%s%N)
      rows=$($MYSQL -e "SET enable_inverted_index_query=$mode; SELECT count(*) FROM $TABLE WHERE msg REGEXP '$esc';" | tail -1)
      e=$(date +%s%N); echo $(( (e-s)/1000000 ))
    done > /tmp/bench_$mode.txt
    eval "t_$mode=\$(median < /tmp/bench_$mode.txt)"
  done
  printf "%-60s %10s %10s %8s %8.1f\n" "$re" "$t_true" "$t_false" "$rows" "$(echo "$t_false / $t_true" | bc -l)"
done < "$QF"
```

- [ ] **Step 2: 执行 B1–B3 并记录**

Run（每张表）：建表（DDL 同 Task 15，analyzer 为 `gram_sparse`；httplogs 用 `mode=dense` + `stop_gram_df=0.25`）→ stream load 语料 → `SHOW DATA FROM <table>` 记录体积 → `bench_gram_regexp.sh <table> tools/regex-ngram-model/q_<corpus>.txt 5`。
Expected（设计文档 §9.2）：B1 可索引查询 p50 ≥ 10×；B2 p50 ≥ 10×；B3 选择率 <1% 的查询 ≥ 10×、宽泛查询不慢于无索引 ×1.05；结果行数 on/off 逐条相同。不达标的项目按 §4.6 的选择率分解定位（复验成本 vs 候选精度 vs posting I/O），写入结果文件的「偏差分析」段。

- [ ] **Step 3: 文档同步**

在设计文档追加「§2.6 master 命名对照」（V4/SPIMI → SNII、`SpimiPostingBuffer::Append` → `SpimiTermBuffer::add_token`、`SpimiQueryExecutor` → `snii::query::*` + `LogicalIndexReader::lookup`），把 §6.2.1 示例中的索引级 `"lower_case"` 改到 TOKENIZER 属性，并把 §9.2 的实测数字填入。

- [ ] **Step 4: 提交**

```bash
git add tools/regex-ngram-model/e2e/bench_gram_regexp.sh tools/regex-ngram-model/results/e2e_B1_textbench.md tools/regex-ngram-model/results/e2e_B2_weibo.md tools/regex-ngram-model/results/e2e_B3_httplogs.md be/src/storage/index/inverted/REGEX_SPARSE_GRAM_INDEX_DESIGN.md
git commit -m "docs(be): gram index P0 acceptance results B1-B3 and design doc sync"
```

---

## 自查记录（writing-plans Self-Review）

**Spec 覆盖**（设计文档 §9.1 P0 五条）：
1. GramExtractor + golden；RegexGramCompiler + 差分模糊 → Task 1–6 ✔
2. FE：tokenizer 参数校验、gram 族识别、下推标记、会话变量 → Task 13、14 ✔；「下推标记」经勘察确认 FE 无需改动（conjunct 原样下发）；「会话变量」在 P0 用 BE 配置 `enable_gram_index_regexp` 替代，FE 会话变量放 P1（§6.4.1 的 `enable_regex_gram_index` 需在设计文档标注为 P1）。
3. BE：tokenizer 接入写入器（Task 7、8）、`evaluate_inverted_index`（Task 10）、近似语义（Task 11）、profile 计数（Task 11）✔；「复验预检（memmem/hs 字面量）」按 Global Constraints 归入 P1（现有 `constant_regex_fn` 已含 memmem 快路径与 hyperscan）。
4. 回归矩阵 → Task 15 ✔（schema change / BUILD INDEX 复用 INVERTED 既有回归，未单独列）。
5. 验收 B1–B3 → Task 16 ✔。
额外：段级元数据预留 → Task 12（P1 auto 的前置）。

**占位扫描**：全文无 TBD/TODO；每个 Task 均有测试代码与实现代码；两处依赖现场核对的点已明示核对位置（`GramQuery` 序列化的 base64 头文件、`IndexExecContext` 构造签名、`checkAnalyzerName` 签名、`profile{}` 写法）。

**类型一致性**：`gram::GramScheme / GramMode / GramExtractor::extract(std::string_view, std::vector<std::string_view>*) / grams_of_literal / GramQuery::{all,none,of_gram,and_,or_,serialize,parse,to_debug_string,is_all} / RegexGramCompiler::{compile_regexp,compile_like} / resolve_gram_scheme / GramPostingSource::{df,postings} / gram_boolean_query / InvertedIndexQueryType::GRAM_BOOLEAN_QUERY / InvertedIndexResultBitmap::{set_approximate,approximate} / IndexExecContext::{set_approx_index_result_for_expr,get_approx_index_result_for_expr} / NGramTokenizerFactory::gram_scheme` 在各 Task 间名称与签名一致。

## 执行交接

计划落盘于 `be/src/storage/index/inverted/REGEX_SPARSE_GRAM_INDEX_P0_PLAN.md`。两种执行方式：

1. **Subagent-Driven（推荐）**：每个 Task 派一个全新子代理实现，Task 之间做两阶段评审（规格符合性 + 代码质量），迭代快；REQUIRED SUB-SKILL `superpowers:subagent-driven-development`。
2. **Inline Execution**：在本会话内按 `superpowers:executing-plans` 分批执行，每批后检查点评审。

建议顺序：阶段 A（Task 1–6）作为第一个独立 PR 先行；Task 13 可与阶段 A 并行；阶段 B → C → D(14) → E 串行。


