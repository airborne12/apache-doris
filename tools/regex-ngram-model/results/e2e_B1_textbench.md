# B1 验收基准：textbench 30M 行 OTel 服务日志（稀疏 gram）

> 本文件由 `tools/regex-ngram-model/e2e/bench_gram_regexp.sh` 实测生成，数字未做任何修饰。
> 结论先说：**体积达标（索引 = 原始列字节的 29.8%），可索引查询 p50 加速 3.23×（服务端口径 4.06×），
> 未达到设计文档 §9.2 B1 的「p50 ≥ 10×」；结果集逐条一致（29/29）。** 偏差分析见文末。

## 1. 环境与构建

| 项 | 值 |
|---|---|
| 分支 / HEAD | `worktree-regex-ngram-index-design` / `fdd78aa602cdbe17bb9d60967aba9f3a7976a422` |
| BE 二进制构建时间 | 2026-09-04 20:09:12 +0800（`show backends` 报 `doris-0.0.0-74afd0c893f`） |
| FE jar 构建时间 | 2026-09-04 20:09:09 +0800 |
| 集群 | 验收用 scratch 单机（1 FE + 1 BE），FE query 46031 / http 46030，BE http 46040 |
| BE 配置 | `enable_gram_index_regexp = true` |
| 机器 | 192 核 / 1510 GB 内存；**共享开发机**，跑基准期间 loadavg 1min ≈ 28–54（表头注释里记录了每次运行开始时的实测值） |
| 数据目录 | `/mnt/disk15/jiangkai/scratch_gram_cluster` |

## 2. 建表 DDL

```sql
CREATE INVERTED INDEX TOKENIZER gb_sparse_tok PROPERTIES (
  "type" = "ngram", "mode" = "sparse",
  "min_gram" = "3", "max_gram" = "16", "density" = "0.25");
CREATE INVERTED INDEX ANALYZER  gb_sparse PROPERTIES ("tokenizer" = "gb_sparse_tok");

CREATE TABLE tb (
  id BIGINT,
  msg STRING,
  INDEX idx_msg_gram (msg) USING INVERTED PROPERTIES ("analyzer" = "gb_sparse")
) ENGINE=OLAP
DUPLICATE KEY(id)
DISTRIBUTED BY RANDOM BUCKETS 8
PROPERTIES ("replication_num"="1",
            "inverted_index_storage_format"="SNII",
            "disable_auto_compaction"="true");
```

FE 回填 `"support_phrase" = "false"`（gram 索引强制 docs-only，见 §6.2.1）。
`disable_auto_compaction=true` 与 Task 15 回归用例一致：保住多 rowset 形态，同时避免基准中途触发后台索引合并干扰计时；
代价是索引体积按「未合并的 48 个 segment」计，合并后只会更小。

## 3. 导入

| 项 | 值 |
|---|---|
| 语料 | `/mnt/disk15/jiangkai/text_bench/textbench_body_30m.txt`，30,000,000 行 / 2,533,778,776 字节 |
| 切分 | `split -l 5000000` → 6 块，各约 400 MB |
| 方式 | 6 路并发 stream load 直连 BE 46040，`column_separator=\x01`，`columns:msg, id=cast(0 as bigint)`，`max_filter_ratio:0` |
| 结果 | 6/6 `"Status": "Success"`，`NumberLoadedRows` 各 5,000,000，`NumberFilteredRows` 全为 0 |
| **墙钟耗时** | **177 s**（单次 stream load 的 `LoadTimeMs` 174.6–176.6 s，即 6 路完全并行）；吞吐 14.3 MB/s、169K 行/s |
| 落库行数 | `SELECT count(*)` = 30,000,000 |

## 4. 体积

按 BE 存储目录实际文件统计（8 个 tablet 下 48 个 `.dat` + 48 个 `.idx`），与 `SHOW TABLE STATUS` 的
`Data_length` / `Index_length` 完全一致：

| 量 | 字节 | 换算 |
|---|---:|---|
| 原始列字节 `sum(length(msg))` | 2,503,778,776 | 2.332 GiB |
| 语料文件字节（含换行） | 2,533,778,776 | 2.360 GiB |
| 段数据 `.dat`（压缩后，`Data_length`） | 482,027,711 | 459.7 MiB |
| gram 索引 `.idx`（`Index_length`） | 745,490,696 | 711.0 MiB |

