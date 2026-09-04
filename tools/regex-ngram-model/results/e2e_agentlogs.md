# agentlogs 验收基准：2M 行 agent 观测日志中英混排长文本（稀疏 gram）

> 本文件由 `tools/regex-ngram-model/e2e/bench_gram_regexp.sh` 实测生成，数字未做任何修饰。
> 结论先说：**体积达标且是四张基准表里最好的一张（索引 = 原始列字节的 23.2 %），
> 索引在页层面把 736 MB / 11,439 页的列扫描压到 324 KB / 3 页（最好的一条），
> 但可索引查询 p50 加速只有 1.50×（服务端口径 1.72×），未达到设计文档 §9.2 的「p50 ≥ 10×」；
> 结果集逐条一致（24/24），脚本退出码 0。**
> 直接原因与 B2 同源：2M 行 / 730 MiB 的列在 192 核上无索引全扫只要 63–121 ms，
> 与每查询 25–35 ms 的固定开销同一量级，比值没有可测空间。详见文末偏差分析。

## 1. 环境与构建

| 项 | 值 |
|---|---|
| 分支 | `worktree-regex-ngram-index-design` |
| 运行时 worktree HEAD | `59568a70ee620e65d9918bd1523d256eae2f195a` |
| 集群二进制 | **与 B1–B3 完全相同**：BE `doris_be` 构建于 2026-09-04 20:09:12 +0800，`show backends` / `show frontends` 均报 `doris-0.0.0-74afd0c893f`（B1 基线 `fdd78aa602c` 之后的 3 个提交只涉及 FE 校验、回归用例与文档，未重新部署） |
| 集群 | 验收用 scratch 单机（1 FE + 1 BE），FE query 46031 / http 46030，BE http 46040 |
| BE 配置 | `enable_gram_index_regexp = true` |
| 机器 | 192 核 / 1510 GB 内存；**共享开发机** |
| loadavg（计时相位开始时） | **38.29**（1min，脚本表头 HTML 注释里有原值；B1 39.56 / B2 46.47 / B3 36.47，同一量级） |
| 前置门禁 | 计时相位开始前确认 `pgrep -f run-regression-test` 为空；并额外等到 loadavg(1min) 回落到 ≤ 55（等待 452 s） |
| 客户端连接地板 | 本次实测 **12 ms** |
| 数据目录 | `/mnt/disk15/jiangkai/scratch_gram_cluster` |

## 2. 建表 DDL

tokenizer / analyzer 与 B1、B2 共用 `gb_sparse`，未新建策略：

```sql
-- 已存在，直接复用
CREATE INVERTED INDEX TOKENIZER gb_sparse_tok PROPERTIES (
  "type" = "ngram", "mode" = "sparse",
  "min_gram" = "3", "max_gram" = "16", "density" = "0.25");
CREATE INVERTED INDEX ANALYZER  gb_sparse PROPERTIES ("tokenizer" = "gb_sparse_tok");

CREATE TABLE al (
  observation_id VARCHAR(64),
  event_time     DATETIME,
  app            VARCHAR(32),
  tenant         VARCHAR(32),
  task_category  VARCHAR(64),
  `type`         VARCHAR(32),
  `status`       VARCHAR(16),
  `input`        STRING,
  INDEX idx_input_gram (`input`) USING INVERTED PROPERTIES ("analyzer" = "gb_sparse")
) ENGINE=OLAP
DUPLICATE KEY(observation_id)
DISTRIBUTED BY RANDOM BUCKETS 8
PROPERTIES ("replication_num"="1",
            "inverted_index_storage_format"="SNII",
            "disable_auto_compaction"="true");
```

FE 回填 `"support_phrase" = "false"`（gram 索引强制 docs-only），与 B1/B2/B3 一致。
`disable_auto_compaction=true` 同样是为了保住多 rowset 形态并避免基准中途触发后台索引合并；
代价是索引体积按「未合并的 160 个 segment」计，合并后只会更小。默认压缩（未显式指定）。

## 3. 导入

