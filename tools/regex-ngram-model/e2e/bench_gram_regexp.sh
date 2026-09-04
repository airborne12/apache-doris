#!/bin/bash
# ---------------------------------------------------------------------------
# gram（稀疏 / 稠密 ngram）索引对 REGEXP 的端到端加速比基准驱动。
#
# 用法：
#   bench_gram_regexp.sh <db.table> <column> <query_file> [rounds=5] [port=46031]
#
# 参数：
#   db.table    被测表（必须已建好 gram 索引并完成导入）
#   column      被测的正则列
#   query_file  每行一条正则；空行与以 # 开头的行忽略
#   rounds      每种模式的计时轮数，取中位数（默认 5）
#   port        FE query 端口（默认 46031，即验收用的 scratch 集群）
#
# 方法：
#   对每条正则，分别在 enable_inverted_index_query = true / false 两种模式下执行
#   `SELECT count(*) FROM <table> WHERE <col> REGEXP '<pattern>'`：
#     * 每种模式先跑 1 轮热身（结果丢弃，把 OS page cache / BE page cache 预热到
#       稳态，避免冷读偏差污染中位数）；
#     * 再跑 <rounds> 轮计时，取耗时中位数；
#     * 主口径是 mysql 客户端调用的墙钟时间，含约 10~15 ms 的连接开销（表头的
#       client_floor_ms 给出实测地板）。该开销对「开索引」一侧的相对影响更大，
#       因此只会低估加速比，不会高估。
#     * 辅口径 srv_*_ms 取自 FE profile 的 `Total`（不含客户端连接/认证），
#       用来在小表上剥掉客户端地板看真实的服务端差距。
#
#   **必须关掉两个结果级缓存**，否则第二次起的同一条 SQL 会被整条命中而根本不下扫描，
#   两种模式都退化成「≈客户端地板」，加速比恒等于 1：
#     * enable_sql_cache（FE SQL cache，默认 true）——命中后计划变成 PhysicalSqlCache；
#     * enable_condition_cache（谓词结果缓存，默认 true）——命中后 TotalPagesNum=0、
#       RowsRead=0，同一条谓词第二次几乎零成本。
#   两个缓存都以 SQL 文本 / 谓词为键，对开关索引两侧一视同仁，因此不关掉的话
#   测出来的只是缓存命中延迟，而不是索引价值。
#
#   两种模式的 count(*) 必须逐条相同；一旦不同即判定为**正确性缺陷**（索引把真正
#   匹配的行裁掉了），脚本会在 stderr 大声报错并以退出码 3 结束。
#
#   另外各用 enable_profile=true 跑一次，从 FE profile 里取
#   RowsGramIndexFiltered / GramIndexCandidateRows：
#     * gram_filtered == 0 且 cand_rows == 0  ⇒  该正则被 RegexGramCompiler 编译成
#       ALL（无可用字面量 / 大小写不敏感等），属于**不可索引**查询，不计入 p50 加速比；
#     * gram_filtered  > 0  ⇒  索引参与了裁剪，cand_rows 即候选精度指标。
#
# 输出：Markdown 表格。
# ---------------------------------------------------------------------------
set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: $0 <db.table> <column> <query_file> [rounds=5] [port=46031]" >&2
    exit 2
fi

TABLE=$1
COL=$2
QF=$3
ROUNDS=${4:-5}
PORT=${5:-46031}

# 与集群通信的每个 shell 都必须清掉代理变量，否则 curl / mysql 会被劫持
unset HTTP_PROXY HTTPS_PROXY http_proxy https_proxy
MYSQL=(mysql -h127.0.0.1 -P"$PORT" -uroot --batch --raw --skip-column-names)
FE_HTTP=$((PORT - 1))   # 约定：scratch 集群 query=46031 / http=46030
NOCACHE="SET enable_sql_cache=false; SET enable_condition_cache=false;"