| 比值 | 值 | 说明 |
|---|---:|---|
| **索引 / 原始列字节** | **29.8 %** | 与 §4.5 建模口径同轴（模型对 CDC p=0.25 单层预测 24.1 % + 词典 8.9 % ≈ 33 %） |
| 索引 / 段数据（`index_disk_size / data_disk_size`） | 154.7 % | §9.2 B4 的字面口径；见「偏差分析 D0」 |
| 段数据 / 原始列字节 | 19.3 % | 该语料重复度极高，列压缩比 5.2× |

`SHOW DATA FROM gram_bench.tb` 报 `1.143 GB`（= `.dat` + `.idx` 之和，Doris 的 `SHOW DATA` 不拆分索引体积）。

## 5. 逐条正则结果

口径见脚本头注释。要点：

- `t_*` 是 mysql 客户端墙钟中位（5 轮，先各热身 1 轮），含本机实测 **12 ms** 的客户端连接地板；
- `srv_*` 取自 FE profile 的 `Total`（不含客户端连接），是更贴近服务端真实差距的辅助口径；
- `cand_rows` / `gram_filtered` 取自 profile 的 `GramIndexCandidateRows` / `RowsGramIndexFiltered`；
  **`gram_filtered = 0` 且 `cand_rows = 0` 表示该正则被编译成 `ALL`，属于不可索引查询**，不计入 p50；
- **两侧 `count(*)` 逐条相同，29/29 一致，脚本退出码 0（无 MISMATCH）**。

