# Doris 正则索引（稀疏 gram 行级倒排）技术方案 v0.1

> 状态：设计评审稿（未实现代码）。日期：2026-09-03。分支：`worktree-regex-ngram-index-design`。
> 语言：中文。配套原型与实验数据：`tools/regex-ngram-model/`。

## 0. 摘要

**问题**：Doris 的 `REGEXP` 目前只能全表逐行跑 hyperscan/re2；现有 NGRAM_BF 索引是页级定长布隆、只服务 LIKE，且实测对未聚簇日志几乎没有裁剪能力（1024 行块级：1.0×）。

**方案**：不新增索引类型与分词器类型，复用 INVERTED 索引与现有 `ngram` 分词器（新增 auto/sparse 模式，零配置 `"parser"="ngram"`），以 **内容定义边界的稀疏 gram（CDC-gram）** 建 **行级** DOCS_ONLY 倒排（V4 SPIMI 存储、两层 posting 表示：高频 gram 走词典 + PFOR，稀有 gram 走指纹表，超高频 gram 按 FREE 阈值裁成 stop-gram），配 **Cox 式正则→gram 布尔查询编译器**；查询时索引给出候选行位图（未命中页不读），原 REGEXP 表达式保留在 common expr 阶段对候选 Block 做 **BLARE 字面量预检 + Hyperscan** 复验。非 ASCII 码点按「字」建 1-gram，ASCII 段按 CDC 字节 gram。

**结论（在 4 个真实语料、82 条正则上用原型量化）**：

| 指标 | 结果 |
|---|---|
| 体积（推荐默认 p=0.25, L=16, τ=0.10，两层） | 日志类短文本 **24.8%**；CJK 短文本 **26.2%**（τ=5% 时 15.9%）；长文本 9.3%；低熵 URL 稀疏 20.1% / 稠密 57% |
| 加速（CPU-only，基线 = Doris 现有 memmem/hyperscan 快路径） | 可索引查询 p50 **11.7×**（日志）/ **75×**（CJK）；选择率 <1% 的查询 30×–数千×；宽泛查询上界 = 1/命中率；无字面量模式 1× |
| 正确性 | 编译器只产生超集：候选 ⊇ 真值断言在全部实验通过；语义由原谓词复验兜底 |
| 稠密 3-gram 对照 | 体积 85%（日志）/109%（CJK）——不满足 20–30% 预算，故不采用为默认 |

**自适应**：没有一种 gram 方案通吃，因此写入时按段样本的三个统计量（平均长度、去重后 gram 密度、高频 gram 质量占比）自动在「稀疏 / 稠密 + 阈值 / 稠密」间选择（§6.1.7），非 ASCII 码点行内自动走 1-gram；显式属性可覆盖。

**关键取舍**：p（gram 密度）是体积–覆盖率旋钮：p 越小体积越小、可索引的短字面量越少；推荐 0.25（≈ 8 字节以上字面量可索引）。「p50 ≥ 10×」的成立条件是负载中位查询选择率 ≤ 1–2%（观测型日志检索的典型情形）；宽泛查询和无字面量模式是所有 gram 索引的共同边界，本文不替它承诺。

**cutting edge 组合**：CDC 稀疏 gram（GitHub Blackbird / ClickHouse sparseGrams 2023–2026 的局部化变体，以密度换体积）+ Cox 2012 编译器 + FREE 2002 / VLDB 2026 阈值裁剪 + BLARE SIGMOD 2023 复验 + Airphant ICDE 2022 / LogCloud VLDB 2025 的 S3 单轮取数 + Hyperscan NSDI 2019；REI SIGMOD 2026 的「索引查询」位向量作为 P2 补充。

**实施**：P0（MVP：解析器 + 编译器 + V4 接入 + 近似语义 + 复验，4–6 周）→ P1（两层存储、段级布隆、S3 批量取数，4 周）→ P2（位置局部化、负载感知）。端到端验收基准 B1–B6 见 §9.2。

## 1. 背景与目标

### 1.1 现状痛点

- `REGEXP/RLIKE` 在 Doris 里是纯表达式：扫描整列、逐行 `hs_scan`/`RE2::PartialMatch`（§2.2），数据量线性增长时延迟线性增长；云模式下还要冷读整列。
- 唯一与子串相关的索引 NGRAM_BF 只服务常量 `LIKE`、依赖默认关闭的 `enable_function_pushdown`，且是页级布隆——本次实测表明这种粒度对时间序日志没有裁剪能力（§4.3）。
- 倒排索引的 `match_regexp` 是 term 级语义并受 `max_expansions=50` 截断，不是整行 REGEXP 的替代（§2.3）。

### 1.2 目标与验收指标

1. 体积：索引占列数据的 **20–30% 以内**（上限 30%）。
2. 性能：对典型正则负载，加索引后 **p50 至少快一个数量级**（与无索引 REGEXP 相比）。
3. 技术：调研 2024–2026 的产品化实现与论文，方案在结构上处于前沿（行级、稀疏 gram、S3 友好）。
4. 语义：结果与无索引完全一致；索引只做超集过滤；任何索引侧失败只导致「不加速」。

### 1.3 范围

- 覆盖：`REGEXP/RLIKE`（RE2 语法）、`LIKE`（同一编译器）；STRING/VARCHAR/CHAR/TEXT 列；V4 存储格式。
- 不覆盖：`NOT REGEXP` 裁剪、Unicode 归一化、`match_regexp` 替换、无字面量模式加速（P2 可选）。

### 1.4 本文假设（后台作业无法与用户实时确认，评审时请重点核对）

- A1：目标负载是观测型日志/消息检索（模式含 ≥ 8 字节字面量、中位选择率 ≤ 1–2%）；若目标是低熵短串（URL/枚举）为主，默认应改为稠密模式 mode=dense（§4.8）。
- A2：以 INVERTED 索引 + 现有 `ngram` 分词器新模式的形式交付，不新增 `IndexType` 也不新增 tokenizer 类型（产品层只有一个 n-gram 概念；改动面最小、复用 V4 与 SNII 演进）。
- A3：允许 P0 先不做两层存储（体积 ≈ 33% → P1 降到 ≈ 25%）。
- A4：hyperscan 保持为复验引擎（Doris 已静态链接 5.4.2）。


## 2. Doris 现状勘察（代码事实）

> 勘察基于本仓库 `spimi-optimize` 分支工作区（目录布局为 `be/src/storage/`、`be/src/exprs/function/`）。行号仅作定位参考。

### 2.1 现有 NGRAM_BF 索引：页级定长布隆，只服务 LIKE

| 事实 | 位置 |
|---|---|
| 属性 `gram_size∈[1,255]` 默认 2，`bf_size∈[64,65535]` 默认 256 字节；仅 string 类列；一列只能有一个 BF/NGRAM_BF | `fe/.../commands/info/IndexDefinition.java:49-57,219-233`；`Index.java:275-290` |
| 每个 **data page** 收尾时 `flush()` 一枚布隆（`bf_size` 字节，固定 2 个 CityHash64 哈希） | `column_writer.cpp:831-832`；`bloom_filter/bloom_filter_index_writer.cpp:263-291`；`ngram_bloom_filter.cpp:26-68` |
| gram 按 **UTF-8 码点**滑窗切分；无大小写归一；短于 gram_size 的值不入布隆；NULL 不处理 | `itoken_extractor.cpp:26-77`；`bloom_filter_index_writer.cpp:252-261` |
| 只有函数名 `"like"` 会被转成存储层谓词 `LikeColumnPredicate`，且要求常量模式；REGEXP 完全不走它 | `vexpr.h:284 is_like_expr()`；`olap_scan_operator.cpp:500-503`；`tablet_reader.cpp:450-476` |
| FE 会话变量 `enable_function_pushdown` **默认 false**，不开则 LIKE 也不会下推、NGRAM_BF 形同虚设 | `SessionVariable.java:2100-2101` |
| 页裁剪：按 ordinal index 枚举页、逐页读布隆、命中页加入 `RowRanges` | `column_reader.cpp:569-603`；`segment_iterator.cpp:1182-1200` |
| `LikeColumnPredicate::evaluate_and(bf)` 未检查 `_opposite`（NOT LIKE 语义隐患） | `predicate/like_column_predicate.h:85-92` |
| 无按索引类型的体积统计，只有 `finalize_columns_index` 的总 `index_size` | `segment_writer.cpp:944-952` |

**结论**：NGRAM_BF 是「页级 + 定长布隆 + 仅 LIKE」的三重受限设计；第 4 章的实测表明页级粒度对未聚簇日志几乎没有裁剪能力，因此本方案不在它上面缝补，而是新建行级索引。

### 2.2 REGEXP 的执行路径：逐行、Block 级 common expr

- `regexp`/`rlike` 与 `like` 同在 `be/src/exprs/function/like.cpp`：常量模式在 `open` 时分类——`ALLPASS/EQUALS/STARTS_WITH/ENDS_WITH/SUBSTRING` 走 memmem 类快路径（`like.cpp:40-53,999-1072`），否则 `hs_compile(HS_FLAG_DOTALL|ALLOWEMPTY|UTF8, HS_MODE_BLOCK)`（`:490-513`），失败回退 RE2，再失败在 `enable_extended_regex` 下回退 boost。
- 执行是**逐行** `hs_scan` / `RE2::PartialMatch`（`:418-447`）；非常量模式每个 Block 重新 `hs_compile`（`:449-487`）。
- REGEXP 不会成为 `ColumnPredicate`，只能作为 `_common_expr_ctxs_push_down` 在读出谓词列之后于 Block 上执行（`scan_operator.cpp:358`；`segment_iterator.cpp:3214-3228 _execute_common_expr`）。
- 第三方：re2 `2021-02-02`，hyperscan `5.4.2`（aarch64 用 vectorscan `5.4.11`），静态链接（`thirdparty/vars.sh:157-175`；`be/cmake/thirdparty.cmake:58`）。

**结论**：无索引 REGEXP 的成本 = 全列读取/解码 + 逐行正则；对纯字面量模式 Doris 已有 memmem 快路径，这是评估「10 倍」时必须采用的诚实基线（第 4 章按此口径给数）。

### 2.3 倒排索引的 MATCH_REGEXP：term 级语义，不是整行 REGEXP

- `match_regexp` 把模式整串当 term，遍历词典对每个 term 跑 hyperscan，命中 term 求并集；只有 `^` 开头的模式才用 `RE2::PossibleMatchRange` 取前缀缩小枚举范围；受 `inverted_index_max_expansions`（默认 50）静默截断（`inverted/query/regexp_query.cpp:28-176`）。
- SPIMI V4 路径同样是全词典枚举 + 逐 term RE2（`spimi/spimi_query_executor.cpp:368-420`）。

**结论**：语义与整行 REGEXP 不等价（token 级、截断），不能作为 REGEXP 的实现；但其 hs 编译 flag 与前缀提取可复用。

### 2.4 行位图汇合点：已有「索引给候选、谓词再复验」范式

- `SegmentIterator::_row_bitmap`（roaring）是唯一汇合点：`_apply_inverted_index` 对 `_col_predicates` 调 `pred->evaluate(name_type, IndexIterator*, num_rows, &_row_bitmap)`；**`need_remaining_after_evaluate`**（`segment_iterator.cpp:1455-1461`）让全文索引上的 EQ/IN 走「索引产出候选、谓词留在后续阶段复验」。
- 表达式级路径：`_apply_index_expr` → `VExprContext::evaluate_inverted_index` → `IFunction::evaluate_inverted_index`，结果存 `IndexQueryContext`，`_row_bitmap &= result` 后把表达式从 `_common_expr_ctxs_push_down` **移除**（`segment_iterator.cpp:1328`；`vexpr.cpp:842-957`）——即当前表达式路径假设索引结果精确。
- 位图之后 `BitmapRangeIterator` 只读位图内 rowid，未命中的页不解码（`segment_iterator.cpp:2375-2378`）；残余谓词按 向量化谓词 → 短路谓词 → common expr 三段在 Block 上执行（`:2984-3060`）。

**结论**：新索引只需（a）产出行级候选位图并 `&=` 进 `_row_bitmap`，（b）把 REGEXP 表达式**保留**在 common expr 阶段做复验，即可自然得到「先索引过滤、再按 Block 跑正则」的执行形态，且页级 I/O 裁剪是免费的。

### 2.5 SPIMI V4 写读组件：可直接承载 gram 倒排