| 项 | 值 |
|---|---|
| 语料 | `/mnt/disk15/jiangkai/agentlogs_raw/agent_observations_0001.ndjson`（4,105,925,923 B / 998,799 行）+ `agent_observations_0002.ndjson`（4,119,377,574 B / 1,001,626 行）；合计 **8,225,303,497 B（7.66 GiB）/ 2,000,425 行** |
| 为什么用两个文件 | 单文件只有 998,799 行（< 1 M），按验收口径补第二个文件到 2.0 M 行；**未超过 2 个文件** |
| 切分 | `split -C 400M` → 20 块（每文件 10 块，各约 400 MB） |
| 方式 | 6 路并发 stream load 直连 BE 46040，`format:json` + `read_json_by_line:true`，`jsonpaths` 取 8 个字段，`max_filter_ratio:0` |
| 结果 | 20/20 `"Status": "Success"`，`NumberFilteredRows` 全为 0，`NumberUnselectedRows` 全为 0，无 `ErrorURL` |
| **墙钟耗时** | **18 s**（单次 stream load 的 `LoadTimeMs` 3,257–4,939 ms）；吞吐 435.8 MiB/s（JSON 输入口径）、111 K 行/s |
| 落库行数 | `SELECT count(*)` = **2,000,425**（= 998,799 + 1,001,626，逐块行数相加一致） |
| `input` 列 NULL 占比 | 437,862 / 2,000,425 = **21.9 %**（`REASONING` / `EVENT` / `GUARDRAIL` 三类观测天然没有 input；NULL 不参与 REGEXP 匹配，也不产生 posting） |

导入吞吐（435.8 MiB/s）远高于 B1 的 14.3 MB/s，是因为这里的 8.2 GiB 里绝大部分是**未导入的 `payload` 字段**，
真正落库并建索引的只有 8 个字段、其中 `input` 列 765 MB。

## 4. 体积

按 BE 存储目录实际文件统计（8 个 tablet 下 160 个 `.dat` + 160 个 `.idx`），
与 `SHOW TABLE STATUS` 的 `Data_length` / `Index_length` **完全一致**
（注意 BE 上报有滞后：导入刚结束时 `SHOW TABLE STATUS` 仍报 0，需等一轮上报）：

| 量 | 字节 | 换算 |
|---|---:|---|
| 原始列字节 `sum(length(input))` | 764,813,122 | 729.38 MiB |
| 其中非 NULL 行 | 1,562,563 行 | 平均 489.5 B/行 |
| 语料文件字节（两个 ndjson 全文） | 8,225,303,497 | 7.66 GiB |
| 段数据 `.dat`（压缩后，`Data_length`） | 153,065,844 | 145.98 MiB |
| gram 索引 `.idx`（`Index_length`） | 177,431,378 | 169.21 MiB |

| 比值 | 值 | 说明 |
|---|---:|---|
| **索引 / 原始列字节** | **23.2 %** | §4.4/§4.5 建模口径；四张基准表里最好的一张（tb 29.8 %、wb 27.2 %、hl 56.0 %） |
| 索引 / 段数据（`index_disk_size / data_disk_size`） | 115.9 % | §9.2 B4 的字面口径；见 B1 偏差分析 D0 |
| 段数据 / 原始列字节 | 20.0 % | 该语料模板化程度高，列压缩比 **5.00×** |

`SHOW DATA FROM gram_bench.al` 报 `315.187 MB`（= `.dat` + `.idx` 之和，Doris 的 `SHOW DATA` 不拆分索引体积）。

体积比 B1/B2 更省的直接原因：本语料 21.9 % 的行 `input` 为 NULL（不产生任何 posting），
且中英混排下 ASCII 段走稀疏 CDC 采样、CJK 段走码点 1-gram，两种形态的 term 都被 SNII 词典高度共享
（模板化文本里 `app_0NN` / `tenant_0NN` / `phase=…` 等重复子串极多）。

## 5. 逐条正则结果

口径见脚本头注释。要点：

- `t_*` 是 mysql 客户端墙钟中位（5 轮，先各热身 1 轮），含本机实测 **12 ms** 的客户端连接地板；
- `srv_*` 取自 FE profile 的 `Total`（不含客户端连接），是更贴近服务端真实差距的辅助口径；
- `cand_rows` / `gram_filtered` 取自 profile 的 `GramIndexCandidateRows` / `RowsGramIndexFiltered`；
  **`gram_filtered = 0` 且 `cand_rows = 0` 表示该正则被编译成 `ALL`，属于不可索引查询**，不计入 p50；
- **两侧 `count(*)` 逐条相同，24/24 一致，脚本退出码 0（无 MISMATCH）**。