| regex | rows | sel% | t_on_ms | t_off_ms | speedup | srv_on_ms | srv_off_ms | srv_speedup | cand_rows | gram_filtered |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `error.*timeout` | 1 | 0.0000 | 33 | 387 | 11.73 | 24.0 | 322.0 | 13.42 | 4 | 29999996 |
| `rpc error: code = Unavailable` | 634794 | 2.1160 | 60 | 137 | 2.28 | 40.0 | 136.0 | 3.40 | 634794 | 29365206 |
| `user_id="[0-9a-f]{8}-` | 0 | 0.0000 | 53 | 257 | 4.85 | 42.0 | 273.0 | 6.50 | 502932 | 29497068 |
| `failed to (convert\|charge)` | 3007106 | 10.0237 | 103 | 255 | 2.48 | 90.0 | 252.0 | 2.80 | 3007622 | 26992378 |
| `\[PlaceOrder\]` | 502932 | 1.6764 | 51 | 228 | 4.47 | 37.0 | 225.0 | 6.08 | 502932 | 29497068 |
| `10\.68\.[0-9]+\.[0-9]+:[0-9]+` | 105457 | 0.3515 | 280 | 254 | 0.91 | 268.0 | 270.0 | 1.01 | 0 | 0 |
| `code = (Unavailable\|Internal\|DeadlineExceeded)` | 634801 | 2.1160 | 119 | 251 | 2.11 | 98.0 | 254.0 | 2.59 | 3448898 | 26551102 |
| `severity":"(warn\|error)"` | 0 | 0.0000 | 31 | 258 | 8.32 | 19.0 | 253.0 | 13.32 | 0 | 30000000 |
| `opentelemetry-api-1\.[0-9]+\.[0-9]+` | 375010 | 1.2500 | 54 | 253 | 4.69 | 41.0 | 293.0 | 7.15 | 375010 | 29624990 |
| `^\{"message"` | 810095 | 2.7003 | 88 | 204 | 2.32 | 75.0 | 210.0 | 2.80 | 2113731 | 27886269 |
| `Sending Quote: [0-9]+\.[0-9]+` | 389750 | 1.2992 | 55 | 257 | 4.67 | 42.0 | 242.0 | 5.76 | 389750 | 29610250 |
| `cart\.cartstore\.ValkeyCartStore` | 4312587 | 14.3753 | 93 | 251 | 2.70 | 78.0 | 236.0 | 3.03 | 4312587 | 25687413 |
| `(?i)unavailable` | 639880 | 2.1329 | 251 | 216 | 0.86 | 203.0 | 217.0 | 1.07 | 0 | 0 |
| `[0-9]{3}-[0-9]{4}` | 741884 | 2.4729 | 421 | 407 | 0.97 | 373.0 | 358.0 | 0.96 | 0 | 0 |
| `[a-z]+@[a-z]+\.com` | 20406 | 0.0680 | 278 | 246 | 0.88 | 265.0 | 228.0 | 0.86 | 0 | 0 |
| `conn(ection)? re(set\|fused)` | 621456 | 2.0715 | 105 | 253 | 2.41 | 87.0 | 292.0 | 3.36 | 2059262 | 27940738 |
| `GET\|POST` | 489024 | 1.6301 | 265 | 287 | 1.08 | 250.0 | 241.0 | 0.96 | 0 | 0 |
| `trace\.rb:[0-9]+:in` | 150004 | 0.5000 | 115 | 275 | 2.39 | 108.0 | 235.0 | 2.18 | 2741799 | 27258201 |
| `desc = error reading from server` | 567210 | 1.8907 | 57 | 147 | 2.58 | 40.0 | 139.0 | 3.48 | 567210 | 29432790 |
| `currency: failed to convert currency` | 634692 | 2.1156 | 54 | 149 | 2.76 | 46.0 | 112.0 | 2.43 | 634692 | 29365308 |
| `Convert conversion successful` | 488863 | 1.6295 | 44 | 142 | 3.23 | 31.0 | 126.0 | 4.06 | 488863 | 29511137 |
| `\[INFO\] Sending Quote` | 389750 | 1.2992 | 44 | 200 | 4.55 | 32.0 | 177.0 | 5.53 | 389750 | 29610250 |
| `transport: Error while dialing` | 67483 | 0.2249 | 36 | 172 | 4.78 | 25.0 | 131.0 | 5.24 | 67483 | 29932517 |
| `context deadline exceeded` | 73 | 0.0002 | 23 | 133 | 5.78 | 13.0 | 137.0 | 10.54 | 73 | 29999927 |
| `product_id=[A-Z0-9]{10}` | 0 | 0.0000 | 29 | 232 | 8.00 | 18.0 | 244.0 | 13.56 | 0 | 30000000 |
| `quantity=[0-9]+` | 1500890 | 5.0030 | 120 | 295 | 2.46 | 114.0 | 297.0 | 2.61 | 2960766 | 27039234 |
| `gems/(grpc\|opentelemetry)-[a-z]*-?[0-9]` | 375010 | 1.2500 | 334 | 338 | 1.01 | 302.0 | 286.0 | 0.95 | 0 | 0 |
| `Unavailable desc = error` | 567245 | 1.8908 | 51 | 132 | 2.59 | 39.0 | 138.0 | 3.54 | 567245 | 29432755 |
| `(?s)Sending.*Quote` | 389750 | 1.2992 | 57 | 252 | 4.42 | 37.0 | 300.0 | 8.11 | 389750 | 29610250 |

不可索引（编译成 `ALL`）的 6 条：
`10\.68\.[0-9]+\.[0-9]+:[0-9]+`、`(?i)unavailable`、`[0-9]{3}-[0-9]{4}`、`[a-z]+@[a-z]+\.com`、
`GET|POST`、`gems/(grpc|opentelemetry)-[a-z]*-?[0-9]`。
可索引 23/29，与 §4.5 模型对 CDC p=0.25 的预测（23/29）完全吻合。

## 6. 加速比分位数

| 集合 | n | 墙钟 p25 / p50 / p75 | 服务端 p25 / p50 / p75 |
|---|---:|---|---|
| 全部 29 条 | 29 | 2.28 / **2.59** / 4.67 | 2.43 / 3.40 / 6.08 |
| **可索引（`gram_filtered > 0`）** | 23 | 2.47 / **3.23** / 4.74 | 2.92 / **4.06** / 6.83 |
| 不可索引（编译成 `ALL`） | 6 | 0.89 / 0.94 / 1.00 | 0.95 / 0.96 / 1.00 |
| 可索引且选择率 < 1 % | 7 | — / **5.78** / — | — / **10.54** / — |

## 7. 验收判定（设计文档 §9.2）