- 写入 API 是 term 级 `SpimiPostingBuffer::Append(term, doc_id, position)`（`spimi/posting_buffer.h:383`），不必经过分词器，可直接喂「一行的 gram 集合」；compact 模式同 doc 重复 term 不写任何字节（天然去重）。
- DOCS_ONLY（`omit_term_freq_and_positions`）：`.prx` 为空，`.frq` 只存 doc-delta；df<512 走纯 VInt，df≥512 走自适应窗口（256/512/1024/2048 doc）+ 128 值 PFOR 块（含 patch 与常数块），稠密 gram 可低至 0.125 bit/posting（`spimi/freq_prox_encoder.cpp:306-314,546-559`；`window_frame_encoder.cpp:36,612-630`；`pfor_encoder.h:50-110`）。
- 词典：前缀编码，每 term 固定开销约 5–8 B + 后缀；≤256 B 的小 term posting 内联进 `.tis`；`.tii` 每 128 条采样常驻（`term_dict_writer.cpp:142-178`；`segment_writer.h:52-84`）。
- **限制 1**：term 经 `Utf8ToWide` 转宽字符再按 CLucene modified-UTF-8 写出，非法 UTF-8 折叠为 U+FFFD，NUL 在读侧会截断（`term_dict_writer.cpp:86`；`byte_output.cpp:185-195`；`query_term_docs.cpp:85-94`）→ gram 必须按码点切、且是合法 UTF-8。
- **限制 2**：读侧 `SpimiFulltextIndexReader` 把 `.tis/.tii/.frq/...` 整流读入内存（`spimi_fulltext_index_reader.cpp:53-57,99-101`），`PostingStore` 的定位读接缝存在但生产只接 `MemPostingStore` → S3 上是整段 GET。SNII/BASALT 设计正在解决这一点，本方案与之对齐而不重复造轮子。
- 新索引类型先例：`IndexType{BITMAP,INVERTED,BLOOMFILTER,NGRAM_BF,ANN}`（`olap_file.proto:437-442`），ANN 作为独立 `IndexColumnWriter/Reader` 放进同一 `.idx` 容器（`index_storage_format_v2.cpp:83-225`）。
- 已有 `NGramTokenizer`/`EdgeNGramTokenizer`（按 Unicode 码点，工厂名 `ngram`/`edge_ngram`），但只能经自定义 analyzer 使用，不是内置 parser（`tokenizer/ngram/ngram_tokenizer.h:29-55`；`analysis_factory_mgr.cpp:60-62`）。
- 段/页参数：page 以编码后 64 KB 为界（`options.h:28`，表属性 4 KB–10 MB 可调）；segment ≈ 230 MB 或 `max_rows_per_segment`；V4 的 index compaction 走「从列数据重建」门控（`compaction.cpp:1224-1234`）。

**结论**：走「INVERTED 索引 + 新内置 gram parser + V4 DOCS_ONLY」路线，写入/溢写/归并/内联/PFOR/compaction 全部复用，改动面集中在 **gram 切分器、正则编译器、查询执行与近似语义、S3 布局** 四处。


## 3. 业界产品与学术前沿调研

> 本章由两路调研代理（产品线 / 论文线）在 2026-09 用一手文档、源码、官方博客与论文原文交叉核对得出；每条结论附来源，未核实处注明。完整报告见附录 C。

### 3.1 产品化实现

| 系统 | 索引结构 | gram / 粒度 | 正则支持方式 | 体积 / 关键数字 | 来源 |
|---|---|---|---|---|---|
| **ClickHouse** `ngrambf_v1`/`tokenbf_v1` | 每 granule 一枚布隆（跳数索引） | n 可配；8192 行 | PR #57882（2024-01）：`match()` 经 `OptimizedRegularExpression::analyze` 抽 `required_substring`（≥3 字节）+ 顶层 `\|` 各分支 `alternatives`；`. ^ $ +` 终止字面串，`* ? {` 再删前一字符；字符类/`(?i)` 视为不可提取 | 每块固定 256/512 B，全 part 仅 KB–MB；只能整块跳过 | [CH-1][CH-4][CH-7] |
| **ClickHouse** `text` 索引（25.9 exp → 26.2 GA） | 每 part 一份：front-coding 分块词典 `.dct` + 稀疏头 `.idx` + posting `.pst`（≤6 rowid 内嵌 / 7–12 VarInt / >12 roaring） | 分词器含 `ngrams(N)`（1..8）、`sparseGrams(min=3,max=100[,min_cutoff])`；行级 | `=`/IN/LIKE/`match`/startsWith 为 **hint 模式**（跳 granule + 行级重验）；`match` 条件为 `req AND (alt1 OR alt2)`；26.4 新增 `%foo%` 走有序词典扫描 direct read（needle ≥4） | GitHub 事件集 text 215.84 GiB vs bloom 7.04 GiB，写入吞吐 −50%，冷查询 7–10×；路线图明列「faster LIKE / regex」 | [CH-6][CH-11][CH-12][CH-15] |
| **ClickHouse** `sparseGrams` | — | 取「长度 ≥n 且两端 (n−1)-gram 的 CRC32 严格大于内部任一 (n−1)-gram」的全部子串，`sparseGrams('alice',3)` → ali, lic, ice, lice | 与 GitHub sparse grams 同源 | 25.5 函数 → 26.x `sparse_grams` 索引（PR #79985） | [CH-3][CH-13] |
| **Snowflake** Search Optimization（SUBSTRING 方法） | 每 micro-partition 的 search access path（专利 US10997179B1：N-gram + blocked bloom） | 分区级剪枝 | LIKE/ILIKE/CONTAINS/RLIKE/REGEXP/REGEXP_LIKE；正则需 **≥5 字符且必出现**的字面量，alternation 任一分支 <5 或 `(x)?` 可选字面量均不加速 | 存储「约为原表 1/4」，极端可达表大小；build 与维护计费 | [SF-1][SF-3][SF-5] |
| **Elasticsearch** `wildcard` 字段（7.9） | 3-gram 倒排 + binary doc value 存原值 | 3；文档级；首尾 NUL 填充 | automaton → 近似查询（`*shell*` → `she AND ell`），`MAX_CLAUSES_IN_APPROXIMATION_QUERY=10`，候选取原值跑 automaton 复核 | 200 万行 weblog：keyword 310 MB vs wildcard 227 MB（−27%）；7.10 起 lowercase + 标点折叠、`case_insensitive` | [ES-1][ES-2][ES-3] |
| **Google Code Search**（Cox 2012） | trigram → 文件倒排 | 3；文件级 | 五元组 (emptyable, exact, prefix, suffix, match) 自底向上推导，`maxExact=7`、`maxSet=20` | Linux 源码 420 MB → 索引 77 MB（≈20%）；"hello world" 候选 36,972 → 25 | [GCS-1][GCS-2] |
| **Sourcegraph Zoekt** | trigram → (文件, offset) 倒排（带位置） | 3 | Literal≥3 → substring，Concat→AND，Alternate→OR，`.*`/字符类 → bruteForce；用两个最低频 trigram 按距离校验 | 索引 ≈ 语料 3×（2× offset + 1× 原文） | [ZK-4][ZK-6] |
| **GitHub Blackbird**（2023） | sparse grams 倒排（Rust） | 变长（≥3）；文档级 | 给相邻字符对赋权，切出「内部权重严格小于边界」的区间为 gram；`/arguments?/` → arg∧rgu∧gum∧((ume∧ment)∨uments)；regex → 子串查询 → posting 交集 → 逐文档验证 | 115 TB 内容 → 索引+压缩内容 25 TB；摄入 12 万文档/s；2026 起索引前全量 case-fold | [GH-7][GH-8][GH-10] |
| **Cursor**（2026-03） | trigram + 8 位位置掩码 + 后继字符掩码（"3.5-gram"）+ sparse n-gram | 3 | 字符类展开为分支、交替取并、宽字符类处断开 | — | [CU-11] |
| **PostgreSQL** `pg_trgm` | GIN 倒排 / GiST 签名（siglen 12 B） | 3；行级；词前补 2 空格后补 1 | `~`/`~*`：regex → NFA（color）→ trigram 图 → 查询时 BFS 判终态可达；`MAX_EXPANDED_STATES=128`、`MAX_TRGM_COUNT=256`；无 trigram → 全索引扫描 | 无官方体积数字 | [PG-1][PG-4] |
| **Grafana Loki** 3.0 blooms | 每 stream/chunk 的 n-gram 布隆 | `bloom_ngram_length=4`、`skip=1` | 仅 `\|=` 与可化简为字面量集合的 `\|~`（`(error\|warn)` 可，`f.*oo` 不可） | ≈ chunk 数据 3%；3.3 起改为只索引 structured metadata（<1%） | [LK-1][LK-3] |
| **VictoriaLogs** | 每 (block, column) 词级布隆 | 词 | `~"re"` 先 `GetLiterals()` 抽必需字面量 → 查布隆 → 块内逐行正则 | ≈2 B/唯一词 | [VL-5][VL-6] |
| **Milvus 2.6** NGRAM（2025-12） | 变长 gram 倒排 + 验证 | `min_gram`/`max_gram` | LIKE 前/后/中缀及可抽字面量的正则（RE2 复核） | 「扩 1 字符范围索引约翻倍」 | [MV-8][MV-9] |
| **StarRocks** NGRAMBF（3.3） / **Databend** NGRAM（2025-05） / **TiDB Cloud Lake** | 块级布隆 | gram_num 默认 2 / 3 / — | 仅 LIKE（无 regexp） | Databend 每列每块默认 1 MiB | [SR-1][DB-6][TI-7] |
| **Doris** NGRAM_BF | 页级定长布隆 | 默认 2；`bf_size` 256 B | 仅 LIKE | — | 见 §2.1 |

**正则引擎侧的字面量提取 API**（可作编译器实现基础）：Hyperscan `HS_FLAG_PREFILTER` 与 NSDI'19 的图分解 + FDR/Teddy 多串匹配；rust `regex-syntax` `hir::literal::Extractor`（prefix/suffix `Seq`，`limit_class=10`、`limit_total=250`，`optimize_for_prefix_by_preference` 剔除有毒短串）；RE2 `Prefilter`/`FilteredRE2(min_atom_len=3)`（ALL/NONE/ATOM/AND/OR，exact 集叉积 >16 截为 AND）。[HS-1][HS-2][RS-4][RE2-8][RE2-9]

### 3.2 学术论文（2002–2026）

