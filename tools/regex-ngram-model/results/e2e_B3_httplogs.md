# B3 验收基准：httplogs 34.46M 行 `request` 列（稠密 gram）

> 本文件由 `tools/regex-ngram-model/e2e/bench_gram_regexp.sh` 实测生成，数字未做任何修饰。
> 结论先说：**15/15 全部可索引，结果集逐条一致；「宽泛查询不慢于无索引 ×1.05」达标（最慢的一条也快 1.20×）；
> 但「选择率 < 1 % 的查询 ≥ 10×」未达标（墙钟 p50 4.62×／服务端 6.10×）。
> 体积 56.0 %（索引 / 原始列字节），超出 §9.2 B4 对稠密 URL 表的 ≤ 45 % 预算——
> 根因已定位：`stop_gram_df`（τ）在 P0 只被解析与落盘，写入侧尚未实现高频 gram 裁剪。**

## 1. 环境与构建

同 B1（`e2e_B1_textbench.md` §1）。本次运行开始时 loadavg 1min = **36.47**。

## 2. 建表 DDL

```sql
CREATE INVERTED INDEX TOKENIZER gb_dense_tok PROPERTIES (
  "type" = "ngram", "mode" = "dense", "min_gram" = "3", "stop_gram_df" = "0.25");
CREATE INVERTED INDEX ANALYZER  gb_dense PROPERTIES ("tokenizer" = "gb_dense_tok");

CREATE TABLE hl (
  ts DATETIME,
  clientip VARCHAR(64),
  status INT,
  sz INT,
  request STRING,
  INDEX idx_req_gram (request) USING INVERTED PROPERTIES ("analyzer" = "gb_dense")
) ENGINE=OLAP
DUPLICATE KEY(ts)
DISTRIBUTED BY RANDOM BUCKETS 8
PROPERTIES ("replication_num"="1",
            "inverted_index_storage_format"="SNII",
            "disable_auto_compaction"="true");
```

按 §4.8「低熵短串（URL、路径、枚举型）→ `mode=dense, n=3, τ=0.25`」选型。
`DUPLICATE KEY(ts)` 让数据按时间聚簇（httplogs 本身就是时序日志），这是该负载的自然形态。

## 3. 导入

| 项 | 值 |
|---|---|
| 语料 | `documents-171998.json` … `documents-201998.json` 共 4 个文件，4,612,845,015 字节 |
| 字段 | `{"@timestamp": <epoch int>, "clientip", "request", "status", "size"}`，逐行 JSON |
| 切分 | `split -C 400M` → 13 块 |
| 方式 | 6 路并发 stream load 直连 BE 46040，`format=json`、`read_json_by_line=true`，`max_filter_ratio:0`，<br>`jsonpaths:["$.@timestamp","$.clientip","$.request","$.status","$.size"]`，<br>`columns:ts_raw,clientip,request,status,sz,ts=from_unixtime(ts_raw)` |
| 结果 | 13/13 `"Status": "Success"`，`NumberFilteredRows` 全为 0 |
| **墙钟耗时** | **215 s**（含 6 路并发）；吞吐 21.4 MB/s JSON、160K 行/s |
| 落库行数 | **34,460,091**（4 个文件全部导入，未提前截断） |

## 4. 体积

| 量 | 字节 | 换算 |
|---|---:|---|
| 原始列字节 `sum(length(request))` | 1,366,647,115 | 1.273 GiB |
| 段数据 `.dat`（全部 5 列，`Data_length`） | 223,987,531 | 213.6 MiB |
| gram 索引 `.idx`（`Index_length`） | 765,052,497 | 729.6 MiB |

| 比值 | 值 | 说明 |
|---|---:|---|
| **索引 / 原始 `request` 列字节** | **56.0 %** | §9.2 B4 对 URL 表稠密模式的预算是 ≤ 45 % |
| 索引 / 段数据 | 341.6 % | §9.2 B4 的字面口径，见 B1 的偏差分析 D0 |
| 段数据 / 原始 `request` 列字节 | 16.4 % | URL 重复度极高，且 `.dat` 还含另外 4 列 |

## 5. 逐条正则结果

口径同 B1。客户端连接地板本次实测 **11 ms**。
**两侧 `count(*)` 逐条相同，15/15 一致，脚本退出码 0（无 MISMATCH）；15/15 全部可索引。**

