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

// Feasibility spike on SIMD shapes: evidence for the design, not for merging. Three questions:
//   - can a plain loop plus the helper Doris filters use replace the hand-written backends?
//   - what does one pass over a column's contiguous chars buy over a pass per row?
//   - does comparing trie children in a vector help the dictionary stage?

#pragma once

#include "util/simd/bits.h"

namespace doris::segment_v2::inverted_index::analyzer_spike {

// The rules of classify_block as a loop the compiler has to vectorize on its own. The byte masks go
// through the helper that filters use, so the same source serves AVX2, SSE and NEON.
inline void classify_block_auto(const char* __restrict src, char* __restrict dst, uint32_t* alnum,
                                uint32_t* high) {
    uint8_t alnum_bytes[32];
    uint8_t high_bytes[32];
    for (int i = 0; i < 32; ++i) {
        const auto c = static_cast<uint8_t>(src[i]);
        const auto is_alpha = static_cast<uint8_t>(static_cast<uint8_t>((c | 0x20) - 'a') < 26);
        const auto is_digit = static_cast<uint8_t>(static_cast<uint8_t>(c - '0') < 10);
        const auto is_upper = static_cast<uint8_t>(static_cast<uint8_t>(c - 'A') < 26);
        alnum_bytes[i] = is_alpha | is_digit;
        high_bytes[i] = c >> 7;
        dst[i] = static_cast<char>(c + static_cast<uint8_t>(is_upper << 5));
    }
    *alnum = simd::bytes32_mask_to_bits32_mask(alnum_bytes);
    *high = simd::bytes32_mask_to_bits32_mask(high_bytes);
}

inline void classify_auto(std::string_view text, Scratch& s) {
    const size_t n = text.size();
    const size_t words = n / 64 + 2;
    if (s.alnum.size() < words) {
        s.alnum.resize(words * 2);
        s.high.resize(words * 2);
    }
    if (s.lowered.size() < n + 64) {
        s.lowered.resize((n + 64) * 2);
    }
    const char* src = text.data();
    char* dst = s.lowered.data();
    size_t i = 0;
    size_t w = 0;
    for (; i + 64 <= n; i += 64, ++w) {
        uint32_t a0, h0, a1, h1;
        classify_block_auto(src + i, dst + i, &a0, &h0);
        classify_block_auto(src + i + 32, dst + i + 32, &a1, &h1);
        s.alnum[w] = a0 | (static_cast<uint64_t>(a1) << 32);
        s.high[w] = h0 | (static_cast<uint64_t>(h1) << 32);
    }
    const size_t rest = n - i;
    if (rest != 0) {
        char in[64] = {};
        char out[64];
        memcpy(in, src + i, rest);
        uint32_t a0, h0, a1, h1;
        classify_block_auto(in, out, &a0, &h0);
        classify_block_auto(in + 32, out + 32, &a1, &h1);
        memcpy(dst + i, out, rest);
        const uint64_t mask = (1ULL << rest) - 1;
        s.alnum[w] = (a0 | (static_cast<uint64_t>(a1) << 32)) & mask;
        s.high[w] = (h0 | (static_cast<uint64_t>(h1) << 32)) & mask;
        ++w;
    }
    s.alnum[w] = 0;
    s.high[w] = 0;
}

inline void english_auto(std::string_view text, Scratch& s) {
    classify_auto(text, s);
    english_scan(text, s);
}

inline void basic_auto(std::string_view text, Scratch& s) {
    classify_auto(text, s);
    basic_scan(text, s);
}

inline void lower_copy_auto(std::string_view text, Scratch& s) {
    const size_t n = text.size();
    if (s.lowered.size() < n + 32) {
        s.lowered.resize((n + 32) * 2);
    }
    const char* __restrict src = text.data();
    char* __restrict dst = s.lowered.data();
    for (size_t i = 0; i < n; ++i) {
        const auto c = static_cast<uint8_t>(src[i]);
        const auto is_upper = static_cast<uint8_t>(static_cast<uint8_t>(c - 'A') < 26);
        dst[i] = static_cast<char>(c + static_cast<uint8_t>(is_upper << 5));
    }
}

inline void standard_auto(std::string_view text, Scratch& s) {
    s.tokens.clear();
    if (!accepted(text)) {
        return;
    }
    lower_copy_auto(text, s);
    standard_scan(s.lowered.data(), static_cast<int32_t>(text.size()), s);
}

inline void chinese_coarse_simd(std::string_view text, Scratch& s) {
    FlatJieba::simd_child = true;
    chinese_coarse(text, s);
    FlatJieba::simd_child = false;
}

inline void chinese_fine_simd(std::string_view text, Scratch& s) {
    FlatJieba::simd_child = true;
    chinese_fine(text, s);
    FlatJieba::simd_child = false;
}

// A Block's worth of rows at a time, as SniiIndexColumnWriter::add_values receives them: `chars` is
// the contiguous buffer of a ColumnString. Classification runs once over the whole buffer, so no
// row pays for a tail, and the per-row work is only finding the row's edges in the bitmaps.
constexpr size_t kBlockRows = 4096;

using BlockPrepare = void (*)(std::string_view chars, Scratch& s);
using BlockRow = void (*)(size_t begin, size_t end, std::string_view chars, Scratch& s);

inline void prepare_classify(std::string_view chars, Scratch& s) {
    classify(chars, s);
}

inline void english_row(size_t begin, size_t end, std::string_view, Scratch& s) {
    s.tokens.clear();
    if (begin == end) {
        return;
    }
    const char* base = s.lowered.data();
    const size_t first = begin >> 6;
    const size_t last = (end - 1) >> 6;
    uint64_t carry = 0;
    int64_t run_start = -1;
    const auto emit = [&](int64_t start, int64_t stop) {
        while (stop - start > 255) {
            s.tokens.push_back({base + start, 255, 1});
            start += 255;
        }
        s.tokens.push_back({base + start, static_cast<uint32_t>(stop - start), 1});
    };
    for (size_t w = first; w <= last; ++w) {
        uint64_t m = s.alnum[w];
        if (w == first) {
            m &= ~0ULL << (begin & 63);
        }
        if (w == last && (end & 63) != 0) {
            m &= (1ULL << (end & 63)) - 1;
        }
        uint64_t edges = m ^ ((m << 1) | carry);
        carry = m >> 63;
        while (edges != 0) {
            const int64_t pos = static_cast<int64_t>(w * 64) + __builtin_ctzll(edges);
            edges &= edges - 1;
            if (run_start < 0) {
                run_start = pos;
            } else {
                emit(run_start, pos);
                run_start = -1;
            }
        }
    }
    if (run_start >= 0) {
        emit(run_start, static_cast<int64_t>(end));
    }
}

inline void basic_row(size_t begin, size_t end, std::string_view, Scratch& s) {
    s.tokens.clear();
    if (begin == end) {
        return;
    }
    const char* base = s.lowered.data();
    const auto stop = static_cast<int32_t>(end);
    const size_t first = begin >> 6;
    const size_t last = (end - 1) >> 6;
    uint64_t carry = 0;
    int64_t run_start = -1;
    int32_t skip_until = 0;
    for (size_t w = first; w <= last; ++w) {
        uint64_t window = ~0ULL;
        if (w == first) {
            window &= ~0ULL << (begin & 63);
        }
        if (w == last && (end & 63) != 0) {
            window &= (1ULL << (end & 63)) - 1;
        }
        const uint64_t m = s.alnum[w] & window;
        const uint64_t high = s.high[w] & window;
        const uint64_t edges = m ^ ((m << 1) | carry);
        carry = m >> 63;
        uint64_t interesting = edges | high;
        while (interesting != 0) {
            const int bit = __builtin_ctzll(interesting);
            const auto pos = static_cast<int32_t>(w * 64 + bit);
            const uint64_t one = 1ULL << bit;
            interesting &= interesting - 1;
            if ((edges & one) != 0) {
                if (run_start < 0) {
                    run_start = pos;
                } else {
                    s.tokens.push_back(
                            {base + run_start,
                             static_cast<uint32_t>(std::min<int64_t>(pos - run_start, 255)), 1});
                    run_start = -1;
                }
            }
            if ((high & one) != 0 && pos >= skip_until) {
                int32_t i = pos;
                UChar32 c = U_UNASSIGNED;
                U8_NEXT(reinterpret_cast<const uint8_t*>(base), i, stop, c);
                skip_until = i;
                if (c >= 0 && is_cjk(c)) {
                    s.tokens.push_back({base + pos, static_cast<uint32_t>(i - pos), 1});
                }
            }
        }
    }
    if (run_start >= 0) {
        s.tokens.push_back({base + run_start,
                            static_cast<uint32_t>(std::min<int64_t>(stop - run_start, 255)), 1});
    }
}

// One validation and one lowercase pass for the whole buffer. A valid buffer proves a row valid only
// if neither end of the row falls inside a character, which is one byte test per row.
inline void prepare_standard(std::string_view chars, Scratch& s) {
    s.block_valid = simdutf::validate_utf8(chars.data(), chars.size());
    lower_copy(chars, s);
}

inline void standard_row(size_t begin, size_t end, std::string_view chars, Scratch& s) {
    s.tokens.clear();
    const auto is_boundary = [&](size_t at) {
        return at == chars.size() || (static_cast<uint8_t>(chars[at]) & 0xC0) != 0x80;
    };
    if (!(s.block_valid && is_boundary(begin) && is_boundary(end)) &&
        !accepted(chars.substr(begin, end - begin))) {
        return;
    }
    standard_scan(s.lowered.data() + begin, static_cast<int32_t>(end - begin), s);
}

inline void run_block(benchmark::State& state, BlockPrepare prepare, BlockRow row_kernel,
                      const std::shared_ptr<const analyzer_benchmark::Corpus>& corpus) {
    Scratch scratch;
    uint64_t tokens = 0;
    const auto& rows = corpus->rows;
    for (auto _ : state) {
        for (size_t i = 0; i < rows.size(); i += kBlockRows) {
            const size_t count = std::min(kBlockRows, rows.size() - i);
            const char* begin = rows[i].data();
            const char* end = rows[i + count - 1].data() + rows[i + count - 1].size();
            const std::string_view chars(begin, static_cast<size_t>(end - begin));
            prepare(chars, scratch);
            for (size_t r = i; r < i + count; ++r) {
                const auto row_begin = static_cast<size_t>(rows[r].data() - begin);
                row_kernel(row_begin, row_begin + rows[r].size(), chars, scratch);
                int32_t position = 0;
                for (const TokenRef& token : scratch.tokens) {
                    if (token.size != 0) {
                        position += token.pos_inc;
                        char first = token.data[0];
                        benchmark::DoNotOptimize(first);
                        benchmark::DoNotOptimize(position);
                        ++tokens;
                    }
                }
            }
        }
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(corpus->bytes.size()));
    state.SetItemsProcessed(static_cast<int64_t>(tokens));
}

// The per-row kernels were compared with the current analyzers, so they stand in for them here.
inline void verify_block(benchmark::State& state, BlockPrepare prepare, BlockRow row_kernel,
                         Kernel row_oracle,
                         const std::shared_ptr<const analyzer_benchmark::Corpus>& corpus) {
    Scratch scratch;
    Scratch oracle;
    int64_t mismatched_rows = 0;
    int64_t compared_tokens = 0;
    const auto& rows = corpus->rows;
    for (auto _ : state) {
        for (size_t i = 0; i < rows.size(); i += kBlockRows) {
            const size_t count = std::min(kBlockRows, rows.size() - i);
            const char* begin = rows[i].data();
            const char* end = rows[i + count - 1].data() + rows[i + count - 1].size();
            const std::string_view chars(begin, static_cast<size_t>(end - begin));
            prepare(chars, scratch);
            for (size_t r = i; r < i + count; ++r) {
                const auto row_begin = static_cast<size_t>(rows[r].data() - begin);
                row_kernel(row_begin, row_begin + rows[r].size(), chars, scratch);
                row_oracle(rows[r], oracle);
                bool same = scratch.tokens.size() == oracle.tokens.size();
                for (size_t t = 0; same && t < oracle.tokens.size(); ++t) {
                    const TokenRef& a = scratch.tokens[t];
                    const TokenRef& b = oracle.tokens[t];
                    same = std::string_view(a.data, a.size) == std::string_view(b.data, b.size) &&
                           a.pos_inc == b.pos_inc;
                }
                compared_tokens += static_cast<int64_t>(oracle.tokens.size());
                if (!same) {
                    ++mismatched_rows;
                }
            }
        }
    }
    state.counters["mismatched_rows"] = static_cast<double>(mismatched_rows);
    state.counters["compared_tokens"] = static_cast<double>(compared_tokens);
}

} // namespace doris::segment_v2::inverted_index::analyzer_spike
