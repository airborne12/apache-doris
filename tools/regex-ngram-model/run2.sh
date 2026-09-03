#!/bin/bash
# 第二轮：真实口径（memmem/hs 基线 + 快速复验）与两层体积；含排序后（聚簇）对照与索引期小写折叠
TB=/mnt/disk15/jiangkai/text_bench/textbench_body.txt
TBS=textbench_sorted.txt
WB=/mnt/disk15/jiangkai/weibo/weibo_corpus.txt
WK=/mnt/disk15/jiangkai/wiki_shards/wiki_corpus.txt
HL=/mnt/disk1/jiangkai/httplogs/data/documents-171998.json
run() { name=$1; shift; nohup ./ngram_model2 "$@" > r2_$name.txt 2> r2err_$name.txt & }
run tb_dense3   --corpus $TB --queries q_textbench.txt --n 3 --blocks 64,1024 --bpp 10
run tb_cdc25    --corpus $TB --queries q_textbench.txt --n 3 --cdc --p 0.25 --maxlen 24 --blocks 64,1024 --bpp 10
run tb_cdc33    --corpus $TB --queries q_textbench.txt --n 3 --cdc --p 0.33 --maxlen 24 --blocks 64,1024 --bpp 10
run tb_cdc20    --corpus $TB --queries q_textbench.txt --n 3 --cdc --p 0.20 --maxlen 24 --blocks 64,1024 --bpp 10
run tb_cdc25_lower --corpus $TB --queries q_textbench.txt --n 3 --cdc --p 0.25 --maxlen 24 --lower --blocks 64,1024 --bpp 10
run tb_dense3_lower --corpus $TB --queries q_textbench.txt --n 3 --lower --blocks 64,1024 --bpp 10
run tbs_dense3  --corpus $TBS --queries q_textbench.txt --n 3 --blocks 256,1024,4096 --bpp 10
run tbs_cdc25   --corpus $TBS --queries q_textbench.txt --n 3 --cdc --p 0.25 --maxlen 24 --blocks 256,1024,4096 --bpp 10
run wb_cp2      --corpus $WB --queries q_weibo.txt --n 2 --cp --blocks 64,1024 --bpp 10
run wb_dense3   --corpus $WB --queries q_weibo.txt --n 3 --blocks 64,1024 --bpp 10
run wb_cdc25    --corpus $WB --queries q_weibo.txt --n 3 --cdc --p 0.25 --maxlen 24 --blocks 64,1024 --bpp 10
run wb_cdc25_n6 --corpus $WB --queries q_weibo.txt --n 6 --cdc --p 0.25 --maxlen 24 --blocks 64,1024 --bpp 10
run wk_dense3   --corpus $WK --queries q_wiki.txt --n 3 --max_rows 8000 --blocks 16,64 --bpp 10
run wk_cdc25    --corpus $WK --queries q_wiki.txt --n 3 --cdc --p 0.25 --maxlen 24 --max_rows 8000 --blocks 16,64 --bpp 10
run hl_dense3   --corpus $HL --field request --queries q_httplogs.txt --n 3 --max_rows 3000000 --blocks 64,1024 --bpp 10
run hl_cdc25    --corpus $HL --field request --queries q_httplogs.txt --n 3 --cdc --p 0.25 --maxlen 24 --max_rows 3000000 --blocks 64,1024 --bpp 10
wait
echo ALLDONE > r2.done