| regex | rows | sel% | t_on_ms | t_off_ms | speedup | srv_on_ms | srv_off_ms | srv_speedup | cand_rows | gram_filtered |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `任务主题` | 75151 | 3.7568 | 50 | 71 | 1.42 | 36.0 | 54.0 | 1.50 | 75151 | 1925274 |
| `上下文主题` | 75151 | 3.7568 | 48 | 70 | 1.46 | 36.0 | 56.0 | 1.56 | 75151 | 1925274 |
| `保留最近一次` | 19343 | 0.9669 | 42 | 65 | 1.55 | 27.0 | 54.0 | 2.00 | 21749 | 1978676 |
| `高信号证据` | 47 | 0.0023 | 35 | 66 | 1.89 | 24.0 | 58.0 | 2.42 | 8977 | 1991448 |
| `反例线索` | 2 | 0.0001 | 31 | 66 | 2.13 | 21.0 | 52.0 | 2.48 | 2 | 2000423 |
| `分类：coding_devops` | 21080 | 1.0538 | 41 | 65 | 1.59 | 28.0 | 52.0 | 1.86 | 21080 | 1979345 |
| `意图：estimate_tokens` | 8996 | 0.4497 | 36 | 63 | 1.75 | 26.0 | 51.0 | 1.96 | 8996 | 1991429 |
| `display_text":"检索"` | 64291 | 3.2139 | 40 | 71 | 1.77 | 28.0 | 64.0 | 2.29 | 64291 | 1936134 |
| `检索.*证据` | 8938 | 0.4468 | 55 | 86 | 1.56 | 40.0 | 67.0 | 1.68 | 8941 | 1991484 |
| `任务主题.*租户` | 75151 | 3.7568 | 67 | 90 | 1.34 | 52.0 | 76.0 | 1.46 | 75151 | 1925274 |
| `rid=[0-9a-f]{4}` | 75151 | 3.7568 | 74 | 86 | 1.16 | 62.0 | 84.0 | 1.35 | 234754 | 1765671 |
| `窗口=[0-9a-f]{4}/[0-9a-f]{4}` | 165 | 0.0082 | 50 | 73 | 1.46 | 36.0 | 62.0 | 1.72 | 9171 | 1991254 |
| `latency_ms=[0-9]{4}` | 58117 | 2.9052 | 102 | 98 | 0.96 | 90.0 | 78.0 | 0.87 | 0 | 0 |
| `tenant_01[0-8]` | 496074 | 24.7984 | 98 | 95 | 0.97 | 88.0 | 83.0 | 0.94 | 1224932 | 775493 |
| `phase=(direct_count_tokens\|direct_generation_input)` | 469362 | 23.4631 | 94 | 99 | 1.05 | 79.0 | 90.0 | 1.14 | 469362 | 1531063 |
| `mcp__matrix__(deploy\|deploy_service)_[0-9]\.md` | 63658 | 3.1822 | 69 | 101 | 1.46 | 54.0 | 82.0 | 1.52 | 63658 | 1936767 |
| `^\{"cwd"` | 752011 | 37.5926 | 82 | 72 | 0.88 | 66.0 | 68.0 | 1.03 | 0 | 0 |
| `replay_[0-9a-f]{4}\.snapshot` | 30080 | 1.5037 | 69 | 91 | 1.32 | 47.0 | 70.0 | 1.49 | 30119 | 1970306 |
| `retry budget depleted` | 30476 | 1.5235 | 45 | 89 | 1.98 | 36.0 | 73.0 | 2.03 | 30581 | 1969844 |
| `unable to open /workspace/artifacts` | 30741 | 1.5367 | 46 | 70 | 1.52 | 30.0 | 58.0 | 1.93 | 30790 | 1969635 |
| `mcp__matrix__vector_search` | 113718 | 5.6847 | 44 | 66 | 1.50 | 28.0 | 61.0 | 2.18 | 113718 | 1886707 |
| `(?i)guardrail` | 34908 | 1.7450 | 81 | 73 | 0.90 | 61.0 | 59.0 | 0.97 | 0 | 0 |
| `(?i)snapshot` | 57402 | 2.8695 | 94 | 89 | 0.95 | 101.0 | 74.0 | 0.73 | 0 | 0 |
| `[0-9]{4}` | 1321638 | 66.0679 | 123 | 121 | 0.98 | 122.0 | 108.0 | 0.89 | 0 | 0 |

不可索引（编译成 `ALL`）的 5 条：
`latency_ms=[0-9]{4}`、`^\{"cwd"`、`(?i)guardrail`、`(?i)snapshot`、`[0-9]{4}`。
其中 `(?i)…` 两条与 `[0-9]{4}` 是**刻意构造**的不可索引查询（§6.3.4：大小写展开后精确集爆炸；纯字符类无确定字面量），
另两条属于「意料之外但可解释」，取证见偏差分析 D4。可索引 **19/24**。

