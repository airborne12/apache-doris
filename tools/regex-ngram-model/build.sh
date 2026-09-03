#!/bin/bash
clang++ -O2 -std=c++17 -w -I /mnt/disk1/jiangkai/workspace/src/doris-clean/thirdparty/installed/include ngram_model.cpp -L /mnt/disk1/jiangkai/workspace/src/doris-clean/thirdparty/installed/lib -lroaring -lre2 -lhs -lpthread -o ${1:-ngram_model2}