| 论文 | 会议 | 核心思想 | 关键数字 | 对本方案的适用性 |
|---|---|---|---|---|
| Cho & Rajagopalan, *A Fast Regular Expression Indexing Engine*（FREE） | ICDE 2002 | 只索引 **useful** 多元 gram：选择率低于阈值 c 才有用；useful 单调故只保留前缀最小者；查询编译 连接→AND、交替→OR、闭包→ANY | 摘要称对 grep 类工具「数量级提升」（原文不可访问，倍数未核实） | ★★★★ 高频 gram 阈值裁剪的祖型 |
| Russ Cox, *Regular Expression Matching with a Trigram Index* | 2012 技术文章 | 五元组推导 + 信息保留化简 | 索引 ≈ 源码 20%；候选 36,972 → 25 | ★★★★★ 编译器直接照搬 |
| Hore 等, *Indexing text data under space constraints*（BEST） | CIKM 2004 | 负载感知：cover(g)=(q,d) 对，benefit/cost 贪心，预算最大覆盖 | 后续评测：精度最高但构建最慢 | ★★★ 需完整负载，不适合写入时 |
| Zhang, Deep, Floratou, Gruenheid, Patel, Zhu, *Exploiting Structure in Regular Expression Queries*（BLARE） | SIGMOD 2023 | 不建索引：正则拆成 prefix·S·suffix，先 `string::find` 字面量，命中行才跑正则；多臂老虎机在线选模式 | 比 RE2 快 1.6–3.7×，Boost 3.4–7.9×，PCRE2 3.1–>100×；MAB 开销 <7% | ★★★★★ 复验阶段零成本采用 |
| Zhang, Deep, Patel, Sankaralingam, *An Evaluation of N-Gram Selection Strategies for Regular Expression Indexing…* | VLDB 2026（arXiv 2504.12251） | 统一框架评测 FREE / BEST / LPMS / VGGraph / 全 trigram，5 个负载 | 构建：FREE 30–1,324 s、LPMS 79–4,569 s、BEST 1,781–28,912 s；FREE 相对 BEST 构建 −92%、查询仅 +1.2%；结论「无通用最优」 | ★★★★★ 写入时无负载 → 阈值裁剪或全 gram；有负载回放 → LPMS |
| Zhang 等, *Regular Expression Indexing for Log Analysis*（REI） | SIGMOD 2026（arXiv 2510.10348） | 「索引查询而非数据」：从查询集字面量按频率选 k 个 bigram，每行（或每 m 行）存 k 位位向量；正则编译为位掩码线性扫描 | 相对 BLARE 最高 14×，额外空间 2.1%；DB-X k=64 索引 0.8 GB、过滤后仅 0.63% 行跑正则；同等 bigram 数倒排慢 38–62× | ★★★★ 日志负载的轻量补充层（P2） |
| Qiu 等, *Efficient Index-Based Regex Matching with Optimal Query Plan Tree* | DASFAA 2023 | 带位置 q-gram 倒排 + 查询计划树（交/并顺序）最小化期望代价，NP-hard | — | ★★★ 位置倒排可用相对位置剪枝 |
| Qiu 等, *…Variable-length-gram Inverted Index*（VGGraph） | DASFAA 2024 | 按频率阈值递归扩展高频短 gram，VGgraph 表示可用 gram，贪心集合覆盖 | 复现：构建最快（4–45 s）、低空间预算下精度最高 | ★★★★ 与 sparse grams 殊途同归 |
| Chockchowwat 等, *Airphant: Cloud-oriented Document Indexing* | ICDE 2022 | Lucene 直放对象存储多级往返 >6 s；改用 IoU Sketch + 哈希表 + Super Postings List，一轮并行请求 | 比 Lucene 快至 8.97×，比 ES 至 113×，延迟 13–300 ms | ★★★★ S3 上 gram 查找 = 一批已知 key 的并行查找 |
| Wang 等, *LogCloud: Fast Search of Compressed Logs on Object Storage* | VLDB 2025 | CLP 拆模板/变量后只索引变量词典；面向 S3 的分块 FM-index，≤1 MB byte-range GET 皆延迟受限 | TB 级压缩日志单机 <10 s；存储 <10% | ★★★★ S3 请求次数比字节更关键 |
| Wang 等, *Hyperscan* | NSDI 2019 | 图分解把正则拆为关键字符串 + 小 FA；SIMD 多串匹配 | 比 PCRE 快 24.8–40.1×；Talos 1,300 正则 PCRE 6,942 s / RE2 1,777 s / Hyperscan 多模式 2.15 s | ★★★★★ 复验器与兜底扫描器 |
| Sitaridi 等, *SIMD-Accelerated Regular Expression Matching* | DaMoN 2016 | 列存 REGEXP：DFA 转移表整型化，gather 让多行并行走 DFA，lane 非锁步替换 | 主流 CPU 2×，Xeon Phi 5× | ★★★ 向量化复验的实现范式 |
| Varatalu 等, *RE#*；Moseley 等 .NET NonBacktracking | POPL 2025 / PLDI 2023 | 符号导数、无回溯、惰性 DFA | 比 Rust 最快引擎快 >71% | ★★★ 引擎选型参考 |
| Jokinen & Ukkonen, q-gram 引理 | TCS 1992 | 精确子串须命中全部 m−q+1 个 q-gram；q 越大单 gram 越选择、可用 gram 越少 | — | 2/3/4-gram 与最短字面量权衡的理论根源 |
| Rodrigues 等 CLP（OSDI 2021）、LogGrep（EuroSys 2023）、μSlope（OSDI 2024） | — | 模板/变量拆分、压缩块上直接过滤 | CLP 压缩比 ≈ gzip 2×、搜索快 8× | ★★★ 词典级过滤 + 编码流验证 |

### 3.3 技术谱系与对 Doris 的启示

**谱系**（按四个维度）：
- 索引结构：固定 gram 倒排（FREE、Cox、pg_trgm、ES）→ 带位置 gram 倒排（Zoekt、Cursor、DASFAA'23）→ 变长/稀疏 gram 倒排（Blackbird、ClickHouse sparseGrams、VGGraph）→ 每块布隆/位向量（ClickHouse ngrambf、Loki、Snowflake、REI）→ 后缀结构（Baeza-Yates'96、LogCloud FM-index）。
- gram 选择：数据阈值（FREE）/ 负载覆盖（BEST、LPMS、REI）/ 内容自适应变长（sparse grams）/ 全 trigram（Postgres、GitLab、Zoekt）。VLDB'26 评测：无通用最优，轻量启发式构建省 90%+ 而查询仅差 ~1%。
- 查询编译：Cox 五元组、FREE AND/OR/ANY、BLARE prefix·S·suffix、ClickHouse required+alternatives、pg_trgm NFA 图。共同点：**无字面量时显式退化为全扫描**。
- 验证引擎：先字面量后自动机（Hyperscan、Teddy、BLARE），按列批量（DaMoN'16）。

**对 Doris 的 12 条启示（已在第 6 章逐条落地）**：
1. 必须有一层「正则 → gram 布尔查询」编译器，规则显式：alternation 每分支都须有 ≥n 字面量否则整体放弃；`NOT` 永不裁剪；`(?i)`、字符类、`(x)?` 保守处理（Cox、ClickHouse、Snowflake、Loki 一致）。
2. **行级候选 + 原值复验**（ES wildcard、Blackbird、Milvus、ClickHouse text hint 模式）优于页级跳过；Doris 已有 `need_remaining_after_evaluate` 范式。
3. 固定 3-gram 对 "for"、"ERR"、"/images/" 类高频 gram 选择性不足，**变长/稀疏 gram** 是产品（GitHub、ClickHouse、Cursor）与学术（VGGraph）的共同答案。
4. 高频 gram 用 **FREE 阈值裁剪**（写入时无负载），可选 **REI/LPMS 负载回放**（P2）。
5. 复验阶段用 **BLARE 字面量预检 + Hyperscan**，纯字面量正则的基线本来就是 memmem，必须诚实对比。
6. ngram 倒排普遍大于词级（ClickHouse 明示 216 GiB vs 7 GiB），需要 **三档 posting 表示 + front-coding 词典 + 小 term 内联**——与 SPIMI 路线图同一结论。
7. 大小写：索引期 case-fold（GitHub 2026、ES 7.10）而非查询侧枚举 ≤8 变体（Cox/Zoekt）。
8. 短模式兜底：ClickHouse 26.4 用有序词典扫描（needle ≥4）；SPIMI 有序 term dict 可提供同样能力。
9. S3：以**请求轮次**为优化目标（Airphant 6 s → 13–300 ms；LogCloud ≤1 MB GET 延迟受限）；gram 查询天然是一批已知 key 的并行查找，布局应保证 1–2 个 RTT。
10. 位置信息可先剪候选、再把验证局部化到窗口（Zoekt、Cursor、DASFAA'23）；对长文本列尤为关键。
11. 体积口径要诚实：Snowflake ≈ 1/4 表大小、ES 可低于 keyword、Cox ≈ 20%、Zoekt 3×——取决于粒度与 gram 策略，20–30% 的预算只有「稀疏 gram + 行级 + 好的 posting 编码」这一条路能同时满足体积与精度（第 4 章实测）。
12. 建索引即生效：`enable_function_pushdown` 默认 false 的门控不应延续到新索引。


## 4. 真实语料上的量化建模（方案的决策依据）

### 4.1 方法与工具

为了不凭感觉定粒度和 gram 策略，本次写了一个一次性原型 `tools/regex-ngram-model/ngram_model.cpp`（约 1,300 行 C++，链接仓库第三方库 CRoaring / re2 2021-02-02 / hyperscan 5.4.2），它做四件事：

1. 读语料（每行一条记录，可从 JSON 行抽取字段），按 **稠密 n-gram**（字节或码点）或 **CDC 稀疏 gram**（§6.1.2 同一规则）切分，建 **行级** 倒排（行内去重），派生 **块级** 倒排与 **块级布隆**（B = 4…4096 行，bpp = 8/10/12）。
2. 用三种口径估算 posting 体积：Roaring（`runOptimize` 后 portable 大小）、**FOR-128**（每 128 个 delta 按最大位宽打包 + 1 字节头，近似 V4 PFOR 无 patch 的下界偏保守）、VByte；词典按「gram 字节 + 6」估算；另给「两层」口径（df ≤ 2 的 gram 只存 4 B 指纹 + 3 B/docid）。
3. 把正则用 Cox 算法编译成 gram 布尔查询（§6.3），在行级 / 块级 / 布隆上求候选，再用 re2 复验；**断言候选 ⊇ 真值**（82 条正则 × 4 语料 × 全部配置均通过）。
4. 计时：无索引基线 = 逐行 `RE2::PartialMatch`、逐行 `hs_scan`、纯字面量的逐行 `memmem`（Doris 现有快路径，§2.2）；索引路径 = 位图运算 + 对候选行的复验（字面量用 memmem，否则 hs）。**所有数字都是单线程 CPU 时间，不含列读取/解压/S3**，因此对索引路径是偏保守的口径。

复现：`tools/regex-ngram-model/build.sh && ./run2.sh`；原始输出在 `tools/regex-ngram-model/results/`。

### 4.2 数据集与工作负载

| 语料 | 行数 / 字节 | 平均行长 | 形态 | 正则条数 |
|---|---|---|---|---|
| textbench（ClickHouse TextBench OTel 日志 `Body`） | 999,872 / 81.4 MB | 81 B | 混合 JSON / gRPC 报错 / 堆栈 / 短消息 | 29 |
| httplogs `request` 字段 | 3,000,000 / 118 MB | 40 B | 低熵 URL 路径 | 15 |
| weibo | 500,000 / 67.9 MB | 136 B | 短 CJK 消息 | 19 |
| wikipedia（前 8,000 篇） | 8,000 / 408 MB | 51 KB | 超长英文文档 | 19 |

正则刻意混合三类：选择率 <1% 的「针」（`user_id="[0-9a-f]{8}-`、`context deadline exceeded`）、1–5% 的常见错误（`rpc error: code = Unavailable`）、以及宽泛或无字面量的模式（`cart\.cartstore\.ValkeyCartStore` 14%、`[0-9]{3}-[0-9]{4}`、`(?i)unavailable`、`GET|POST`）。工作负载文件：`q_textbench.txt` 等。

### 4.3 结论 1：页/块级布隆对未聚簇日志几乎无裁剪能力

textbench，稠密 3-gram，无索引基线 = re2：

| 粒度 | 倒排 FOR 体积 | 布隆(10 bit/gram) 体积 | 中位加速 |
|---|---|---|---|
| 行级 | 85.2% | — | 11.2× |
| B=16 行 | 31.4% | 49.1%（8 bit） | 3.3× |
| B=64 行 | 14.7% | 35.4% | 1.6× |
| B=256 行 | 6.4% | 14.5%（8 bit） | 1.0× |
| B=1024 行（≈ 一个 64 KB 页） | 2.4% | 8.6% | **1.0×** |
| B=4096 行 | 0.65% | 2.5%（8 bit） | 1.0× |

原因：时间序日志里每 1024 行几乎包含所有常见 gram（每块 distinct gram 5,728 ≈ 全局 15,764 的 36%），任何由常见 gram 组成的查询在每个块都命中。**现有 NGRAM_BF 的页级布隆结构对 REGEXP 场景没有出路**；只有当数据按内容聚簇时才有效（§4.7 的排序实验：B=1024 布隆 1.3% 体积即得 38×）。

### 4.4 结论 2：行级稠密 3-gram 倒排在短行日志上体积 57–103%

| 语料 | posting/字节 | bits/posting（FOR） | 行级体积（FOR + 词典） |
|---|---|---|---|
| textbench | 0.833 | 8.2 | 85.3% |
| httplogs | 0.941 | 4.9 | 57.2% |
| weibo（字节 3-gram） | 0.839 | 9.8 | 109.1% |
| weibo（码点 2-gram） | 0.358 | 13.0 | 80.0% |
| wikipedia | 0.129 | 5.2 | 9.6% |

信息论上行级 gram 倒排 ≈ (posting/字节) × (log₂(N/df)+常数) bit；短行文本每字节接近 1 个 posting、每 posting 8 bit，天然逼近 100%。textbench 的 posting 质量分布（FOR）：df ≤0.1% 的 gram 占 3.4% 质量但 4.9% 字节；df 1–10% 的 1,033 个 gram 占 55% 质量、46% 字节——**体积主要花在中频 gram 上，而这些 gram 恰恰是有裁剪价值的**，单靠 FREE 阈值裁掉高频 gram 只能省到 48%（τ=5%）。

### 4.5 结论 3：CDC 稀疏 gram 把 posting 数降 4×，两层存储后进入 20–30% 预算

textbench，行级，REAL 口径（memmem/hs 基线 + 快复验）：

