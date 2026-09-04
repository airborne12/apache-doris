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

#pragma once

#include <map>
#include <optional>
#include <string>

#include "storage/index/inverted/gram/gram_scheme.h"

namespace doris {
class IndexPolicyMgr;
}

namespace doris::segment_v2::gram {

// 从索引属性解析 analyzer/normalizer 名 -> 策略 -> gram 方案，是写入侧（SNII writer）判断
// 一个索引是否属于"gram 族"（tokenizer 为 ngram 且携带 mode 属性）、以及取得其 GramScheme
// 参数的唯一入口。内置 parser、非 gram 族 tokenizer、找不到策略或策略管理器未就绪时均
// 返回 std::nullopt，调用方按"不是 gram 族"处理即可，无需区分具体原因。
std::optional<GramScheme> resolve_gram_scheme(
        const std::map<std::string, std::string>& index_properties, IndexPolicyMgr* mgr);

} // namespace doris::segment_v2::gram
