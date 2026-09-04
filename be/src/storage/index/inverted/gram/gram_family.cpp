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

#include "storage/index/inverted/gram/gram_family.h"

#include "runtime/index_policy/index_policy_mgr.h"
#include "storage/index/inverted/analyzer/analyzer_provider.h"
#include "storage/index/inverted/inverted_index_parser.h"

// 本文件（gram/ 目录下唯一的例外）依赖 runtime/index_policy 与 analyzer 的具体类型，
// 因为"解析 analyzer 名 -> 取得 provider -> 读它的 gram 方案"这条链路本身就是运行时
// 策略解析，无法在不引入这些依赖的情况下完成（R8：gram/ 下其余文件保持对 runtime 的零依赖）。

namespace doris::segment_v2::gram {

std::optional<GramScheme> resolve_gram_scheme(
        const std::map<std::string, std::string>& index_properties, IndexPolicyMgr* mgr) {
    const std::string name = get_analyzer_name_from_properties(index_properties);
    if (name.empty() || mgr == nullptr) {
        return std::nullopt;
    }
    auto provider = mgr->get_analyzer_provider_by_name(name);
    if (provider == nullptr) {
        return std::nullopt;
    }
    return provider->gram_scheme();
}

} // namespace doris::segment_v2::gram