| 配置 | posting/字节 | distinct gram | FOR 体积 + 词典 | 两层口径 | 可索引查询 | 中位加速（全部 / 仅可索引） |
|---|---|---|---|---|---|---|
| 稠密 3-gram | 0.833 | 15,764 | 85.2% + 0.2% | 85.3% | 28/29 | 8.6× / 9.0× |
| 稠密 3-gram + casefold（即 lower_case=true）| 0.823 | 14,612 | 83.6% + 0.2% | 83.8% | 28/29 | 8.8× / 9.0× |
| CDC p=0.20, L=24 | 0.150 | 467,854 | 19.0% + 9.6% | 22.1% | 22/29 | 5.8× / 8.9× |
| CDC p=0.25, L=24 | 0.199 | 471,865 | 24.1% + 8.9% | **27.1%** | 23/29 | 6.1× / 9.3× |
| CDC p=0.25, L=24, casefold（即 lower_case=true）| 0.199 | 471,834 | 24.0% + 8.9% | 27.1% | 23/29 | 6.9× / 7.1× |
| **CDC p=0.25, L=16, τ=0.10（推荐默认）** | 0.199 | 460,388 | 21.8% + 8.5% | **24.8%** | 21/29 | 7.1× / **11.7×** |
| CDC p=0.33, L=24 | 0.281 | 444,407 | 32.8% + 7.6% | 35.7% | 25/29 | 7.0× / 10.6× |

另测「稠密 3-gram + 行组粒度」这一替代路线（把 B 行合为一个 posting 单元）：B=4 → 57.1%、B=8 → 43.2%（τ=5% 时 36.6% / 30.3%），re2 基线中位加速 6.8× / 4.8×（τ=5% 时 7.2× / 5.5×）。与 CDC p=0.25 行级（27.1%，同口径 6.4×）相比体积更大、加速更低，且候选单位是 4–8 行而非 1 行（多读多验），故不采用。

要点：
- 稀疏 gram 的 distinct 数暴涨（47 万，其中 95% 的 gram df ≤ 2），单层词典成本 9%；「df ≤ 2 只存指纹」后降到 1.5% + 3.6%，这就是 §6.2.2 两层结构的来由。
- p 是覆盖率旋钮：p 越小体积越小、不可索引的短字面量越多（`GET|POST`、`10\.68\.` 等）；推荐 p=0.25。
- casefold（属性 `lower_case=true`）让 `(?i)unavailable` 从全扫变为可索引（体积不变）。
- 在 httplogs（低熵 URL）上 CDC p=0.25 体积 20.1% 但只有 12/15 可索引、中位 1.4×（该负载本身宽泛：7/15 的查询命中 ≥3%）；把密度提到 p=0.40 也只是 29.4% / 可索引中位 6.5×。稠密 3-gram 57% 得 4.2×；**稠密 + τ=0.25 裁剪为 41.1%、可索引中位 20.5×**（12/15 可索引，退回全扫的 3 条本来就是命中 44–85% 的宽泛查询）。低熵短串列的正确答案是稠密模式（mode=dense）+ τ，而不是稀疏 gram——这正是 §6.1.7 自适应选择要解决的问题。

### 4.6 结论 4：加速比由选择率决定，行级候选几乎就是真值

textbench，稠密 3-gram 行级，REAL 口径（候选数 ≈ 真值数，说明 gram 交集的假阳性极少）：

| 命中率区间 | 例子 | 加速 |
|---|---|---|
| 0（无匹配） | `error.*timeout`、`user_id="[0-9a-f]{8}-`、`product_id=[A-Z0-9]{10}` | 810× / 2464× / 3849×（只付 posting 读取） |
| <0.5% | `[a-z]+@[a-z]+\.com`（564 行）、`context deadline exceeded`（12 行）、`transport: Error while dialing`（0.2%）、`trace\.rb:[0-9]+:in`（0.4%） | 231× / 120× / 51× / 31× |
| 1–3% | `\[PlaceOrder\]`、`opentelemetry-api-1\.[0-9]+`、`Sending Quote: …`、`rpc error: code = Unavailable`、`GET\|POST` | 10.7× / 17.2× / 8.6× / 5.1× / 6.9× |
| 5–15% | `quantity=[0-9]+`（4.9%，候选 9.7%）、`failed to (convert\|charge)`（11%）、`cart\.cartstore\.ValkeyCartStore`（14%） | 1.7× / 1.3× / 2.4× |
| 无字面量 | `[0-9]{3}-[0-9]{4}` | 1.0×（全扫） |

分位数（29 条）：稠密 3-gram p25 / p50 / p75 = 4.4× / 8.6× / 31×；CDC p=0.25 为 1.8× / 6.1× / 12.7×（仅可索引 23 条的 p50 = 9.3×，L=16+τ=0.1 时 11.7×）。

因此「p50 ≥ 10×」成立的条件是：**工作负载中位查询的选择率 ≤ 1–2%**（观测型日志检索的典型情形），且模式含 ≥ 8 字节左右的字面量。宽泛查询（命中 ≥5%）的加速上界是 1/命中率，与索引实现无关；无字面量模式任何 gram 索引都无能为力。上述均为 CPU-only 口径；端到端时无索引路径还要读整列（本机页缓存下约等于再加一倍时间，S3 冷读则是数量级差距），索引路径只读候选页，实际比值会更高——这留给 §9 的端到端验收基准来证明，而不是在这里替它承诺。

### 4.7 结论 5：CJK、数据聚簇与长文本

- **CJK（weibo）**：字节 3-gram 体积 109% 不可接受；码点 2-gram 80%；**码点 1-gram（每个字一个 term，11,820 个 distinct）两层口径仅 26.2%，19/19 可索引，中位加速 75×**；再加 τ=5% 裁剪为 15.9%（15/19 可索引，仅可索引中位 48×）。结论：非 ASCII 码点按「字」建索引，ASCII 段按 CDC 字节 gram——即 §6.1 的**按脚本自适应**方案。
- **数据聚簇**：把 textbench 按内容排序后再建索引，稠密 3-gram 行级体积 85% → 29.6%（Roaring 25.4%）、中位加速 28.8×；CDC p=0.25 两层口径 13.3%、23.8×；此时 B=1024 布隆也有 38.6×。说明行级 posting 编码会自动吃到聚簇红利，不需要单独的块级层；对按服务/模板聚簇存储的日志表，体积会显著低于本文数字。
- **长文本（wikipedia，51 KB/篇）**：稠密 3-gram 仅 9.3%（两层）但中位加速 2.3×——瓶颈是对整篇文档跑正则的复验成本，且常用英文短语命中率高；CDC 反而因 790 万 distinct gram 使词典膨胀到 32%。长文本列应用稠密模式（mode=dense）并在 P2 引入位置流把复验局部化（§6.4.5）。

### 4.8 推荐配置矩阵

| 列特征 | 推荐 | 预期体积 | 预期效果 |
|---|---|---|---|
| 日志/消息类短文本（≤ 数百字节，ASCII 为主） | `ngram` mode=sparse, p=0.25, n=3, L=16, τ=0.10 | 22–28% | 选择率 ≤1% 查询 ≥ 30×，1–3% 查询 5–20× |
| 不确定 / 混合 | `"parser"="ngram"`（auto，§6.1.7：按段样本统计自动选上述之一） | 随段而定 | 与人工选择一致 |
| CJK 为主短文本 | 自动：非 ASCII 码点 1-gram + ASCII 段 CDC | 16–26% | 中位 48–75× |
| 低熵短串（URL、路径、枚举型） | `ngram` mode=dense, n=3, τ=0.25（预算优先时 mode=sparse p=0.40） | 41%（稀疏 29%） | 可索引查询中位 20.5×（稀疏 6.5×），宽泛查询 1–2× |
| 长文本（≥ 4 KB） | `ngram` mode=dense, n=3；P2 开位置 | ≈ 10% | v1 2×，P2 位置局部化后预期 ≥ 10× |
| 要求 `(?i)` / ILIKE | 加 `lower_case=true` | 不变 | `(?i)` 查询从全扫变为可索引 |


## 5. 方案总览

### 5.1 一句话

在 Doris 现有 INVERTED 索引与 `ngram` 分词器上新增 **稀疏 / auto 模式**（不新增索引类型、不新增分词器类型，零配置 `"parser"="ngram"`），用 **内容定义边界的稀疏 gram（CDC-gram）** 建 **行级** DOCS_ONLY 倒排（V4 SPIMI 存储，两层 posting 表示），配一个 **Cox 式「正则 → gram 布尔查询」编译器**，把 `REGEXP / RLIKE / LIKE` 编译为 gram 查询求出候选行位图，未命中的页不读，命中的候选行再用 **BLARE 字面量预检 + Hyperscan** 复验。索引只做超集过滤，语义由原谓词兜底。

### 5.2 架构图

```
                 ┌──────────────── 写入路径（segment flush）────────────────┐
 列值(UTF-8 bytes) ─▶ GramExtractor(ngram tokenizer: auto|sparse|dense, lower_case?) ─▶ SpimiPostingBuffer(omit_tfap)
                       │                                                     │ spill/merge 复用 V4
                       │  段级 gram 布隆(负缓存)  ◀──── df 统计 ────▶ stop-gram 表(FREE 阈值)
                       ▼                                                     ▼
                  .idx 容器: [tii/tis 词典(高频 gram) | fp-table(稀有 gram 指纹) | frq(PFOR posting) | meta]

                 ┌──────────────── 查询路径（SegmentIterator）──────────────┐
 REGEXP 谓词 ─▶ RegexGramCompiler ─▶ GramQuery(AND/OR/ALL/NONE) ─▶ 段级布隆 ──否──▶ 段跳过
                    │(不可索引→ALL)                                       │是
                    │                                              df 驱动的 gram 选择 + 顺序求交(early-exit)
                    ▼                                                     ▼
              保留原表达式于 common expr ◀────── _row_bitmap &= 候选位图 ◀── posting(range-GET 批量)
                    │
                    ▼   只读候选行所在 page → Block → memmem/Teddy 字面量预检 → hs_scan 复验 → 输出
```

### 5.3 与三条硬约束的对应

| 约束 | 满足方式 | 证据 |
|---|---|---|
| 体积 ≤ 20–30% | 行级 + CDC 稀疏 gram（posting 数比稠密 3-gram 少 4×）+ 两层 posting（稀有 gram 只存指纹）+ FREE stop-gram + V4 PFOR | textbench 27.1%、httplogs 20.1%、wiki 9.3%（§4） |
| p50 ≥ 10× | 行级候选几乎等于真值、页级 I/O 免读、字面量快复验；选择率 ≤1% 的查询 30×–20000×，1–3% 为 5–20× | §4.6 分布；端到端含 I/O 的验收基准见 §9 |
| cutting edge | CDC-gram（GitHub/ClickHouse sparse grams 的局部化变体）+ Cox 编译器 + FREE/REI 选择 + BLARE 复验 + Airphant 式 S3 单轮取数 | §3 谱系逐条落地 |

### 5.4 非目标（v1 不做）

- 不改变 REGEXP 语义（RE2 语法）；不支持 `NOT REGEXP` 裁剪；不做 `(?i)` 的查询侧大小写枚举（用索引期 casefold 属性代替）。
- 不为 `\d{3}-\d{4}`、`[a-z]+@` 这类无字面量模式提供加速（任何 gram 索引都不能；REI 位向量作为 P2 可选补充）。
- 不做 `match_regexp`（term 级语义）的替换；两者并存。

## 6. 详细设计

### 6.1 gram 方案

#### 6.1.1 gram 的基本单位：按脚本自适应（ASCII 段用字节 gram，非 ASCII 码点用「字」）

- 行值先按 UTF-8 解码成码点流，再切成 **ASCII 段**（连续的 ASCII 字节）与 **非 ASCII 码点**两类单元：
  - ASCII 段内按 **字节** 切 gram（稀疏 CDC 或稠密 n-gram，§6.1.2/§6.1.3），段的两端是硬边界（gram 不跨出段）；
  - 每个非 ASCII 码点（CJK、假名、西里尔等）单独产出一个 **1-gram term**（term = 该码点的 UTF-8）。