| regex | rows | sel% | t_on_ms | t_off_ms | speedup | srv_on_ms | srv_off_ms | srv_speedup | cand_rows | gram_filtered |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `^GET /images/.*\.gif` | 15232112 | 44.2022 | 195 | 344 | 1.76 | 179.0 | 338.0 | 1.89 | 15232136 | 19227955 |
| `/french/competition/` | 55685 | 0.1616 | 28 | 106 | 3.79 | 19.0 | 90.0 | 4.74 | 55685 | 34404406 |
| `flashed_stage[0-9]` | 68506 | 0.1988 | 41 | 189 | 4.61 | 29.0 | 196.0 | 6.76 | 68506 | 34391585 |
| `\.(jpg\|jpeg) HTTP` | 2393301 | 6.9451 | 69 | 227 | 3.29 | 54.0 | 201.0 | 3.72 | 2393301 | 32066790 |
| `HTTP/1\.0$` | 28147531 | 81.6815 | 351 | 447 | 1.27 | 328.0 | 425.0 | 1.30 | 28147532 | 6312559 |
| `/english/(index\|venues)` | 861344 | 2.4995 | 50 | 267 | 5.34 | 39.0 | 260.0 | 6.67 | 861352 | 33598739 |
| `tck_pkit_fx_[a-z]` | 24485 | 0.0711 | 47 | 217 | 4.62 | 39.0 | 188.0 | 4.82 | 24485 | 34435606 |
| `stage[12]\.htm` | 135199 | 0.3923 | 39 | 225 | 5.77 | 28.0 | 193.0 | 6.89 | 135199 | 34324892 |
| `/images/[0-9]+\.gif` | 1144092 | 3.3200 | 332 | 397 | 1.20 | 326.0 | 401.0 | 1.23 | 26544630 | 7915461 |
| `^POST ` | 49680 | 0.1442 | 29 | 86 | 2.97 | 18.0 | 74.0 | 4.11 | 49680 | 34410411 |
| `/(french\|english)/frntpage\.htm` | 132933 | 0.3858 | 43 | 211 | 4.91 | 29.0 | 177.0 | 6.10 | 132933 | 34327158 |
| `nav_(top\|bottom)_inet\.html` | 348298 | 1.0107 | 42 | 211 | 5.02 | 32.0 | 259.0 | 8.09 | 348298 | 34111793 |
| `[0-9]{5}\.jpg` | 778980 | 2.2605 | 70 | 275 | 3.93 | 57.0 | 265.0 | 4.65 | 2393170 | 32066921 |
| `/images/s102[0-9]+\.gif` | 3426994 | 9.9448 | 78 | 346 | 4.44 | 73.0 | 407.0 | 5.58 | 3426995 | 31033096 |
| `hm_(nbg\|bg)\.gif` | 0 | 0.0000 | 34 | 211 | 6.21 | 24.0 | 194.0 | 8.08 | 0 | 34460091 |

## 6. 加速比分位数

| 集合 | n | 墙钟 p25 / p50 / p75 | 服务端 p25 / p50 / p75 |
|---|---:|---|---|
| 全部 15 条（= 全部可索引） | 15 | 3.13 / **4.44** / 4.96 | 3.92 / **4.82** / 6.71 |
| **选择率 < 1 %（验收关注）** | 7 | — / **4.62** / — | — / **6.10** / — |
| 选择率 ≥ 1 %（宽泛） | 8 | — / 3.61 / — | — / — / — |
| 宽泛查询最慢的一条 | — | **1.20×**（`/images/[0-9]+\.gif`，3.32 %） | 1.23× |

选择率 < 1 % 的 7 条逐条加速（墙钟）：`^POST ` 2.97×、`/french/competition/` 3.79×、
`flashed_stage[0-9]` 4.61×、`tck_pkit_fx_[a-z]` 4.62×、`/(french|english)/frntpage\.htm` 4.91×、
`stage[12]\.htm` 5.77×、`hm_(nbg|bg)\.gif`（0 命中）6.21×。

## 7. 验收判定（设计文档 §9.2）

| 条目 | 通过标准 | 实测 | 判定 |
|---|---|---|---|
| B3 选择性查询 | 选择率 < 1 % 的查询 ≥ 10× | 墙钟 p50 4.62×／服务端 p50 6.10×，最好一条 6.21× | **FAIL** |
| B3 宽泛查询 | 不慢于无索引 ×1.05（即加速 ≥ 0.952×） | 15 条里最慢的也有 **1.20×**，无一条变慢 | **PASS** |
| B3 语义 | 结果集逐条一致 | 15/15 一致 | **PASS** |
| B4 体积（建模口径：索引 / 原始列字节） | URL 表稠密 ≤ 45 % | 56.0 % | **FAIL**（根因见 D0） |
| B4 体积（字面口径：`index_disk_size / data_disk_size`） | ≤ 45 % | 341.6 % | **FAIL**（口径问题，见 B1 的 D0） |

## 8. 偏差分析

### D0（体积超预算，可定位到具体缺失功能）：`stop_gram_df` 在 P0 是空实现