| 条目 | 通过标准 | 实测 | 判定 |
|---|---|---|---|
| B1 加速 | 可索引查询 p50 ≥ 10× | 墙钟 3.23×／服务端 4.06× | **FAIL** |
| B1 语义 | 结果集逐条一致 | 29/29 一致 | **PASS** |
| B4 体积（建模口径：索引 / 原始列字节） | 日志表 ≤ 30 % | 29.8 % | **PASS** |
| B4 体积（字面口径：`index_disk_size / data_disk_size`） | ≤ 30 % | 154.7 % | **FAIL**（口径问题，见 D0） |

附带观察：不可索引的 6 条最慢一条是 `(?i)unavailable`（0.86×，服务端 1.07×），即开索引比不开慢 14 %；
其余 5 条在 0.88–1.08× 之间。这部分开销是「编译正则 + 发现无可用 gram + 退回全扫」的固定成本，
落在个位数毫秒量级。B1 没有「宽泛查询不慢于 1.05×」这条标准（那是 B3 的），此处仅作记录。

## 8. 偏差分析

### D0：体积口径不一致（文档问题，不是实现问题）

§9.2 B4 写的是 `index_disk_size / data_disk_size`，但 §4.4/§4.5 的全部建模数字（57 %、85 %、24.8 %、27.1 %…）
分母都是**原始列字节**（`tools/regex-ngram-model/ngram_model.cpp` 的 `data_bytes`），不是 Doris 压缩后的段数据。
本语料的列压缩比是 5.2×，两个口径差 5 倍以上，B4 按字面口径永远不可能通过。
**结论：B4 的分母应写成「原始列字节（未压缩）」，或者把阈值按压缩比重新标定。** 已在设计文档 §9.2 注明。
按建模口径，P0 实测 29.8 % 落在预算内，且优于模型对「单层（无 §6.2.2 两层结构）」的预测（≈33 %）。

### D1：索引在行 / 页层面完全达标，端到端比值被固定开销压平

取证（同一条正则、开 / 关索引各跑一次，读 profile；完整数据见 §9）：

| 正则 | 命中行 | 开索引读列字节 / 页数 | 关索引读列字节 / 页数 | 墙钟 |
|---|---:|---|---|---|
| `error.*timeout` | 1 | **128 KB / 2 页** | 2.43 GB / 39,742 页 | 40 ms vs 380 ms |
| `context deadline exceeded` | 73 | **4.38 MB / 70 页** | 2.43 GB / 39,742 页 | 25 ms vs 154 ms |
| `\[PlaceOrder\]` | 502,932（1.68 %） | **2.43 GB / 39,739 页** | 2.43 GB / 39,742 页 | 55 ms vs 231 ms |
| `rpc error: code = Unavailable` | 634,794（2.12 %） | 2.31 GB / 37,750 页 | 2.43 GB / 39,742 页 | 73 ms vs 137 ms |

两条独立的天花板：

1. **每查询固定开销 ≈ 25–35 ms**（客户端连接 12 ms + FE 解析/规划 + fragment 建立）。
   即使索引把 30M 行裁到 1 行、列 I/O 降到 1/19000，墙钟也只能到 33 ms，对 387 ms 的基线就是 11.7×——
   这已经是本机能拿到的**上限**。要 p50 ≥ 10×，需要 p50 那条查询的无索引基线 ≥ 250 ms 且索引侧接近空扫，
   而实际 p50 查询的基线只有约 250 ms、索引侧却要复验 50 万行。
2. **选择率 ≥ 1 % 时候选行是散布的，页一张都省不掉**。`\[PlaceOrder\]` 命中 1.68 %：
   1024 行一页时几乎每页都含候选，开索引读的列字节与关索引**完全相同**（2.43 GB）。
   此时 gram 索引只省下「对 29.5M 行跑正则」的 CPU，不省列 I/O 与解压——这正是 §4.6
   「加速上界 ≈ 1/命中率，与索引实现无关」在端到端场景里的具体形态，再叠加 I/O 不可省，
   实际上界还要更低。§4.7 已指出：把数据按内容聚簇后候选会集中到少数页，体积和加速都会大幅改善；
   本基准的 textbench 语料是**随机顺序**、且建表用 `DISTRIBUTED BY RANDOM` + 常量排序键，属于最不利情形。

### D2：无索引基线是 48 路并行 + 全内存，比建模时的单线程基线强一个数量级