- 依据：weibo 实测字节 3-gram 体积 109%、码点 2-gram 80%，而码点 1-gram 只有 26.2% 且 19/19 可索引、中位 75×（§4.7）——对 CJK，一个「字」的选择性相当于英文的一个词；多字字面量用 AND 组合足够精确。ASCII 文本上字节 gram 与 Cox/Zoekt/GitHub 一致。
- 查询侧对字面量做同样的分段：ASCII 段 → `grams_of`（只取窗口完整落在该段内的 gram），非 ASCII 码点 → 1-gram；全部 AND。含非法 UTF-8 的字面量 → 不可索引（保守）。
- 与 V4 词典（modified-UTF-8）的适配：非 ASCII 1-gram 本身就是合法 UTF-8；ASCII 字节 gram 全是 ASCII，也是合法 UTF-8——因此 **P0 不需要任何词典转义**（这是选择「ASCII 段字节 gram」而不是「全字节 gram」的第二个理由）。控制字符与 NUL：NUL 映射为 U+0100 后写词典（避开读侧 NUL 截断）。
- 可选扩展（P2）：对非 ASCII 段再按 CDC 采样码点 bigram，用于 CJK 长文本上进一步提高单 term 选择性；短消息上无必要。

#### 6.1.2 稀疏 gram：内容定义边界（CDC-gram）

定义（参数 `p`、`min_len=n`、`max_len=L`）：
1. 位置 i 是 **边界** ⇔ `H(b[i], b[i+1]) mod 2^16 < p·2^16`，`H` 为固定的 64 位混合哈希（版本化，进 index meta）。
2. 对每个边界 k，gram(k) = `b[k .. j+2)`，j 为 k 之后第一个满足 `j+2−k ≥ n` 的边界；若 `L` 字节内无此边界，则 gram(k) = `b[k .. k+L)`；若剩余长度不足 `L` 则不产出。
3. 同一行内 gram 去重后写入（V4 compact 模式天然去重）。

性质：
- **局部性**：gram(k) 只依赖 `b[k .. k+L)`。因此查询侧对任意字面量 s 计算出的每个 gram，只要其窗口完整落在 s 内，在任何包含 s 的行里一定被索引侧产出——这是「索引只能漏杀不能误杀」的正确性基础，也是与 GitHub sparse grams（权重比较也是局部的）和 ClickHouse `sparseGrams`（边界哈希比较）同源的原因。与固定步长采样（Loki `bloom_ngram_skip`）不同，CDC 不需要查询侧枚举对齐。
- **密度**：期望 gram 数 ≈ p × 行字节数（textbench 实测 0.199/字节 @p=0.25，稠密 3-gram 为 0.833），gram 期望长度 ≈ n + 1/p。
- **选择性**：gram 更长 → 单 gram 更选择；代价是短字面量（< 约 2/p 字节）可能没有完整 gram → 不可索引（回退全扫）。p 是体积–覆盖率的旋钮（§4.5：p=0.20/0.25/0.33 → 22%/27%/36%，可索引查询 22/23/25 of 29）。
- **哈希版本**：`H` 与参数写入 index meta；查询侧按段的参数编译（不同段可不同）。

#### 6.1.3 稠密模式（`mode=dense`）

同一解析器的退化：每个位置产出定长 n 字节 gram（n=3 默认）。适用：长文本列（wiki 类，gram 行内去重后 posting/byte 仅 0.13，索引 ≈ 8–9%）与低熵短串（URL 路径类，稀疏 gram 覆盖率不足）。

#### 6.1.4 高频 gram 裁剪（FREE 阈值 → stop-gram）

- 段 flush 时已知每个 gram 的 df；`df/N > τ`（默认 τ=0.25，属性可调）的 gram **不写 posting**，而是记入段内 `stop-gram` 列表（几十到几百项）。
- 查询语义：stop-gram → `ALL`；**词典中不存在的 gram → `NONE`**（这是 gram 索引最强的剪枝，绝不能因为裁剪而丢失，所以必须显式区分「被裁」与「不存在」）。
- 收益：textbench 稠密模式 85% → 48%（τ=0.05）且中位加速不降；httplogs 中 df>50% 的 21 个 gram 占 posting 的 49.9%。

#### 6.1.5 大小写与规范化

- 索引属性 `lower_case=true`（沿用 INVERTED 既有属性）时，索引侧与查询侧都先做 ASCII/Unicode simple case folding（GitHub 2026 做法）；此时 `(?i)`、`ILIKE`、`lower(col) REGEXP` 可走索引；大小写敏感查询仍正确（复验兜底），只是候选略多。默认 `false`（与现有 NGRAM_BF 语义一致）。
- 不做 Unicode 归一化（NFC/NFD）；不做标点折叠。

#### 6.1.6 NULL、空串与短值

- NULL 与长度 < n 的值不产 gram；正则若能匹配空串/短串（`canEmpty` 或不可索引），编译结果为 `ALL`，不使用索引。
- 编译器保证：只有当每条匹配都必然包含某个 gram 集合时才产出约束，因此短值无需特殊标记。

#### 6.1.7 自适应：让「不适合稀疏 gram 的语料」自动落到正确方案

§4.5–§4.8 表明没有一种 gram 方案通吃：日志类短文本要稀疏 gram，CJK 要码点 1-gram，低熵短串与长文本要稠密。自适应分四层，每层解决一种「不适合」，且**决策只依赖列的数据分布，不依赖查询**：

**第 1 层：行内按脚本（自动，无需决策）**。§6.1.1 的规则本身就是自适应的：ASCII 段走 gram 方案，非 ASCII 码点走 1-gram，中英混排一行内同时成立。CJK 语料的问题在这一层已经解决。

**第 2 层：段级自动选模式（`mode=auto`，即 `"parser"="ngram"` 的默认）**。索引写入器先攒一个样本（本段前 8,192 行或 4 MB 原值，二者先到为准；值只是暂存，内存可忽略），在样本上算三个统计量，据此锁定本段的方案并写入段级 index meta；随后正常流式提取。统计量与阈值全部来自 §4 的四个语料：

| 统计量 | 含义 | textbench | weibo | httplogs | wikipedia |
|---|---|---|---|---|---|
| L̄ | 平均值长度（字节） | 81 | 136 | 40 | 51,000 |
| D | 行内去重后的稠密 3-gram 数 / 字节 | 0.83 | 0.84 | 0.94 | 0.13 |
| H | 稠密 gram 中 df/N > 25% 的 gram 所占 posting 比例 | 11.5% | 6.5% | 59.4% | 59.0% |
| 决策 | | 稀疏 p=0.25 | 行内按脚本 → 1-gram | 稠密 + τ=0.25 | 稠密 |
| 实测 | 两层体积 / 可索引中位加速 | 24.8% / 11.7× | 26.2% / 75× | 41.1% / 20.5× | 9.3% / 2.3× |

决策规则（按顺序命中）：
1. `L̄ ≥ 2 KB` 或 `D ≤ 0.3` → **dense**（长文本行内去重后 posting 本来就少，稀疏反而让词典膨胀到 32%）。
2. `H ≥ 40%` → **dense + τ=0.25**（低熵、模板型：稀疏 gram 落在公共子串上没有选择性，httplogs 稀疏 1.4× vs 稠密+τ 20.5×）。
3. 否则 → **sparse p=0.25, L=16, τ=0.10**（日志类）。
4. 预算兜底：按样本估算本段体积（D × 每 posting 比特 / 8），若 dense 分支估算超过 `gram_size_budget`（默认 30%），改用 sparse p=0.40（httplogs：29.4% / 6.5×）。这是「预算优先」与「效果优先」之间唯一需要用户表态的地方，用属性表达。

样本决策可行的理由：三个统计量刻画的是列的静态分布，同一列不同段高度一致；一段一旦锁定不再改变，避免了流式写入中途切换方案的复杂度。

**第 3 层：查询级自适应（§6.4.3，与方案无关）**。df 驱动的 gram 选择、early-exit、放弃阈值对任何方案都生效。补测揭示一个事实：τ 裁剪带来的加速有一半来自「查询不再读高频 gram」，这部分查询侧成本模型本来就会做；τ 真正额外贡献的是存储（httplogs 57% → 41%）。

**第 4 层：列级建议器与负载回放（P2）**。`SHOW INDEX STATS` 输出上述统计量、当前各段方案分布与推荐值；有查询日志时按 REI/LPMS 的思路把查询字面量回放到样本上，直接量出覆盖率与候选比例，给出 p 的推荐。显式属性永远覆盖 auto。

段间异构的处理：不同段方案不同时，编译器按 `gram_scheme` 编译并缓存（一张表通常只有两三种）；compaction 走 V4「从列数据重建」时按合并后的样本重新决策；EXPLAIN 展示各段方案分布；`hash_version` 与方案一起进 meta。自适应不能解决的部分也要说清：短字面量的覆盖率由查询决定而非数据，p 的最终取值仍建议按列 override；无字面量正则在任何方案下都是 1×。

### 6.2 索引结构

#### 6.2.1 用户接口：不新增概念，复用 INVERTED 与现有 `ngram` 分词器

产品层原则（奥卡姆剃刀）：Doris 里已经有两处 n-gram——自定义分词器框架的 `ngram`/`edge_ngram` tokenizer，以及 `NGRAM_BF` 索引。本方案**不引入第三个**，也不新增 `USING NGRAM` 之类的索引类型。用户面只有一句话：**n-gram 是 INVERTED 索引的一种分词方式，用什么函数查决定它怎么用。**

- 索引类型：仍是 `INVERTED`。
- 分词：零配置走内置 parser 别名 `ngram`（`"parser"="ngram"`，等价于 auto 模式）；要调参走现有 `CREATE INVERTED INDEX TOKENIZER ... "type"="ngram"` 框架，在**这个既有 tokenizer 上新增参数**，而不是新增 tokenizer 类型。
- 查询：`MATCH_*` 在该索引上保持 token 语义（今天已如此）；`LIKE / REGEXP / RLIKE` 新增能力：只要列上的 INVERTED 索引的分词器属于 gram 族，就自动编译为 gram 查询走索引，建了即生效，不依赖 `enable_function_pushdown`。
- `NGRAM_BF`：标记为 legacy，文档引导迁移，不再演进；同一列同时存在时 `LIKE` 优先走 INVERTED。

```sql
-- 零配置：auto 模式（按段样本自适应稀疏/稠密，非 ASCII 码点按字，见 §6.1.7）
CREATE INDEX idx_msg ON logs(message) USING INVERTED PROPERTIES("parser"="ngram");

-- 大小写不敏感：沿用现有属性
CREATE INDEX idx_msg ON logs(message) USING INVERTED
PROPERTIES("parser"="ngram", "lower_case"="true");

-- 需要调参：沿用现有自定义分词器框架，只在 ngram tokenizer 上加参数
CREATE INVERTED INDEX TOKENIZER log_gram PROPERTIES(
  "type"      = "ngram",
  "mode"      = "sparse",      -- 新增：auto | sparse | dense（默认 auto；原有行为 = dense）
  "min_gram"  = "3",           -- 原有参数：稀疏模式下即 n
  "max_gram"  = "16",          -- 原有参数：稀疏模式下即 L
  "density"   = "0.25",        -- 新增专家参数 p
  "stop_gram_df" = "0.10"      -- 新增专家参数 τ
);
CREATE INVERTED INDEX ANALYZER log_gram PROPERTIES("tokenizer" = "log_gram");
CREATE INDEX idx_msg ON logs(message) USING INVERTED PROPERTIES("analyzer" = "log_gram");
```

兼容性：现有 `ngram` tokenizer 的默认行为（`min_gram=1`、`max_gram=2`、码点级、产出全部长度）保持不变，即 `mode=dense` 的一种取值；只有 `parser=ngram` 或显式 `mode=auto/sparse` 才启用新行为。已经用 `ngram` tokenizer 建好的 INVERTED 索引在升级后**自动获得** LIKE/REGEXP 加速：编译器按其 tokenizer 参数（稠密码点 gram）编译，无需重建。

用户抉择表（产品文档口径）：

| 需求 | 建法 | 查法 |
|---|---|---|
| 分词全文检索（相关性、短语） | `INVERTED` + 语言 parser（english/chinese/unicode/…） | `MATCH_*` |
| 子串 / 正则 / LIKE 加速 | `INVERTED` + `"parser"="ngram"` | `LIKE` / `REGEXP`，自动走索引 |
| 两者都要 | 同列两个 INVERTED 索引（不同 analyzer）；需确认并放开现有「一列一个倒排索引」的限制（开放问题 Q4）。放开之前，ngram 索引也能服务 `MATCH_ALL`（token = gram 语义） | 各走各的函数 |
| 老表已有 `NGRAM_BF` | 继续可用（仅 LIKE、页级）；建议 `DROP INDEX` 后改建 `INVERTED` + `ngram` | — |