§4.5 给 httplogs 的两个数字是：**稠密 3-gram 无 τ = 57 %**、**稠密 3-gram + τ=0.25 = 41.1 %**。
本次建表写了 `"stop_gram_df" = "0.25"`，实测 **56.0 %**——精确落在「无 τ」那一档。

代码核对（HEAD `fdd78aa602c`）：`stop_gram_df` 被 `gram_scheme.cpp:79` 解析成 `stop_df_permille`，
经 `core_metadata.cpp:44` 写进段级 `SniiCoreMetadataPB`，并参与 `GramScheme` 的相等性判定；
但**全代码库没有任何地方读它来裁剪高频 gram**（`grep -rn stop_df_permille be/src/` 只有上述解析 /
落盘 / 比较三处）。也就是说 P0 的 τ 是「已定义、已持久化、未生效」。

因此这一项**不是模型失准，而是 P0 未实现的功能**：τ 生效后按模型预期可从 56 % 降到约 41 %，
仍略高于 45 % 的预算边界之下，属于可达标范围。已在设计文档 §6.1.4 / §9.2 标注为 P0 未落地项。

### D1（加速未达 10×）：与 B1 同源——固定开销 + 候选散布，见 `e2e_B1_textbench.md` §8 D1/D2

取证（同一条正则开 / 关索引各跑一次读 profile）：

| 正则 | 命中行 | 开索引读列字节 / 页数 | 关索引读列字节 / 页数 | 墙钟 |
|---|---:|---|---|---|
| `tck_pkit_fx_[a-z]` | 24,485（0.071 %） | **193.89 MB / 2,864 页** | 277.02 MB / 4,225 页 | 51 ms vs 236 ms |
| `stage[12]\.htm` | 135,199（0.39 %） | 275.95 MB / 4,200 页 | 277.02 MB / 4,225 页 | 43 ms vs 290 ms |

即使命中率只有 **0.071 %**，开索引仍要读 70 % 的页——因为 34.46M 行按 `ts` 聚簇，
而 `tck_pkit_fx_*` 这类资源名在整个时间段上均匀出现，候选行散布在 68 % 的页里。
命中 0.39 % 的 `stage[12]\.htm` 更是 99.4 % 的页都要读。
**低熵短串负载的候选虽然精确（15 条里 9 条 `cand_rows` 与真值完全相等、12 条放大 ≤ 1.05×），却在页级不可分离**，
这是 §4.6「加速上界 ≈ 1/命中率」在端到端场景的更严格版本：上界还要再乘上「含候选页的比例」。

固定开销同样在起作用：选择率最低的 `tck_pkit_fx_[a-z]` 开索引侧服务端 39 ms，
其中约 20 ms 是解析/规划/fragment 建立；把这部分扣掉，比值从 4.82× 变成 ≈9.9×。

### D2：宽泛查询这一项是明确的正面结果

设计只要求宽泛查询「不慢于 1.05×」，实测**全部变快**：

- `HTTP/1\.0$`（81.7 % 命中）：1.27×——索引仍裁掉 6,312,559 行（18 %）；
- `^GET /images/.*\.gif`（44.2 %）：1.76×——裁掉 19,227,955 行（56 %）；
- `/images/[0-9]+\.gif`（3.32 %）：1.20×——这条候选放大明显（候选 26,544,630 / 真值 1,144,092，
  放大 23×），因为编译后只剩 `/images/` 与 `.gif` 这类超高频 gram；即便如此仍未变慢。

说明「近似候选 + 复验」这条路径在候选放大到 77 % 全表时也不会退化成负收益，
与 §6.4.3 的成本驱动目标一致（虽然 P0 还没有 `regex_gram_index_max_candidate_ratio` 这个 P1 阈值）。

### D3：本基准的取舍

- 按任务范围只导入 4 个文件（34.46M 行），不是设计文档 §9.2 写的 247M 行；行数少一个数量级，
  无索引基线相应更小（86–447 ms），比值天花板更低。
- `disable_auto_compaction=true`，104 个 segment 未合并。
- 全部数据在本地 page cache；S3 冷读场景（§6.5 / B5）未测。

## 9. 复现命令

```bash
split -C 400M -d --additional-suffix=.json documents-171998.json hl_
curl --location-trusted -u root: -H "Expect:" -H "format:json" -H "read_json_by_line:true" \
  -H 'jsonpaths:["$.@timestamp","$.clientip","$.request","$.status","$.size"]' \
  -H "columns:ts_raw,clientip,request,status,sz,ts=from_unixtime(ts_raw)" \
  -H "max_filter_ratio:0" -T hl_00.json \
  "http://127.0.0.1:46040/api/gram_bench/hl/_stream_load"

tools/regex-ngram-model/e2e/bench_gram_regexp.sh \
    gram_bench.hl request tools/regex-ngram-model/q_httplogs.txt 5 46031
```