§4.6 的 6.1×/9.3× 是 CPU-only 单线程口径（memmem/hs 基线 ~40 ms 扫 100 万行）。
本机 192 核、语料 2.4 GB 全部驻留 page cache，一次全扫被拆成 8 tablet × 48 segment 并行执行，
30M 行的无索引全扫中位只要 **250 ms（≈120M 行/s）**。基线越强，比值越小。

并行度敏感性实验（`SET parallel_pipeline_task_num=1`，即把每查询扫描并行度压到 1，
更接近「集群有并发负载、单查询拿不到整台机器」的真实情形）：

| 正则 | par=1 加速 | par=8 加速 |
|---|---:|---:|
| `context deadline exceeded`（0.0002 %） | **25.08×** | 5.71× |
| `\[PlaceOrder\]`（1.68 %） | **8.43×** | 4.36× |
| `transport: Error while dialing`（0.22 %） | **8.15×** | 4.17× |
| `Sending Quote: [0-9]+\.[0-9]+`（1.30 %） | **7.33×** | 4.84× |
| `Convert conversion successful`（1.63 %） | 5.14× | 3.05× |
| `rpc error: code = Unavailable`（2.12 %） | 3.90× | 2.50× |

同一批查询，仅改并行度，中位加速从 4.3× 升到 7.7×，高选择性查询直接到 25×。
说明「未达 10×」主要来自**基线过强 + 固定开销**，而不是索引没起作用。

### D3：候选精度本身没有问题

23 条可索引查询里，**15 条的 `cand_rows` 与真值行数完全相等**（假阳性为 0），其余 8 条的放大倍数是：

| 正则 | 真值 | 候选 | 放大 |
|---|---:|---:|---:|
| `error.*timeout` | 1 | 4 | 4×（绝对量可忽略） |
| `code = (Unavailable\|Internal\|DeadlineExceeded)` | 634,801 | 3,448,898 | 5.4× |
| `^\{"message"` | 810,095 | 2,113,731 | 2.6× |
| `conn(ection)? re(set\|fused)` | 621,456 | 2,059,262 | 3.3× |
| `trace\.rb:[0-9]+:in` | 150,004 | 2,741,799 | 18× |
| `quantity=[0-9]+` | 1,500,890 | 2,960,766 | 2.0× |
| `user_id="[0-9a-f]{8}-` | 0 | 502,932 | —（真值 0） |
| `severity":"(warn\|error)"` | 0 | 0 | —（整段裁光） |

放大的原因都一样：编译后只剩一个短字面量（`cod` / `essag` / `" re"` / `trace` / `tity=`），
这与 §4.5 的模型一致（p=0.25 下短字面量会被稀疏采样跳过），不是缺陷。
放大的这几条恰好也是加速比最低的（2.1×–2.5×），说明**候选精度而非索引机制**是这批查询的瓶颈。

### D4：本基准可能低估的因素

- 关闭了 `disable_auto_compaction`，48 个 segment 未合并；合并后 posting 更连续，索引侧还会更快、体积更小。
- 全部数据在本地 page cache。设计的主要收益场景是 **S3 / file cache 冷读**（§6.5）：
  此时无索引路径必须冷读整列 2.43 GB，索引路径只读 128 KB–4 MB（见 D1 表），差距是数量级的。
  该场景对应 §9.2 的 B5，本次未测（P0 未做段级布隆）。

## 9. 复现命令

```bash
# 建库 / 策略 / 建表 / 导入（脚本见 /mnt/disk15/jiangkai/gram_bench/）
split -l 5000000 -d --additional-suffix=.txt textbench_body_30m.txt tb_
curl --location-trusted -u root: -H "Expect:" -H "column_separator:\x01" \
     -H "columns:msg, id=cast(0 as bigint)" -H "max_filter_ratio:0" \
     -T tb_00.txt "http://127.0.0.1:46040/api/gram_bench/tb/_stream_load"

# 基准
tools/regex-ngram-model/e2e/bench_gram_regexp.sh \
    gram_bench.tb msg tools/regex-ngram-model/q_textbench.txt 5 46031

# 体积（SHOW DATA 不拆分索引，用 SHOW TABLE STATUS 或 du 段目录）
mysql -h127.0.0.1 -P46031 -uroot -e "SHOW TABLE STATUS FROM gram_bench LIKE 'tb'\G"
```