实现映射（与前文一致）：`parser=ngram` → 内置 analyzer 别名 `InvertedIndexParserType::NGRAM`；tokenizer 参数 → 段级 index meta 的 `gram_scheme`（§6.2.3）；`GramExtractor` 作为 ngram tokenizer 的 auto/sparse 模式实现注册进现有 `analysis_factory_mgr`；`InvertedIndexColumnWriter::add_values` 在 gram 族分词器下 `omit_term_freq_and_positions=true`（v1）。

#### 6.2.2 两层 posting 表示（P1）

| 层 | 对象 | 存储 | 查询 |
|---|---|---|---|
| L-freq | df ≥ 3 的 gram | V4 词典（front-coding，小 term 内联）+ `.frq` PFOR 窗口（DOCS_ONLY） | 词典查找 → posting 解码 |
| L-rare | df ≤ 2 的 gram（textbench 占 distinct 的 95%、posting 的 10%） | **指纹表**：按 32 位哈希排序的 (fp, docid[1..2]) 数组，delta 编码，每项 ≈ 4 + 3·df 字节；不存 gram 文本 | 二分/分块查找；假阳性 2⁻³² 级，复验兜底 |
| stop | df/N > τ | 段内小表 | ALL |
| 段级布隆 | 全部 gram | 10 bit/gram，textbench ≈ 0.7% | 任一 AND 分量缺失 → 整段跳过（S3 上省一次 posting GET） |

体积效果（textbench，p=0.25）：单层词典 33.0% → 两层 27.1%；weibo cp 模式 80% → 70%；wiki CDC 46% → 24%（§4.5）。P0 先只做 L-freq（全部走 V4 词典），P1 加 L-rare 与段级布隆。

#### 6.2.3 元数据

- index meta 新增：`gram_scheme{mode, n, L, p, hash_version, lower_case, stop_df}`、`stop_gram_count`、`rare_table_offset/len`、`segment_bloom_offset/len`；旧版 BE 读到未知 parser → 拒绝使用该索引（不影响数据读取）。
- 统计：新增按索引类型的 `index_size` 明细（当前只有总 `index_size`，§2.1），以及 profile 计数 `RegexIndexSegmentsSkipped / GramsLookedUp / PostingBytesRead / CandidateRows / VerifiedRows / FallbackFullScan`。


### 6.3 正则 → gram 查询编译器（RegexGramCompiler）

#### 6.3.1 输入与输出

- 输入：REGEXP 模式串（RE2 语法，Doris 现有语义）、段的 `gram_scheme`。
- 输出：`GramQuery`，四种节点 `ALL / NONE / AND{grams, subs} / OR{grams, subs}`；`ALL` = 不可索引（走全扫），`NONE` = 无匹配（整段跳过，来源于「某必需 gram 在词典与稀有表都不存在」）。
- 正确性不变量：对任意行 r，`regex 匹配 r ⇒ GramQuery(r) = true`（只允许假阳性）。原型工具用「候选 ⊇ 真值」断言在 4 个语料 × 82 条正则上全部通过（§4.1）。

#### 6.3.2 解析器与支持的语法子集

自研递归下降解析器（不依赖 RE2 内部头文件，RE2 2021 版未导出 `Regexp` 树），覆盖 RE2 语法：字面量、转义（`\. \n \t \r \xHH \x{...} \Q..\E`）、类（`[...]`、取反、区间、POSIX 类、`\d \w \s \D \W \S \pL`）、`.`、分组（捕获/非捕获/命名）、标志 `(?i) (?s) (?m) (?U)`、量词 `* + ? {m} {m,} {m,n}` 及懒惰后缀、锚点 `^ $ \b \B \A \z`、顶层与嵌套 `|`。**任何解析失败 → `ALL`**（保守），并计数上报。

#### 6.3.3 五元组推导（Cox 2012 的 C++ 移植，扩展到变长 gram）

每个 AST 节点计算 `Info{canEmpty, exact?, prefix, suffix, match}`：

| 节点 | 规则 |
|---|---|
| 空/锚点 | `canEmpty=1, exact={""}` |
| 字面量 c | `exact={c}`；`(?i)` 下 ASCII 字母展开为 `{c, C}` |
| 小类（≤4 个码点） | `exact` = 各码点；大类/取反类/`.` → `anyChar`（`prefix=suffix={""}`） |
| 连接 xy | 两侧 exact 且叉积 ≤20 → `exact=cross`；否则 `prefix = x.exact×y.prefix` 或 `x.prefix ∪ (x.canEmpty? y.prefix)`，`suffix` 对称；两侧都非 exact 时把 `cross(x.suffix, y.prefix)` 的 gram 作为**边界 gram** `AND` 进 match |
| 交替 x\|y | 两侧 exact 且并集 ≤20 → `exact=∪`；否则各自降级（exact 的 gram 折入自身 match），`prefix/suffix` 取并，`match = OR` |
| x\* | `anyMatch`（`canEmpty=1`） |
| x+ | x 降级（exact → prefix/suffix，gram 折入 match） |
| x? | `alt(x, empty)` |
| x{m,n} | m=0 → anyMatch（n=0 → empty）；m≥1 → 展开 min(m,4) 次连接后按 x+ 处理 |

化简（信息保留）：exact 集合过大（>7，或全部 ≥n 且 >4，或最短串 ≥2n）时先把每个串的 gram 折入 `match`（`OR` over 串 of `AND` over gram），再把串裁到 **保留长度**（稠密：n−1；稀疏：L）作为 prefix/suffix；集合 >20 时逐字符再裁直到 ≤20 或成为 `{""}`。根节点最后把 exact（或 prefix、suffix）的 gram 折入 match。

变长 gram 的差异：`trigrams(s)` 换成 `grams_of(s)`（同 §6.1.2 的规则，只产出窗口完整落在 s 内的 gram）；边界 gram 的裁剪长度从 n−1 提到 L，保证跨节点拼接后仍能产出完整 gram。

布尔化简：AND/OR 扁平化、gram 去重、吸收律（AND 内已有 g 则含 g 的 OR 子式恒真；OR 内已有 g 则含 g 的 AND 子式被蕴含；OR 内 AND 子集吸收超集）、结构去重。

示例（p=0.25，实际 gram 由哈希决定）：

| 正则 | 稠密 3-gram 查询 | CDC-gram 查询 |
|---|---|---|
| `error.*timeout` | `err∧rro∧ror∧tim∧ime∧meo∧eou∧out` | `timeo` |
| `rpc error: code = (Unavailable\|Internal)` | 16 个公共 gram ∧ (`Una∧...` ∨ `Int∧...`) | `or: co ∧ ode = U ∧ ( Unavai ∧ ailable ∧ cod)`（Internal 分支无完整 gram → 该分支 ALL → 交替退化为公共部分） |
| `conn(ection)? re(set\|fused)` | ` re∧con∧onn∧(efu∧fus∧ref∧sed∧use ∨ ese∧res∧set)` | 同形 |
| `GET\|POST` | `GET ∨ (POS∧OST)` | ALL（字面量太短） |
| `\d{3}-\d{4}`、`a.*b`、`[a-z]+@` | ALL | ALL |

#### 6.3.4 不可索引与 NOT 语义

- `ALL` 时不使用索引，谓词按现状执行；profile 记 `FallbackFullScan`。
- `NOT REGEXP` / `NOT LIKE`：编译器直接返回 `ALL`（现有 `LikeColumnPredicate::evaluate_and` 忽略 `_opposite` 的隐患在新索引里不复制）。
- `LIKE`：通配符 `%`/`_` 处切断为字面量段（与现有 `next_in_string_like` 一致），每段走 `grams_of`，段间 `AND`；`ILIKE` 仅在 `lower_case=true` 时可索引。

### 6.4 查询执行

#### 6.4.1 FE 侧识别与下推

- Nereids：`Regexp`/`RegexpLike`/`Like` 表达式在其列拥有 `sparse_gram/dense_gram` 索引且模式为常量时，标记为可走索引的 common expr 下推（与 `match` 系函数的处理一致），**不依赖 `enable_function_pushdown`**（该变量只门控 LIKE 转 `LikeColumnPredicate` 的老路径，且默认 false）。
- 会话变量：`enable_regex_gram_index`（默认 true）、`regex_gram_index_max_candidate_ratio`（候选比例阈值，默认 0.3：编译后估计候选 > 30% 行时放弃索引，避免付出 posting I/O 而无收益）。

#### 6.4.2 BE 侧：近似索引语义（关键改动点）

现有表达式级索引路径把索引结果视为精确并移除表达式（§2.4）。本方案在 `IFunction::evaluate_inverted_index` 的结果中增加 `approximate=true` 标记：

1. `SegmentIterator::_apply_index_expr` 得到候选位图后 `_row_bitmap &= 候选`；
2. 若 `approximate`，**不**从 `_common_expr_ctxs_push_down` 移除该表达式，并把它标注为「仅需在候选行上求值」；
3. `_next_batch_internal` 的 common expr 阶段按现有流程在 Block 上执行 REGEXP——此时 Block 里只有候选行，即用户要求的「先索引过滤，剩余按 block 匹配」。

页级 I/O 裁剪自动获得：`_range_iter` 只读位图内 rowid，无候选的 page 不解码（`segment_iterator.cpp:2375-2378`）。

#### 6.4.3 gram 查询求值：成本驱动

对每个 segment：
1. **段级布隆**（P1）：AND 顶层任一 gram 不在布隆 → 该段 `NONE`，零 posting I/O。
2. **df 查询**：对 AND 的所有 gram 只查词典/稀有表拿 df（`docFreq` 不读 posting，§2.5），按 df 升序排序；OR 子式取各分支 df 之和作为估计。
3. **自适应求交**（FREE/VGGraph 思想的查询侧版本）：从最小 df 开始逐个求交；当当前候选基数已 ≤ `max(64, N·0.1%)` 或再加一个 gram 的「预计裁剪收益 < posting 读取代价」时提前停止（S3 上每个 posting 窗口是一次 range-GET，代价可量化）。stop-gram 与 df 极高的 gram 因此几乎从不被读取。
4. **放弃阈值**：最小 df / N > `regex_gram_index_max_candidate_ratio` → 放弃索引（等价于 §4.6 中 hit ≥ 10% 的查询，索引无收益）。
5. 位图 AND/OR 用 roaring；posting 解码复用 `SpimiQueryTermDocs`（窗口懒解码 + skipTo），求交时用 skip 表跳过。

#### 6.4.4 复验流水线（BLARE + Hyperscan）

对候选行 Block：
1. **字面量预检**：编译器同时输出模式的 **必需字面量集**（Cox 的 exact/prefix/suffix 与边界串，取最长者）；先用 `memmem`（单串）或 Hyperscan 字面量库 `hs_compile_lit`/Teddy（多串）扫描，未命中的行直接判否；纯字面量模式到此结束（无需正则）。
2. **正则复验**：其余行走现有 `constant_regex_fn`（hs_scan，失败回退 RE2）；复用 `LikeState` 的 hs db + 每线程 scratch，避免逐 Block 重编译。
3. 复验成本模型（§4.6）：候选 ≈ 真值时，总时间 ≈ 真命中行数 × 单行复验成本，因此宽泛查询的加速上界 = 1/选择率；这是设计的固有边界而非实现缺陷。

#### 6.4.5 长文本列（P2）

`support_phrase=true` 时写位置流（V4 `.prx`）；查询侧用 gram 相邻关系（位置差 = gram 起点差）在索引内剔除「gram 都有但顺序不对」的候选（Zoekt/Cursor），并把 hs 复验限定在候选位置 ± L 的窗口，把 51 KB 文档的复验从整篇缩到几百字节（wiki §4.7 的 2.3× 主要受此限制）。

### 6.5 S3 访问模型（与 SNII/BASALT 对齐）

| 层 | 内容 | 驻留 | 访问 |
|---|---|---|---|
| L0 常驻 | index meta、`.tii` 采样、stop-gram、段级布隆、稀有指纹表索引头 | 内存（textbench ≈ 0.7% + 0.2%） | 打开段时一次读入 |
| L1 | 词典块 `.tis`（front-coding，4 KB 块） | 缓存 | 每 gram 一次块读，同批 gram 合并为一次 range-GET |
| L2 | posting 窗口 `.frq`、稀有表数据 | 按需 | 求交前**一次性并行**取回被选中 gram 的窗口（Airphant「一轮往返」），按 df 顺序解码，early-exit 时未取的窗口不读 |

