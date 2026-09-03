#!/bin/bash
# 并行跑配置矩阵；每个进程单线程
TB=/mnt/disk15/jiangkai/text_bench/textbench_body.txt
WB=/mnt/disk15/jiangkai/weibo/weibo_corpus.txt
WK=/mnt/disk15/jiangkai/wiki_shards/wiki_corpus.txt
HL=/mnt/disk1/jiangkai/httplogs/data/documents-171998.json
BL="16,64,256,1024,4096"
run() { name=$1; shift; nohup ./ngram_model "$@" > out_$name.txt 2> err_$name.txt & }
# textbench 1M
run tb_dense3      --corpus $TB --queries q_textbench.txt --n 3 --blocks $BL --bpp 8,12
run tb_dense4      --corpus $TB --queries q_textbench.txt --n 4 --blocks $BL --bpp 8,12
run tb_dense3_drop5 --corpus $TB --queries q_textbench.txt --n 3 --blocks 64,1024 --bpp 10 --drop_df 0.05
run tb_cdc25       --corpus $TB --queries q_textbench.txt --n 3 --cdc --p 0.25 --maxlen 24 --blocks $BL --bpp 8,12
run tb_cdc25_m12   --corpus $TB --queries q_textbench.txt --n 3 --cdc --p 0.25 --maxlen 12 --blocks 64,1024 --bpp 10
run tb_cdc33       --corpus $TB --queries q_textbench.txt --n 3 --cdc --p 0.33 --maxlen 24 --blocks 64,1024 --bpp 10
run tb_cdc20       --corpus $TB --queries q_textbench.txt --n 3 --cdc --p 0.20 --maxlen 24 --blocks 64,1024 --bpp 10
run tb_cdc25_n4    --corpus $TB --queries q_textbench.txt --n 4 --cdc --p 0.25 --maxlen 24 --blocks 64,1024 --bpp 10
# weibo 500K
run wb_cp2         --corpus $WB --queries q_weibo.txt --n 2 --cp --blocks $BL --bpp 8,12
run wb_dense3      --corpus $WB --queries q_weibo.txt --n 3 --blocks 64,1024 --bpp 10
run wb_dense4      --corpus $WB --queries q_weibo.txt --n 4 --blocks 64,1024 --bpp 10
run wb_cdc25       --corpus $WB --queries q_weibo.txt --n 3 --cdc --p 0.25 --maxlen 24 --blocks $BL --bpp 8,12
run wb_cdc25_n6    --corpus $WB --queries q_weibo.txt --n 6 --cdc --p 0.25 --maxlen 24 --blocks 64,1024 --bpp 10
# wiki 8000 大文档
run wk_dense3      --corpus $WK --queries q_wiki.txt --n 3 --max_rows 8000 --blocks 16,64,256 --bpp 8,12
run wk_cdc25       --corpus $WK --queries q_wiki.txt --n 3 --cdc --p 0.25 --maxlen 24 --max_rows 8000 --blocks 16,64,256 --bpp 8,12
# httplogs request 字段 3M
run hl_dense3      --corpus $HL --field request --queries q_httplogs.txt --n 3 --max_rows 3000000 --blocks $BL --bpp 8,12
run hl_cdc25       --corpus $HL --field request --queries q_httplogs.txt --n 3 --cdc --p 0.25 --maxlen 24 --max_rows 3000000 --blocks $BL --bpp 8,12
wait
echo ALLDONE > all.done