选择率覆盖 **0.0001 %（2 行）到 66 %**，其中 6 条 < 1 %。

## 6. 加速比分位数

| 集合 | n | 墙钟 p25 / p50 / p75 | 服务端 p25 / p50 / p75 |
|---|---:|---|---|
| 全部 24 条 | 24 | 1.03 / **1.46** / 1.57 | 1.11 / 1.54 / 1.97 |
| **可索引（`gram_filtered > 0`）** | 19 | 1.38 / **1.50** / 1.67 | 1.50 / **1.72** / 2.01 |
| 不可索引（编译成 `ALL`） | 5 | 0.90 / 0.95 / 0.96 | 0.87 / 0.89 / 0.97 |
| 可索引且选择率 < 1 % | 6 | — / **1.66** / — | — / **1.98** / — |

## 7. 验收判定（比照设计文档 §9.2 日志表口径）

| 条目 | 通过标准 | 实测 | 判定 |
|---|---|---|---|
| 加速 | 可索引查询 p50 ≥ 10× | 墙钟 1.50×／服务端 1.72× | **FAIL** |
| 语义 | 结果集逐条一致 | 24/24 一致，脚本退出码 0 | **PASS** |
| 体积（建模口径：索引 / 原始列字节） | 日志表 ≤ 30 % | **23.2 %** | **PASS** |
| 体积（字面口径：`index_disk_size / data_disk_size`） | ≤ 30 % | 115.9 % | **FAIL**（口径问题，见 B1 的 D0） |

附带观察：不可索引的 5 条最慢一条是 `^\{"cwd"`（0.88×，服务端 1.03×），即开索引比不开慢 12 %；
其余 4 条在 0.90–0.98× 之间。这部分开销是「编译正则 + 发现无可用 gram + 退回全扫」的固定成本，
落在个位数毫秒量级。本基准没有「宽泛查询不慢于 1.05×」这条标准（那是 B3 的），此处仅作记录。

## 8. 偏差分析

### D0：体积口径不一致（沿用 B1 的结论）

§9.2 B4 写的是 `index_disk_size / data_disk_size`，而 §4.4/§4.5 的全部建模数字分母都是**原始列字节**。
本语料的列压缩比是 5.00×，两个口径差 5 倍，按字面口径永远不可能通过。详见 `e2e_B1_textbench.md` 的 D0。

### D1（主因）：索引在页层面完全达标，端到端比值被固定开销压平

取证（同一条正则、开 / 关索引各跑一次，读 profile）：

| 正则 | 命中行 | 开索引读列字节 / 页数 | 关索引读列字节 / 页数 | 墙钟 |
|---|---:|---|---|---|
| `反例线索` | 2（0.0001 %） | **323.57 KB / 3 页** | 736.02 MB / 11,439 页 | 34 ms vs 75 ms |
| `高信号证据` | 47（0.0023 %） | 410.66 MB / 6,233 页 | 736.02 MB / 11,439 页 | 38 ms vs 68 ms |
| `retry budget depleted` | 30,476（1.52 %） | 689.59 MB / 10,669 页 | 736.02 MB / 11,439 页 | 52 ms vs 88 ms |
| `^\{"cwd"`（编译成 `ALL`） | 752,011（37.6 %） | 736.02 MB / 11,439 页 | 736.02 MB / 11,439 页 | 81 ms vs 76 ms |

三条结论：

1. **最好的一条把列 I/O 降到 1/2,329（页数 1/3,813）、把 2,000,425 行裁到 2 行候选，端到端却只快 2.13×**：
   省下来的 736 MB 解压 + 200 万行正则在这台 192 核机器上只值 41 ms，而每查询固定开销
   （客户端连接 12 ms + FE 解析/规划 + fragment 建立/回收）本身就有 25–35 ms。
   「要省的那部分」和「省不掉的那部分」量级相同，比值上限约 2–3×，与索引质量无关。
   这与 B2 的 D1 是同一个现象，只是表大了 4 倍，比值从 1.22× 提到 1.50×。
2. **候选行散布时页一张也省不掉**：`高信号证据` 只命中 47 行，但候选是 8,977 行（见 D3），
   1024 行一页时这些候选散布在 6,233 / 11,439 页里，列字节只省下 44 %。
   `retry budget depleted` 命中 1.52 %，页数只省 6.7 %。
   这正是 §4.6「加速上界 ≈ 1/命中率」在端到端场景的具体形态，再叠加 I/O 不可省，实际上界更低。
   本基准同样是**随机顺序** + `DISTRIBUTED BY RANDOM`，属最不利情形（§4.7 已预告按内容聚簇会大幅改善）。