- 请求轮次目标：段级布隆命中 → 1（词典）+ 1（posting 批量）= 2 个 RTT；未命中 → 0。
- 当前 V4 读侧是整流读入（§2.5），P0 阶段在本地盘/文件缓存下可接受；P1 依赖 SNII 的 `PostingStore` 定位读接缝落地。
- 体积–请求权衡：`gram_max_len` 越大 gram 越选择、posting 越短，但词典越大；默认 L=16 是 textbench 上词典与 posting 的折中（§4.5）。

### 6.6 写入、溢写、合并与 compaction

- 写入：`GramExtractor` 为每行产出 gram 集合并 `Append`；边界判定用 65536 项位图查表（8 KB，每字节对一次查表），gram 哈希直接在原串上算，不拷贝。

**写入成本估算**（单线程 CPU；提取吞吐为原型实测，posting 侧按本项目 V4 全文索引 DOCS_ONLY 基准折算：textbench english 分词 ≈ 0.42 µs/行、存储层 ≈ 1.5 µs/行、约 10 token/行 → ≈ 0.15 µs/posting，稀有 term 多时取上限 2×）：

| 方案 | 提取实测 | posting/行（去重后） | 估算写入 CPU | 相对 V4 全文索引（同列 DOCS_ONLY） |
|---|---|---|---|---|
| 稠密 3-gram（textbench） | 134 ns/行，580 MB/s | 68 | ≈ 10 µs/行，≈ 8 MB/s | ≈ 5× |
| **稀疏 CDC p=0.25, L=16（textbench）** | 394 ns/行，197 MB/s | 16 | ≈ 2.7–4 µs/行，≈ 20–30 MB/s | **≈ 1.4–2×** |
| 稀疏 + lower_case | 601 ns/行，129 MB/s | 16 | ≈ 2.9–4.2 µs/行 | ≈ 1.5–2.1× |
| CJK 码点 1-gram（weibo） | 272 ns/行，475 MB/s | 38 | ≈ 5.8 µs/行，≈ 23 MB/s | ≈ 0.9×（V4 unicode 全文 ≈ 6.5 µs/行） |
| 稠密 3-gram + τ（httplogs 40 B 行） | 72 ns/行，526 MB/s | 37 | ≈ 5.4 µs/行，≈ 7 MB/s | 该列无全文索引对照 |

读法：提取本身很便宜，稀疏方案的写入成本几乎全部来自 posting 条数，因此「稀疏 gram 比稠密少 4× posting」同时也是「写入 CPU 少 4×」；推荐配置下大约是同列 V4 全文索引写入成本的 1.4–2 倍，与 §9.2 B6「stream load 吞吐下降 ≤ 15%」的验收条是否相容，取决于全文索引在该表导入 CPU 中的份额，必须端到端实测而不是推算。内存：稀疏 gram 16 posting/行 × compact 约 3 B，1M 行约 50 MB，在默认 128 MB 溢写预算内不溢写；稠密则 4 倍并触发溢写。词典侧稀疏 gram 的 distinct 数是稠密的 30 倍（47 万 vs 1.6 万），flush 时排序与 front-coding 写出约几十到一百毫秒/段，可忽略。`gram_mode=auto` 的样本暂存 4 MB，延迟一次提取，开销可忽略。可进一步优化：一趟预计算「下一个边界」数组把内层扫描降为 O(L)、行内先去重再 Append 减少 buffer 写入、SIMD 批量查表。
- 溢写/归并：复用 `SpillManager`/`SegmentMerger`（omit 模式字节直拷）；stop-gram 判定在最终 EmitSegment 时按全段 df 完成，spill 期间不裁。
- 稀有指纹表（P1）：EmitSegment 时按 df ≤ 2 分流，不进入词典。
- compaction：沿用 V4「从列数据重建」门控（§2.5）；未来 SNII Tier A/B 的 stitch/remap 对 DOCS_ONLY gram 索引同样适用（无 tf/位置，最简单的情形）。
- BUILD INDEX / light schema change：与 INVERTED 相同。

### 6.7 兼容性与降级

- `parser=ngram` 与 auto/sparse 模式仅 V4（SPIMI）存储格式；V2/V3 存储格式下建索引报错「需要 inverted_index_storage_format=V4」。既有的稠密码点 `ngram` tokenizer 索引不受影响。
- 旧 BE 读到 auto/sparse 模式的索引：索引不可用但数据可读；FE 版本校验在 tokenizer 参数校验处做。
- 索引缺失/损坏/编译 ALL：一律退回现有 REGEXP 执行路径；任何索引侧错误只能导致「不加速」，不能改变结果。
- 与 NGRAM_BF 并存：同一列允许同时有 NGRAM_BF（服务 LIKE 页级）与 gram 倒排（服务 REGEXP/LIKE 行级）；建议文档引导迁移。

### 6.8 可观测性与配置

- profile：`RegexIndexCompileTime / CompiledLeaves / SegmentsSkippedByBloom / GramsLookedUp / PostingBytesRead / CandidateRows / VerifiedRows / LiteralPrefilterDropped / FallbackFullScan`。
- BE 配置：`regex_gram_index_enable`、`regex_gram_index_max_set=20`、`regex_gram_index_max_exact=7`、`regex_gram_index_early_exit_rows`。
- 索引/分词器属性见 §6.2.1；EXPLAIN 输出编译后的 gram 查询（便于用户理解为何某模式不走索引）。


## 7. 与三条需求的对照与承诺边界

| 需求 | 承诺 | 依据 | 边界 |
|---|---|---|---|
| 索引体积 ≤ 数据的 20–30% | 推荐默认配置（sparse_gram p=0.25, L=16, τ=0.10 + 两层存储）在日志类短文本上 **≈ 25%**；CJK 短文本 16–26%；长文本 ≈ 10% | §4.5、§4.7 的两层口径，FOR-128 比 V4 PFOR 偏保守 | 低熵短串（URL 类）用稠密模式会到 30–45%，需用 τ 或接受更低覆盖率的稀疏模式（20%）；P0 未做两层时日志类为 ≈ 33% |
| p50 比无索引 REGEXP 快 ≥ 10× | 对「中位查询选择率 ≤ 1–2%、含 ≥ 8 字节字面量」的负载成立：可索引查询 CPU-only p50 = 11.7×（推荐配置）、选择率 <1% 的查询 30×–数千×；端到端含列读取后更高 | §4.6 分布；候选 ≈ 真值 | 宽泛查询上界 = 1/命中率；无字面量模式 1×；这是所有 gram 索引的共同边界（ClickHouse/Snowflake/Loki 文档均如此声明） |
| 调研最新产品与论文、做成 cutting edge | 采用并组合：CDC 稀疏 gram（GitHub/ClickHouse 2023–2026 sparse grams 的局部化变体）、Cox 编译器、FREE/VLDB'26 的阈值裁剪、REI 的「索引查询」思想（P2）、BLARE 复验（SIGMOD'23）、Airphant/LogCloud 的 S3 单轮取数（ICDE'22/VLDB'25）、Hyperscan | §3 | 位置局部化、负载感知 gram 选择放在 P2 |

**相对现有产品的差异化**：ClickHouse `text` 索引的 `sparseGrams` 是「所有 ≥n 且边界哈希大于内部」的子串（数量 ≥ 稠密 n-gram），追求选择性而非体积；本方案的 CDC-gram 以密度 p 直接换体积（posting 数 1/4），并用两层存储解决稀疏 gram 的词典膨胀——这是在 20–30% 预算下同时保住行级精度的关键组合，目前没有产品化实现同时做到这三点（行级、稀疏、预算内）。

## 8. 风险与开放问题

| # | 风险 / 问题 | 影响 | 缓解 |
|---|---|---|---|
| R1 | V4 词典要求 term 为合法 UTF-8 且读侧 NUL 截断（§2.5） | 若走全字节 gram 需转义 | 采用「ASCII 段字节 gram + 非 ASCII 码点 1-gram」，天然合法 UTF-8，无需转义；NUL 映射为 U+0100；SNII 原生字节词典落地后可放开 |
| R2 | 读侧整流读入（§2.5）在 S3 上是整段 GET | P0 的 S3 冷查询收益有限 | 与 SNII `PostingStore` 定位读接缝同期落地；段级布隆先做，能直接省掉整段 GET |
| R3 | 表达式级索引路径的「近似语义」改动触及 `SegmentIterator` 主干 | 回归风险 | 只对 `approximate=true` 分支生效；现有 match 路径不变；用 `need_remaining_after_evaluate` 的既有测试模板补齐 |
| R4 | 编译器的正确性（假阴性 = 漏结果） | 语义错误 | 原型已通过 82 条 × 全配置超集断言；产品化时加 **差分模糊测试**（随机正则 × 随机行，对比 re2 真值），作为 UT 常驻 |
| R5 | 哈希函数/参数版本漂移导致查询侧与索引侧 gram 不一致 | 漏结果 | `hash_version` 入 meta，查询侧严格按段参数编译；UT 固化 golden gram 集 |
| R6 | 稀疏 gram 的覆盖率：短字面量不可索引 | 用户预期落差 | EXPLAIN 显示编译结果与原因；文档给出最短字面量经验值（≈ 8 字节 @p=0.25）；允许按列调 p |
| R7 | 宽泛查询走索引反而更慢（posting I/O 无收益） | 性能回退 | §6.4.3 的放弃阈值与 early-exit；profile 计数暴露 |
| R8 | compaction 重建索引成本（V4 门控为从列数据重建） | 写放大 | CDC gram 数少、DOCS_ONLY，重建成本低于全文索引；SNII Tier A stitch 对 DOCS_ONLY 最简单 |
| R9 | Doris 已有 `ngram` tokenizer、`NGRAM_BF`、`match_regexp` 三处相近概念 | 产品面歧义 | §6.2.1 的统一接口：不新增索引类型与 tokenizer 类型，n-gram 只是 INVERTED 的分词方式；NGRAM_BF 标记 legacy 并引导迁移；`match_regexp` 保持 term 语义不变 |
| Q1 | 是否把 `LIKE` 也切到本索引的行级路径（替代 `LikeColumnPredicate` + NGRAM_BF）？ | — | 建议是（同一编译器、同一路径），但作为独立小 PR |
| Q2 | 默认 p / L / τ 的最终取值 | — | 用 §9 的端到端基准在 3 个真实表上定；本文推荐 0.25 / 16 / 0.10 |
| Q3 | 是否需要查询侧 `(?i)` 展开（≤8 变体）作为无 lower_case 索引时的兜底？ | — | v1 不做（Cox/Zoekt 经验：精度差、posting 读 8×） |
| Q4 | 是否放开「一列只能有一个 INVERTED 索引」的限制，允许同列同时有语言分词索引与 ngram 索引（不同 analyzer）？ | 产品能力 | 建议放开，按 analyzer 区分；放开前 ngram 索引可兼服务 `MATCH_ALL` |

## 9. 实施计划

### 9.1 分期

**P0（可交付 MVP，约 4–6 周）**
1. `GramExtractor`（dense/sparse/CJK 自适应，casefold）+ golden 测试；`RegexGramCompiler`（解析器、五元组、化简、LIKE 分支）+ 差分模糊测试。
2. FE：`parser=ngram` 别名、`ngram` tokenizer 新参数（mode/density/stop_gram_df）校验、Nereids 下推标记、会话变量。
3. BE：`GramExtractor` 作为 ngram tokenizer 的 auto/sparse 模式接入 `InvertedIndexColumnWriter`（V4 DOCS_ONLY，ASCII 段字节 gram + 非 ASCII 码点 1-gram，无需词典转义）、`FunctionRegexp/FunctionLike::evaluate_inverted_index`、`approximate` 语义改动、复验预检（memmem/hs 字面量）、profile 计数。
4. 回归测试：正则语义对照（走索引 vs 不走索引结果一致）、NULL/空串/短值、NOT、`(?i)`、多段/多 rowset、delete bitmap、schema change、BUILD INDEX。
5. 验收：§9.2 基准的 B1–B3。

**P1（体积与 S3，约 4 周）**
6. 两层 posting：稀有 gram 指纹表、stop-gram 表、段级布隆；按索引类型的体积统计。
7. df 驱动的 gram 选择、early-exit、放弃阈值；posting 窗口批量 range-GET（依赖 SNII `PostingStore` 接缝）。
8. 验收：B4（体积 ≤ 30%）、B5（S3 冷查询 RTT ≤ 2）。

**P2（进阶）**
9. 位置流与复验局部化（长文本）；gram 相邻剪枝。
10. 负载感知：REI 式热 bigram 位向量或 LPMS 回放，覆盖无字面量/短字面量模式。
11. SNII Tier A/B 合并适配。

### 9.2 验收基准（端到端，Doris 集群）