median() { sort -n | awk '{a[NR]=$1} END {print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

# 把正则原文转成 SQL 字符串字面量的内容：
#   Doris 与 MySQL 一样，会先在字符串字面量层面吃掉一层反斜杠，所以 \. 要写成 \\.
#   单引号转义成 \'
sql_escape() {
    local s=$1
    s=${s//\\/\\\\}
    s=${s//\'/\\\'}
    printf '%s' "$s"
}

run_count() {   # run_count <mode true|false> <escaped pattern>
    "${MYSQL[@]}" -e "$NOCACHE SET enable_inverted_index_query=$1; SELECT count(*) FROM $TABLE WHERE $COL REGEXP '$2';" | tail -1
}

# profile 里的计数器有两种写法：`Name: 91` 和 `Name: 19.909K (19909)`。
# 有括号时取括号里的精确值，否则取冒号后的整数。
parse_counter() {   # parse_counter <profile text> <counter name>
    local line
    line=$(grep -m1 -- "- $2: " <<<"$1" || true)
    [[ -z "$line" ]] && { printf '%s' '-'; return; }
    if [[ "$line" == *"("*")"* ]]; then
        sed 's/.*(\([0-9]*\)).*/\1/' <<<"$line" | tr -d '\n'
    else
        sed 's/.*: *//; s/[^0-9].*//' <<<"$line" | tr -d '\n'
    fi
}

# Doris 的时间格式：`373ms` / `2sec640ms` / `1min3sec` / `12.3us` / `0ns`
parse_ms() {
    awk '{
        s=$0; ms=0
        if (match(s, /[0-9]+min/))  { ms += substr(s, RSTART, RLENGTH-3) * 60000 }
        if (match(s, /[0-9]+sec/))  { ms += substr(s, RSTART, RLENGTH-3) * 1000 }
        if (match(s, /[0-9.]+ms/))  { ms += substr(s, RSTART, RLENGTH-2) }
        else if (match(s, /[0-9.]+us/)) { ms += substr(s, RSTART, RLENGTH-2) / 1000 }
        else if (match(s, /[0-9.]+ns/)) { ms += substr(s, RSTART, RLENGTH-2) / 1000000 }
        printf "%.1f", ms
    }'
}

# 跑一次带 profile 的查询，回填 PROF_TOTAL_MS / PROF_CAND / PROF_FILT。
# FE 会剥掉 SQL 注释，没法用 tag 定位 profile；但 profile 列表只收录开了 enable_profile
# 的查询，而本脚本是串行的，所以「列表最新的一条且 id 与上一条不同」就是刚跑的这次。
PROF_PREV_QID=""
PROF_TOTAL_MS="-"; PROF_CAND="-"; PROF_FILT="-"
profile_run() {   # profile_run <mode true|false> <escaped pattern>
    PROF_TOTAL_MS="-"; PROF_CAND="-"; PROF_FILT="-"
    "${MYSQL[@]}" -e "$NOCACHE SET enable_profile=true; SET profile_level=2; SET enable_inverted_index_query=$1; SELECT count(*) FROM $TABLE WHERE $COL REGEXP '$2';" >/dev/null
    local i qid prof
    for i in 1 2 3 4 5 6 7 8 9 10; do
        qid=$(curl -s -u root: "http://127.0.0.1:$FE_HTTP/rest/v1/query_profile" 2>/dev/null |
            grep -o '"Profile ID":"[^"]*"' | head -1 | cut -d'"' -f4 || true)
        if [[ -n "$qid" && "$qid" != "$PROF_PREV_QID" ]]; then
            prof=$(curl -s -u root: "http://127.0.0.1:$FE_HTTP/api/profile/text?query_id=$qid" 2>/dev/null || true)
            if grep -q 'RowsGramIndexFiltered' <<<"$prof"; then
                PROF_CAND=$(parse_counter "$prof" GramIndexCandidateRows)
                PROF_FILT=$(parse_counter "$prof" RowsGramIndexFiltered)
                PROF_TOTAL_MS=$(grep -m1 -E '^ *- Total: ' <<<"$prof" | sed 's/.*: *//' | parse_ms)
                PROF_PREV_QID=$qid
                break
            fi
        fi
        sleep 1
    done
}

# 客户端连接开销地板：同样的调用方式跑一条最简单的查询
floor_samples=$(mktemp)
for _ in $(seq 1 5); do
    s=$(date +%s%N); "${MYSQL[@]}" -e "$NOCACHE SELECT 1" >/dev/null; e=$(date +%s%N)
    echo $(((e - s) / 1000000))
done >"$floor_samples"
FLOOR=$(median <"$floor_samples")
rm -f "$floor_samples"

TOTAL=$("${MYSQL[@]}" -e "SELECT count(*) FROM $TABLE" | tail -1)

echo "<!-- table=$TABLE column=$COL query_file=$QF rounds=$ROUNDS total_rows=$TOTAL client_floor_ms=$FLOOR -->"
echo "<!-- loadavg at start: $(cut -d' ' -f1-3 /proc/loadavg) -->"
echo
echo "| regex | rows | sel% | t_on_ms | t_off_ms | speedup | srv_on_ms | srv_off_ms | srv_speedup | cand_rows | gram_filtered |"
echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"

mismatch=0
while IFS= read -r re || [[ -n "$re" ]]; do
    [[ -z "$re" || "$re" == \#* ]] && continue
    esc=$(sql_escape "$re")

    # ---- 热身各一轮，结果同时用于 on/off 行数对照 ----
    rows_on=$(run_count true "$esc")
    rows_off=$(run_count false "$esc")
    if [[ "$rows_on" != "$rows_off" ]]; then
        echo "!!! ROW COUNT MISMATCH for pattern: $re   on=$rows_on off=$rows_off" >&2
        mismatch=1
    fi

    # ---- 计时 ----
    on_samples=$(mktemp); off_samples=$(mktemp)
    for _ in $(seq 1 "$ROUNDS"); do
        s=$(date +%s%N); run_count true "$esc" >/dev/null; e=$(date +%s%N)
        echo $(((e - s) / 1000000))
    done >"$on_samples"
    for _ in $(seq 1 "$ROUNDS"); do
        s=$(date +%s%N); run_count false "$esc" >/dev/null; e=$(date +%s%N)
        echo $(((e - s) / 1000000))
    done >"$off_samples"
    t_on=$(median <"$on_samples"); t_off=$(median <"$off_samples")
    rm -f "$on_samples" "$off_samples"

    # ---- profile：服务端耗时 + 索引是否真的参与了裁剪 ----
    profile_run true "$esc";  srv_on=$PROF_TOTAL_MS;  cand=$PROF_CAND; filt=$PROF_FILT
    profile_run false "$esc"; srv_off=$PROF_TOTAL_MS

    speedup=$(awk -v a="$t_off" -v b="$t_on" 'BEGIN{ if (b+0==0) b=1; printf "%.2f", a/b }')
    srv_speedup=$(awk -v a="$srv_off" -v b="$srv_on" 'BEGIN{ if (a=="-"||b=="-"||b+0==0) {print "-"} else printf "%.2f", a/b }')
    sel=$(awk -v r="$rows_on" -v t="$TOTAL" 'BEGIN{ if (t+0==0) t=1; printf "%.4f", 100.0*r/t }')
    flag=""
    [[ "$rows_on" != "$rows_off" ]] && flag=" **MISMATCH**"
    # 正则里的 `|`（交替）必须转义，否则会被 Markdown 当成表格列分隔符
    re_md=${re//|/\\|}
    printf '| `%s` | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |%s\n' \
        "$re_md" "$rows_on" "$sel" "$t_on" "$t_off" "$speedup" "$srv_on" "$srv_off" "$srv_speedup" "$cand" "$filt" "$flag"
done <"$QF"

if [[ $mismatch -ne 0 ]]; then
    echo >&2
    echo "FAILED: 至少一条正则在开 / 关索引两种模式下返回的行数不同，属于正确性缺陷。" >&2
    exit 3
fi