3. `ALL` 路径的额外开销确实存在但很小：`^\{"cwd"` 两侧读的字节与页数完全相同，差的 5 ms 就是编译正则 + 发现无可用 gram 的成本。

### D2：无索引基线是 8 tablet × 160 segment 并行 + 全内存

2M 行 / 730 MiB 的列全部驻留 page cache，一次全扫被拆成 8 tablet × 160 segment 并行执行，
无索引全扫中位只有 **63–121 ms**。基线越强，比值越小。

并行度敏感性实验（`SET parallel_pipeline_task_num=1`，即把每查询扫描并行度压到 1，
更接近「集群有并发负载、单查询拿不到整台机器」的真实情形），取 5 条最具选择性的可索引查询：

| 正则 | par=1 加速 | par=8 加速 |
|---|---:|---:|
| `反例线索`（0.0001 %） | **3.83×** | 2.10× |
| `意图：estimate_tokens`（0.45 %） | **2.89×** | 1.89× |
| `高信号证据`（0.0023 %） | **2.78×** | 1.82× |
| `窗口=[0-9a-f]{4}/[0-9a-f]{4}`（0.0082 %） | 2.16× | 1.48× |
| `检索.*证据`（0.45 %） | 2.11× | 1.51× |

同一批查询，仅改并行度，中位加速从 **1.82× 升到 2.78×**（`反例线索` 到 3.83×）。
说明「未达 10×」主要来自**基线过强 + 固定开销**，而不是索引没起作用；
但也要如实指出：即便压到 par=1，本表规模下最好也只有 3.83×，
比 B1（30M 行、par=1 时 25.08×）低一个量级——**表规模仍是决定性因素**。

### D3：候选精度基本没有问题，两条稀有中文短语除外

19 条可索引查询里，**10 条的 `cand_rows` 与真值行数完全相等**（假阳性为 0），
另有 5 条放大 ≤ 1.12×。放大明显的 4 条是：

| 正则 | 真值 | 候选 | 放大 | 原因 |
|---|---:|---:|---:|---|
| `高信号证据` | 47 | 8,977 | 191× | CJK 码点 1-gram，「高/信/号/证/据」各自 df 很高，交集仍有噪声（§4.7「CJK 加 τ 裁剪」的动机） |
| `窗口=[0-9a-f]{4}/[0-9a-f]{4}` | 165 | 9,171 | 55.6× | 同上，确定部分只有「窗口=」三个码点 |
| `rid=[0-9a-f]{4}` | 75,151 | 234,754 | 3.12× | 稀疏采样后只剩短字面量 `rid=`，与 `corr=`/`tid=` 等同族串共享 gram |
| `tenant_01[0-8]` | 496,074 | 1,224,932 | 2.47× | 字面量 `tenant_01` 之外的字符类无法进一步裁剪 |

这四条恰好也是加速比偏低的一批（1.16×–1.89×），说明**候选精度而非索引机制**是它们的瓶颈，
与 §4.5/§4.7 的模型预期一致，不是缺陷。

### D4：5/24 不可索引，其中 2 条是稀疏采样（density = 0.25）跳过短字面量所致

`(?i)guardrail`、`(?i)snapshot`、`[0-9]{4}` 是**刻意构造**的不可索引查询，符合 §6.3.4 的设计。
`latency_ms=[0-9]{4}` 与 `^\{"cwd"` 则是意料之外的两条——把字面量加长后立刻恢复可索引，
证明原因是「稀疏 CDC 采样在这段短字面量里没有取到任何 gram」，而不是编译器漏掉了字面量：

| 探针正则 | rows | cand_rows | gram_filtered |
|---|---:|---:|---:|
| `latency_ms=[0-9]{4}`（11 字符字面量） | 58,117 | 0 | 0 |
| `latency_ms=`（同上，纯字面量） | 122,485 | 0 | 0 |
| `rerank_ms=[0-9]+ latency_ms=`（28 字符） | 122,485 | **124,091** | 1,876,334 |
| `^\{"cwd"`（6 字符字面量） | 752,011 | 0 | 0 |
| `\{"cwd":"/workspace`（19 字符） | 752,011 | **1,217,260** | 783,165 |
| `"cwd":"/workspace/tenant_01`（27 字符） | 305,112 | **752,011** | 1,248,414 |
| `(?i)guardrail` | 34,908 | 0 | 0 |
| `guardrail`（同一字面量，大小写敏感） | 34,908 | **34,917** | 1,965,508 |