| 编号 | 内容 | 通过标准 |
|---|---|---|
| B1 | textbench 30M 行 `Body` 列（现有 E2E 驱动 `e2e_driver2.sh`），29 条正则，`enable_regex_gram_index` on/off 各 5 轮取中位 | 可索引查询 p50 ≥ 10×；结果集逐条一致 |
| B2 | weibo 500K 行 CJK 列，19 条正则 | p50 ≥ 10×；结果一致 |
| B3 | httplogs 247M 行 `request` 列，15 条正则 | 选择率 <1% 查询 ≥ 10×；宽泛查询不慢于无索引 ×1.05 |
| B4 | 三张表的 `index_disk_size / data_disk_size` | 日志与 CJK 表 ≤ 30%；URL 表 ≤ 45%（稠密）或 ≤ 25%（稀疏） |
| B5 | 云模式（S3 + file cache 冷）：单段单查询的远端请求次数 | 段级布隆未命中 0 次；命中 ≤ 2 次（P1） |
| B6 | 写入吞吐：带索引 vs 不带索引 stream load | 下降 ≤ 15%（对照现有 V4 全文索引的口径） |

基准方法沿用本项目既有约定：共享开发机上以 CPU 时间/中位数、交错运行为准（见记忆中 SPIMI 基准方法论），磁盘字节与 RSS 为严格对比轴。

### 9.3 交付物

- 本设计文档（评审后转为实施计划，按 brainstorming → writing-plans 流程）。
- 原型工具与全部实验输出：`tools/regex-ngram-model/`（可复现 §4 全部表格）。
- 评审后：P0 的实施计划文档 + 按 §9.1 拆分的 PR 序列（编译器可作为第一个独立 PR，因其无存储依赖、可单测）。


## 附录 C：调研来源索引

### C.1 产品文档 / 源码 / 博客

- [CH-1] ClickHouse MergeTree 跳数索引函数支持表：https://clickhouse.com/docs/engines/table-engines/mergetree-family/mergetree#functions-support
- [CH-2] ClickHouse skipping indexes：https://clickhouse.com/docs/optimize/skipping-indexes
- [CH-3] ClickHouse PR #79985（`sparse_grams` 索引）：https://github.com/ClickHouse/ClickHouse/pull/79985
- [CH-4] ClickHouse PR #57882（`match()` 走 ngrambf/tokenbf）：https://github.com/ClickHouse/ClickHouse/pull/57882
- [CH-5] ClickHouse `ITokenizer.cpp`：https://github.com/ClickHouse/ClickHouse/blob/master/src/Interpreters/ITokenizer.cpp
- [CH-6] ClickHouse text 索引文档：https://clickhouse.com/docs/engines/table-engines/mergetree-family/textindexes
- [CH-7] ClickHouse `OptimizedRegularExpression.cpp`：https://github.com/ClickHouse/ClickHouse/blob/master/src/Common/OptimizedRegularExpression.cpp
- [CH-8] ClickHouse `MergeTreeIndexBloomFilterText.cpp`：https://github.com/ClickHouse/ClickHouse/blob/master/src/Storages/MergeTree/MergeTreeIndexBloomFilterText.cpp
- [CH-9] ClickHouse 字符串搜索函数：https://clickhouse.com/docs/sql-reference/functions/string-search-functions
- [CH-10] ClickHouse `MultiMatchAnyImpl.h`：https://github.com/ClickHouse/ClickHouse/blob/master/src/Functions/MultiMatchAnyImpl.h
- [CH-11] ClickHouse 全文检索 GA 博客：https://clickhouse.com/blog/full-text-search-ga-release
- [CH-12] ClickHouse 面向对象存储的全文索引博客：https://clickhouse.com/blog/clickhouse-full-text-search-object-storage
- [CH-13] ClickHouse `sparseGrams` 函数：https://clickhouse.com/docs/sql-reference/functions/string-functions
- [CH-14] ClickHouse `MergeTreeIndexConditionText.cpp`：https://github.com/ClickHouse/ClickHouse/blob/master/src/Storages/MergeTree/MergeTreeIndexConditionText.cpp
- [CH-15] ClickHouse 26.04 发布博客（LIKE 词典扫描）：https://clickhouse.com/blog/clickhouse-release-26-04
- [CH-16] ClickHouse `Settings.cpp`：https://github.com/ClickHouse/ClickHouse/blob/master/src/Core/Settings.cpp
- [SF-1] Snowflake Search Optimization Service：https://docs.snowflake.com/en/user-guide/search-optimization-service
- [SF-2] Snowflake 专利 US10997179B1：https://patents.google.com/patent/US10997179B1/en
- [SF-3] Snowflake substring / regex 查询：https://docs.snowflake.com/en/user-guide/search-optimization/substring-queries
- [SF-4] Snowflake 半结构化列子串搜索：https://docs.snowflake.com/en/user-guide/search-optimization/semi-structured-queries
- [SF-5] Snowflake 成本估算：https://docs.snowflake.com/en/user-guide/search-optimization/cost-estimation
- [ES-1] Elastic 博客 wildcard 字段：https://www.elastic.co/blog/find-strings-within-strings-faster-with-the-new-elasticsearch-wildcard-field
- [ES-2] `WildcardFieldMapper.java`：https://github.com/elastic/elasticsearch/blob/main/x-pack/plugin/wildcard/src/main/java/org/elasticsearch/xpack/wildcard/mapper/WildcardFieldMapper.java
- [ES-3] Elasticsearch PR #55548：https://github.com/elastic/elasticsearch/pull/55548
- [ES-5] Lucene `RegexpQuery`：https://lucene.apache.org/core/9_11_1/core/org/apache/lucene/search/RegexpQuery.html
- [GCS-1] Russ Cox, Regular Expression Matching with a Trigram Index：https://swtch.com/~rsc/regexp/regexp4.html
- [GCS-2] google/codesearch `index/regexp.go`：https://github.com/google/codesearch/blob/master/index/regexp.go
- [ZK-4] Zoekt 设计文档：https://github.com/sourcegraph/zoekt/blob/main/doc/design.md
- [ZK-6] Zoekt `matchtree.go`：https://github.com/sourcegraph/zoekt/blob/main/index/matchtree.go
- [GH-7] GitHub Blackbird 技术博客：https://github.blog/engineering/architecture-optimization/the-technology-behind-githubs-new-code-search/
- [GH-8] GitHub case-folding 博客（2026）：https://github.blog/engineering/architecture-optimization/dont-stop-early-case-folding-source-code-at-memory-speed/
- [GH-10] danlark1/sparse_ngrams：https://github.com/danlark1/sparse_ngrams
- [CU-11] Cursor fast regex search（2026-03）：https://cursor.com/blog/fast-regex-search
- [PG-1] PostgreSQL pg_trgm 文档：https://www.postgresql.org/docs/current/pgtrgm.html
- [PG-4] `trgm_regexp.c`：https://github.com/postgres/postgres/blob/master/contrib/pg_trgm/trgm_regexp.c
- [LK-1] Loki 3.0 query acceleration blooms：https://grafana.com/docs/loki/v3.0.x/operations/query-acceleration-blooms/
- [LK-3] Loki 3.3 blooms for structured metadata：https://grafana.com/blog/2024/11/21/grafana-loki-3.3-release-faster-query-results-via-blooms-for-structured-metadata/
- [VL-5] VictoriaLogs 列式存储内幕：https://victoriametrics.com/blog/victorialogs-internals-columnar-storage-on-disk/
- [VL-6] VictoriaLogs `filter_regexp.go`：https://github.com/VictoriaMetrics/VictoriaLogs/blob/master/lib/logstorage/filter_regexp.go
- [QW-8] tantivy `RegexQuery`：https://docs.rs/tantivy/latest/tantivy/query/struct.RegexQuery.html
- [SR-1] StarRocks N-gram bloom filter index：https://docs.starrocks.io/docs/table_design/indexes/Ngram_Bloom_Filter_Index/
- [DR-3] Doris NGRAM_BF 文档：https://doris.apache.org/docs/table-design/index/ngram-bloomfilter-index
- [DB-6] Databend NGRAM INDEX：https://docs.databend.com/sql/sql-commands/ddl/ngram-index/create-ngram-index
- [TI-7] TiDB Cloud Lake ngram index：https://docs.pingcap.com/tidbcloudlake/ngram-index/
- [MV-8] Milvus NGRAM 索引：https://milvus.io/docs/ngram.md
- [MV-9] Milvus NGRAM 博客：https://milvus.io/blog/milvus-ngram-index-faster-keyword-matching-and-like-queries-for-agent-workloads.md
- [MT-10] Manticore 通配设置：https://manual.manticoresearch.com/Creating_a_table/NLP_and_tokenization/Wildcard_searching_settings
- [HS-1] Hyperscan 编译文档：https://intel.github.io/hyperscan/dev-reference/compilation.html
- [HS-2] Hyperscan NSDI'19 论文：https://www.usenix.org/system/files/nsdi19-wang-xiang.pdf
- [RS-4] regex-syntax `literal::Extractor`：https://docs.rs/regex-syntax/latest/regex_syntax/hir/literal/struct.Extractor.html
- [RS-6] regex 内幕（BurntSushi）：https://burntsushi.net/regex-internals/
- [RE2-8] RE2 `prefilter.h`：https://github.com/google/re2/blob/main/re2/prefilter.h
- [RE2-9] RE2 `filtered_re2.h`：https://github.com/google/re2/blob/main/re2/filtered_re2.h

### C.2 论文

- FREE：Cho & Rajagopalan, ICDE 2002：https://dl.acm.org/doi/10.5555/876875.879032
- BEST：Hore 等, CIKM 2004：https://dl.acm.org/doi/10.1145/1031171.1031212
- Baeza-Yates & Gonnet, JACM 1996：https://dl.acm.org/doi/10.1145/235809.235810
- Ukkonen, q-gram 引理, TCS 1992：https://www.cs.helsinki.fi/u/ukkonen/TCS92.pdf
- BLARE：Zhang 等, SIGMOD 2023：https://dl.acm.org/doi/10.1145/3589297
- n-gram 选择策略评测：Zhang 等, VLDB 2026（arXiv 2504.12251）：https://arxiv.org/abs/2504.12251
- REI：Zhang 等, SIGMOD 2026（arXiv 2510.10348）：https://arxiv.org/abs/2510.10348
- 查询计划树：Qiu 等, DASFAA 2023：https://link.springer.com/chapter/10.1007/978-3-031-30637-2_3
- VGGraph：Qiu 等, DASFAA 2024：https://link.springer.com/chapter/10.1007/978-981-97-5779-4_22
- Airphant：Chockchowwat 等, ICDE 2022：https://arxiv.org/abs/2112.13323
- LogCloud：Wang 等, VLDB 2025：https://www.vldb.org/pvldb/vol18/p2362-wang.pdf
- CLP：Rodrigues 等, OSDI 2021：https://www.usenix.org/conference/osdi21/presentation/rodrigues
- LogGrep：Wei 等, EuroSys 2023：https://dl.acm.org/doi/10.1145/3552326.3567484
- μSlope：Wang 等, OSDI 2024：https://www.usenix.org/conference/osdi24/presentation/wang-rui
- Hyperscan：Wang 等, NSDI 2019：https://www.usenix.org/conference/nsdi19/presentation/wang-xiang
- RE#：Varatalu 等, POPL 2025：https://dl.acm.org/doi/10.1145/3704837
- .NET NonBacktracking：Moseley 等, PLDI 2023：https://dl.acm.org/doi/10.1145/3591262
- SIMD 正则：Sitaridi 等, DaMoN 2016：http://www.cs.columbia.edu/~eva/simd_regex_damon2016.pdf
- HybridSA：OOPSLA 2024：https://dl.acm.org/doi/10.1145/3689771 ；ngAP：ASPLOS 2024：https://dl.acm.org/doi/10.1145/3617232.3624848

### C.3 未能核实的条目（沿用调研代理的标注）

- FREE 原文 PDF 不可访问，useful/prefix-free 定义取自 Zhang 等的复述；ICDE 2002 具体倍数未核实。
- LPMS 原始出处未核实；DASFAA 2023/2024 两篇原文数字未取得。
- Snowflake 现行实现是否仍为专利中的 N-gram + blocked bloom 未核实（官方文档不提 n-gram）。
- pg_trgm 无官方体积数字；StarRocks NGRAMBF 粒度/体积未核实；MySQL LIKE/REGEXP 无索引路径为常识但未逐条核实。
- 「learned regex index」2024–2026 未检索到同行评审论文。


