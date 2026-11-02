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

#include "storage/index/inverted/analysis/analyzer/ik/core/CJKSegmenter.h"

namespace doris::segment_v2 {

CJKSegmenter::CJKSegmenter() = default;

void CJKSegmenter::analyze(AnalyzeContext& context) {
    if (context.getCurrentCharType() != CharacterUtil::CharType::USELESS) {
        // Prioritize processing the prefixes that have already been matched
        if (!tmp_hits_.empty()) {
            size_t kept = 0;
            for (size_t i = 0; i < tmp_hits_.size(); ++i) {
                Hit& hit = tmp_hits_[i];
                Dictionary::getSingleton()->matchWithHit(context.getTypedRuneArray(),
                                                         context.getCursor(), hit);
                if (hit.isMatch()) {
                    Lexeme newLexeme(context.getBufferOffset(), hit.getByteBegin(),
                                     hit.getByteEnd() - hit.getByteBegin(), Lexeme::Type::CNWord,
                                     hit.getCharBegin(), hit.getCharEnd());
                    context.addLexeme(newLexeme);

                    if (hit.isPrefix()) {
                        if (kept != i) {
                            tmp_hits_[kept] = hit;
                        }
                        ++kept;
                    }
                } else if (!hit.isUnmatch()) {
                    if (kept != i) {
                        tmp_hits_[kept] = hit;
                    }
                    ++kept;
                }
            }
            tmp_hits_.resize(kept);
        }
        // Perform a single-character match at the current pointer position.
        auto singleCharHit = Dictionary::getSingleton()->matchInMainDict(
                context.getTypedRuneArray(), context.getCursor(), 1);
        if (singleCharHit.isMatch()) {
            Lexeme newLexeme(context.getBufferOffset(), context.getCurrentCharOffset(),
                             context.getCurrentCharLen(), Lexeme::Type::CNChar,
                             singleCharHit.getCharBegin(), singleCharHit.getCharEnd());
            context.addLexeme(newLexeme);
        }
        if (singleCharHit.isPrefix()) {
            tmp_hits_.push_back(singleCharHit);
        }
    } else {
        tmp_hits_.clear();
    }

    if (context.isBufferConsumed()) {
        tmp_hits_.clear();
    }

    if (tmp_hits_.empty()) {
        context.unlockBuffer(CJKSegmenter::SEGMENTER_TYPE);
    } else {
        context.lockBuffer(CJKSegmenter::SEGMENTER_TYPE);
    }
}

void CJKSegmenter::reset() {
    tmp_hits_.clear();
}

} // namespace doris::segment_v2