注意这不是单纯的「字面量长度阈值」：`.snapshot`（9 字符）与 `replay_`（7 字符）组成的
`replay_[0-9a-f]{4}\.snapshot` 是可索引的，而 11 字符的 `latency_ms=` 不可索引——
**CDC 的切分边界由内容哈希决定，短字面量能否采到 gram 取决于具体字节**，这正是 §4.5 对 p=0.25 的建模含义。
对这类负载，P1 的可选缓解是提高 density、或按 §6.2.2 引入两层结构。

### D5：本基准可能低估的因素

- `disable_auto_compaction=true`，160 个 segment 未合并；合并后 posting 更连续，索引侧还会更快、体积更小。
- 全部数据在本地 page cache。设计的主要收益场景是 **S3 / file cache 冷读**（§6.5）：
  此时无索引路径必须冷读整列 736 MB，索引路径只读 324 KB（见 D1 表），差距是数量级的。
  该场景对应 §9.2 的 B5，本次未测（P0 未做段级布隆）。
- 21.9 % 的行 `input` 为 NULL。这部分行既不产生 posting、也不参与 REGEXP 匹配，
  对两侧一视同仁，但会让「索引能裁掉的比例」看起来更高、让绝对耗时更低。

## 9. 复现命令

```bash
# 0) 语料（4.1 GB × 2，共 2,000,425 行）
wc -l /mnt/disk15/jiangkai/agentlogs_raw/agent_observations_000{1,2}.ndjson
split -C 400M -d --additional-suffix=.json -a 2 \
      /mnt/disk15/jiangkai/agentlogs_raw/agent_observations_0001.ndjson al_chunks/al1_
split -C 400M -d --additional-suffix=.json -a 2 \
      /mnt/disk15/jiangkai/agentlogs_raw/agent_observations_0002.ndjson al_chunks/al2_

# 1) 导入（6 路并发，直连 BE，必须带 Expect:）
curl --location-trusted -u root: -H "Expect:" \
     -H "format:json" -H "read_json_by_line:true" \
     -H 'jsonpaths:["$.observation_id","$.event_time","$.app","$.tenant","$.task_category","$.type","$.status","$.input"]' \
     -H "columns:observation_id,event_time,app,tenant,task_category,type,status,input" \
     -H "max_filter_ratio:0" -T al_chunks/al1_00.json \
     "http://127.0.0.1:46040/api/gram_bench/al/_stream_load"

# 2) 基准（脚本自己会关掉 enable_sql_cache / enable_condition_cache）
tools/regex-ngram-model/e2e/bench_gram_regexp.sh \
    gram_bench.al input tools/regex-ngram-model/q_agentlogs.txt 5 46031

# 3) 体积（SHOW DATA 不拆分索引，用 SHOW TABLE STATUS 或 du 段目录；BE 上报有滞后）
mysql -h127.0.0.1 -P46031 -uroot -e "SHOW TABLE STATUS FROM gram_bench LIKE 'al'\G"
mysql -h127.0.0.1 -P46031 -uroot -e "SELECT sum(length(\`input\`)) FROM gram_bench.al"
find <tablet dirs> -name '*.idx' -printf '%s\n' | awk '{s+=$1} END{print s}'

# 4) 偏差取证（profile 口径）
mysql -h127.0.0.1 -P46031 -uroot -e "
  SET enable_sql_cache=false; SET enable_condition_cache=false;
  SET enable_profile=true; SET profile_level=2;
  SET enable_inverted_index_query=<true|false>;
  SELECT count(*) FROM gram_bench.al WHERE \`input\` REGEXP '<pattern>';"
curl -s -u root: "http://127.0.0.1:46030/rest/v1/query_profile"
curl -s -u root: "http://127.0.0.1:46030/api/profile/text?query_id=<id>"
```

辅助脚本（不入库，留在 `/mnt/disk15/jiangkai/gram_bench/`）：
`42_al_ddl.sh`、`43_al_load.sh`、`44_al_probe.sh`、`45_al_size.sh`、`46_al_sel.sh`、
`47_al_io_evidence.sh`、`48_al_par_sens.sh`、`50_al_unindexable_probe.sh`、`90_stats.py`。
