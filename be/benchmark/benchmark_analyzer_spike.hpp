// Feasibility spike for the analysis rewrite: evidence for the design, not for merging.
//
// Rewrites the core loops of english, basic and standard over a whole-row token buffer, to measure
// how much of the current cost is structural, and compares every token with the current
// implementation on the same corpus. Registered next to benchmark_analyzer.hpp from the same
// environment variables.
#pragma once

#include <CLucene/util/stringUtil.h>
#include <benchmark/benchmark.h>
#include <immintrin.h>
#include <simdutf.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "benchmark_analyzer.hpp"
#include "storage/index/inverted/analysis/token_filter/lower_case_filter.h"
#include "storage/index/inverted/analysis/tokenizer/standard/standard_tokenizer_impl.h"

namespace doris::segment_v2::inverted_index::analyzer_spike {

struct TokenRef {
    const char* data;
    uint32_t size;
    int32_t pos_inc;
};

struct Scratch {
    std::string lowered;
    std::vector<uint64_t> alnum;
    std::vector<uint64_t> high;
    std::vector<TokenRef> tokens;
    std::deque<std::string> owned;
};

inline void classify_block(const char* src, char* dst, uint32_t* alnum, uint32_t* high) {
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
    const __m256i bit = _mm256_set1_epi8(0x20);
    const __m256i folded = _mm256_or_si256(v, bit);
    const __m256i is_alpha = _mm256_and_si256(_mm256_cmpgt_epi8(folded, _mm256_set1_epi8('a' - 1)),
                                              _mm256_cmpgt_epi8(_mm256_set1_epi8('z' + 1), folded));
    const __m256i is_digit = _mm256_and_si256(_mm256_cmpgt_epi8(v, _mm256_set1_epi8('0' - 1)),
                                              _mm256_cmpgt_epi8(_mm256_set1_epi8('9' + 1), v));
    const __m256i is_upper = _mm256_and_si256(_mm256_cmpgt_epi8(v, _mm256_set1_epi8('A' - 1)),
                                              _mm256_cmpgt_epi8(_mm256_set1_epi8('Z' + 1), v));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst),
                        _mm256_add_epi8(v, _mm256_and_si256(is_upper, bit)));
    *alnum = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_or_si256(is_alpha, is_digit)));
    *high = static_cast<uint32_t>(_mm256_movemask_epi8(v));
}

// One bit per input byte; the lowered copy of the row lands in s.lowered.
inline void classify(std::string_view text, Scratch& s) {
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
        classify_block(src + i, dst + i, &a0, &h0);
        classify_block(src + i + 32, dst + i + 32, &a1, &h1);
        s.alnum[w] = a0 | (static_cast<uint64_t>(a1) << 32);
        s.high[w] = h0 | (static_cast<uint64_t>(h1) << 32);
    }
    const size_t rest = n - i;
    if (rest != 0) {
        char in[64] = {};
        char out[64];
        memcpy(in, src + i, rest);
        uint32_t a0, h0, a1, h1;
        classify_block(in, out, &a0, &h0);
        classify_block(in + 32, out + 32, &a1, &h1);
        memcpy(dst + i, out, rest);
        const uint64_t mask = (1ULL << rest) - 1;
        s.alnum[w] = (a0 | (static_cast<uint64_t>(a1) << 32)) & mask;
        s.high[w] = (h0 | (static_cast<uint64_t>(h1) << 32)) & mask;
        ++w;
    }
    s.alnum[w] = 0;
    s.high[w] = 0;
}

inline void english(std::string_view text, Scratch& s) {
    classify(text, s);
    s.tokens.clear();
    const char* base = s.lowered.data();
    const size_t words = (text.size() + 63) / 64;
    uint64_t carry = 0;
    int64_t run_start = -1;
    const auto emit = [&](int64_t start, int64_t end) {
        while (end - start > 255) {
            s.tokens.push_back({base + start, 255, 1});
            start += 255;
        }
        s.tokens.push_back({base + start, static_cast<uint32_t>(end - start), 1});
    };
    for (size_t w = 0; w < words; ++w) {
        const uint64_t m = s.alnum[w];
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
        emit(run_start, static_cast<int64_t>(text.size()));
    }
}

inline bool is_cjk(UChar32 c) {
    const auto in = [c](uint32_t lo, uint32_t hi) {
        return static_cast<uint32_t>(c) - lo <= hi - lo;
    };
    return in(0x4E00, 0x9FFF) || in(0x3400, 0x4DBF) || in(0x20000, 0x2A6DF) ||
           in(0x2A700, 0x2EBEF) || in(0x30000, 0x3134A);
}

inline void basic(std::string_view text, Scratch& s) {
    classify(text, s);
    s.tokens.clear();
    const char* base = s.lowered.data();
    const auto n = static_cast<int32_t>(text.size());
    const size_t words = (text.size() + 63) / 64;
    uint64_t carry = 0;
    int64_t run_start = -1;
    int32_t skip_until = 0;
    for (size_t w = 0; w < words; ++w) {
        const uint64_t m = s.alnum[w];
        const uint64_t edges = m ^ ((m << 1) | carry);
        carry = m >> 63;
        uint64_t interesting = edges | s.high[w];
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
            if ((s.high[w] & one) != 0 && pos >= skip_until) {
                int32_t i = pos;
                UChar32 c = U_UNASSIGNED;
                U8_NEXT(reinterpret_cast<const uint8_t*>(base), i, n, c);
                skip_until = i;
                if (c >= 0 && is_cjk(c)) {
                    s.tokens.push_back({base + pos, static_cast<uint32_t>(i - pos), 1});
                }
            }
        }
    }
    if (run_start >= 0) {
        s.tokens.push_back({base + run_start,
                            static_cast<uint32_t>(std::min<int64_t>(n - run_start, 255)), 1});
    }
}

// The JFlex tables flattened: states 59, classes 29, so one byte per transition.
struct DenseDfa {
    static constexpr int kClasses = 29;
    static constexpr uint8_t kDead = 0xFF;
    std::array<uint8_t, 59 * kClasses> next {};
    std::array<uint8_t, 59> attribute {};
    std::array<uint8_t, 59> action {};
    std::array<uint8_t, 128> ascii_class {};
    int initial = 0;

    DenseDfa() {
        using Impl = StandardTokenizerImpl;
        for (int state = 0; state < 59; ++state) {
            for (int cls = 0; cls < kClasses; ++cls) {
                const int32_t to = Impl::ZZ_TRANS[Impl::ZZ_ROWMAP[state] + cls];
                next[state * kClasses + cls] = to < 0 ? kDead : static_cast<uint8_t>(to);
            }
            attribute[state] = static_cast<uint8_t>(Impl::ZZ_ATTRIBUTE[state]);
            action[state] = static_cast<uint8_t>(Impl::ZZ_ACTION[state]);
        }
        for (int c = 0; c < 128; ++c) {
            ascii_class[c] = static_cast<uint8_t>(Impl::ZZ_CMAP_BLOCKS[c]);
        }
        initial = Impl::ZZ_LEXSTATE[0];
    }

    static int character_class(int32_t cp) {
        using Impl = StandardTokenizerImpl;
        const int32_t offset = cp & 255;
        return offset == cp ? Impl::ZZ_CMAP_BLOCKS[offset]
                            : Impl::ZZ_CMAP_BLOCKS[Impl::ZZ_CMAP_TOP[cp >> 8] | offset];
    }
};

inline const DenseDfa& dfa() {
    static const DenseDfa instance;
    return instance;
}

// The old validator accepts what strict UTF-8 refuses (overlong forms): strict success is a fast
// accept, strict failure goes to the old rule, which decides.
inline bool accepted(std::string_view text) {
    return simdutf::validate_utf8(text.data(), text.size()) || StringUtil::validate_utf8(text) == 0;
}

inline void lower_copy(std::string_view text, Scratch& s) {
    const size_t n = text.size();
    if (s.lowered.size() < n + 32) {
        s.lowered.resize((n + 32) * 2);
    }
    const char* src = text.data();
    char* dst = s.lowered.data();
    const __m256i bit = _mm256_set1_epi8(0x20);
    size_t i = 0;
    const auto one = [&](const char* in, char* out) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in));
        const __m256i upper = _mm256_and_si256(_mm256_cmpgt_epi8(v, _mm256_set1_epi8('A' - 1)),
                                               _mm256_cmpgt_epi8(_mm256_set1_epi8('Z' + 1), v));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out),
                            _mm256_add_epi8(v, _mm256_and_si256(upper, bit)));
    };
    for (; i + 32 <= n; i += 32) {
        one(src + i, dst + i);
    }
    if (i < n) {
        char in[32] = {};
        char out[32];
        memcpy(in, src + i, n - i);
        one(in, out);
        memcpy(dst + i, out, n - i);
    }
}

inline int32_t decode(const uint8_t* p, int* length) {
    const uint8_t c = p[0];
    if ((c & 0xE0) == 0xC0) {
        *length = 2;
        return ((c & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0) {
        *length = 3;
        return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    *length = 4;
    return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

// Longest stop word is 5 bytes, so nearly every token leaves on the length test.
inline bool is_stop_word(std::string_view term) {
    static const std::unordered_set<std::string_view> words = {
            "a",   "an",    "and",  "are",   "as",    "at",   "be",   "but", "by",  "for",  "if",
            "in",  "into",  "is",   "it",    "no",    "not",  "of",   "on",  "or",  "such", "that",
            "the", "their", "then", "there", "these", "they", "this", "to",  "was", "will", "with"};
    return term.size() <= 5 && words.count(term) != 0;
}

inline void standard(std::string_view text, Scratch& s) {
    s.tokens.clear();
    if (!accepted(text)) {
        return;
    }
    lower_copy(text, s);
    const DenseDfa& table = dfa();
    const auto* p = reinterpret_cast<const uint8_t*>(s.lowered.data());
    const auto n = static_cast<int32_t>(text.size());
    int32_t pos = 0;
    while (pos < n) {
        const int32_t start = pos;
        int32_t cur = pos;
        int32_t marked = pos;
        int state = table.initial;
        int matched = (table.attribute[state] & 1) != 0 ? state : -1;
        while (cur < n && cur - start < 255) {
            const uint8_t c = p[cur];
            int cls;
            int length = 1;
            if (c < 0x80) [[likely]] {
                cls = table.ascii_class[c];
            } else {
                cls = DenseDfa::character_class(decode(p + cur, &length));
            }
            const uint8_t to = table.next[state * DenseDfa::kClasses + cls];
            if (to == DenseDfa::kDead) {
                break;
            }
            cur += length;
            state = to;
            const uint8_t attribute = table.attribute[to];
            if ((attribute & 1) != 0) {
                matched = to;
                marked = cur;
                if ((attribute & 8) != 0) {
                    break;
                }
            }
        }
        pos = marked;
        const int action = table.action[matched];
        if (action == 1) {
            continue; // skipped text
        }
        const auto size = static_cast<uint32_t>(marked - start);
        if (size > 255) {
            continue;
        }
        const char* data = s.lowered.data() + start;
        if (action != 3) {
            // Only WORD is lowercased by the old tokenizer. Every other type is free of ASCII
            // letters, so the lowered copy is byte-identical for them.
            s.tokens.push_back({data, size, 1});
        } else if (!is_stop_word({data, size})) {
            s.tokens.push_back({data, size, 1});
        }
    }
}

inline bool has_high_byte(const char* p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t word;
        memcpy(&word, p + i, 8);
        if ((word & 0x8080808080808080ULL) != 0) {
            return true;
        }
    }
    for (; i < n; ++i) {
        if (static_cast<uint8_t>(p[i]) >= 0x80) {
            return true;
        }
    }
    return false;
}

// One three-byte character in U+3000..U+9FFF: no case mapping exists there.
inline bool caseless_cjk(const char* p, size_t n) {
    const auto lead = static_cast<uint8_t>(p[0]);
    return n == 3 && lead >= 0xE3 && lead <= 0xE9;
}

// custom "standard" + "lowercase". The current tokenizer's behaviour on invalid input depends on
// where its 255-byte buffer refills, so a row strict UTF-8 refuses stays on the current code.
inline void custom_standard_lower(std::string_view text, Scratch& s) {
    s.tokens.clear();
    s.owned.clear();
    if (!simdutf::validate_utf8(text.data(), text.size())) {
        static const AnalyzerPtr analyzer = analyzer_benchmark::custom("standard", {"lowercase"});
        ReaderPtr reader = InvertedIndexAnalyzer::create_reader({});
        reader->init(text.data(), static_cast<int32_t>(text.size()), false);
        std::unique_ptr<lucene::analysis::TokenStream> stream(analyzer->tokenStream(L"", reader));
        lucene::analysis::Token token;
        while (stream->next(&token)) {
            s.owned.emplace_back(token.termBuffer<char>(), token.termLength<char>());
            s.tokens.push_back({s.owned.back().data(), static_cast<uint32_t>(s.owned.back().size()),
                                token.getPositionIncrement()});
        }
        stream->close();
        return;
    }
    lower_copy(text, s);
    const DenseDfa& table = dfa();
    const auto* p = reinterpret_cast<const uint8_t*>(s.lowered.data());
    const auto n = static_cast<int32_t>(text.size());
    int32_t pos = 0;
    while (pos < n) {
        const int32_t start = pos;
        int32_t cur = pos;
        int32_t marked = pos;
        int state = table.initial;
        int matched = (table.attribute[state] & 1) != 0 ? state : -1;
        while (cur < n) {
            const uint8_t c = p[cur];
            int cls;
            int length = 1;
            if (c < 0x80) [[likely]] {
                cls = table.ascii_class[c];
            } else {
                cls = DenseDfa::character_class(decode(p + cur, &length));
            }
            if (cur + length - start > 255) {
                break;
            }
            const uint8_t to = table.next[state * DenseDfa::kClasses + cls];
            if (to == DenseDfa::kDead) {
                break;
            }
            cur += length;
            state = to;
            const uint8_t attribute = table.attribute[to];
            if ((attribute & 1) != 0) {
                matched = to;
                marked = cur;
                if ((attribute & 8) != 0) {
                    break;
                }
            }
        }
        pos = marked;
        if (table.action[matched] == 1) {
            continue;
        }
        const char* data = s.lowered.data() + start;
        const auto size = static_cast<uint32_t>(marked - start);
        if (has_high_byte(data, size) && !caseless_cjk(data, size)) {
            std::string lowered(data, size);
            unicode_lower(lowered);
            s.owned.push_back(std::move(lowered));
            s.tokens.push_back(
                    {s.owned.back().data(), static_cast<uint32_t>(s.owned.back().size()), 1});
        } else {
            s.tokens.push_back({data, size, 1});
        }
    }
}

// Which ASCII bytes let a run of letters and digits be cut without running the DFA. Derived from
// the DFA itself, so the shortcut cannot disagree with the tables:
//   - from every state reachable by letters and digits alone, the byte is a dead transition, so
//     the run ends there ("error" followed by ' ');
//   - from the start state the byte is matched as skipped text, and a letter or digit after it is
//     a dead transition, so the next token starts clean;
//   - two such bytes in a row either stop or stay in the second byte's state, so a stretch of them
//     can be re-entered at its last byte ("\r\n" fails this and stays on the DFA).
struct FastRuns {
    std::array<bool, 256> hard {};
    std::array<bool, 256> alnum {};

    FastRuns() {
        const DenseDfa& t = dfa();
        constexpr int K = DenseDfa::kClasses;
        std::array<bool, DenseDfa::kClasses> alnum_class {};
        for (int c = 0; c < 128; ++c) {
            alnum[c] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (alnum[c]) {
                alnum_class[t.ascii_class[c]] = true;
            }
        }
        std::array<bool, 59> reachable {};
        std::vector<int> pending {t.initial};
        reachable[t.initial] = true;
        while (!pending.empty()) {
            const int state = pending.back();
            pending.pop_back();
            for (int cls = 0; cls < K; ++cls) {
                const uint8_t to = t.next[state * K + cls];
                if (alnum_class[cls] && to != DenseDfa::kDead && !reachable[to]) {
                    reachable[to] = true;
                    pending.push_back(to);
                }
            }
        }
        std::array<int, 128> skip_state {};
        for (int c = 0; c < 128; ++c) {
            if (alnum[c]) {
                continue;
            }
            const int cls = t.ascii_class[c];
            bool usable = true;
            for (int state = 0; state < 59; ++state) {
                usable =
                        usable && !(reachable[state] && t.next[state * K + cls] != DenseDfa::kDead);
            }
            const uint8_t skip = t.next[t.initial * K + cls];
            usable = usable && skip != DenseDfa::kDead && (t.attribute[skip] & 1) != 0 &&
                     t.action[skip] == 1;
            for (int other = 0; usable && other < K; ++other) {
                usable = !(alnum_class[other] && t.next[skip * K + other] != DenseDfa::kDead);
            }
            hard[c] = usable;
            skip_state[c] = skip;
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (int a = 0; a < 128; ++a) {
                for (int b = 0; hard[a] && b < 128; ++b) {
                    if (!hard[b]) {
                        continue;
                    }
                    const uint8_t to = t.next[skip_state[a] * K + t.ascii_class[b]];
                    if (to != DenseDfa::kDead && to != skip_state[b]) {
                        hard[a] = false;
                        hard[b] = false;
                        changed = true;
                    }
                }
            }
        }
    }
};

inline const FastRuns& fast_runs() {
    static const FastRuns instance;
    return instance;
}

inline int32_t run_end(const std::vector<uint64_t>& alnum, int32_t pos, int32_t n) {
    size_t word = static_cast<size_t>(pos) / 64;
    uint64_t rest = ~alnum[word] >> (pos % 64);
    if (rest != 0) {
        return std::min(pos + __builtin_ctzll(rest), n);
    }
    while (true) {
        ++word;
        const uint64_t bits = ~alnum[word];
        if (bits != 0) {
            return std::min(static_cast<int32_t>(word * 64) + __builtin_ctzll(bits), n);
        }
    }
}

// The dense DFA behind the run shortcut. Builtin scans at most 255 bytes from a token start and
// may overshoot by a multi-byte character; custom stops before a character that would not fit.
template <bool kBuiltin, typename Emit>
inline void scan_runs(std::string_view text, Scratch& s, Emit&& emit) {
    classify(text, s);
    const FastRuns& runs = fast_runs();
    const DenseDfa& table = dfa();
    const auto* p = reinterpret_cast<const uint8_t*>(s.lowered.data());
    const auto n = static_cast<int32_t>(text.size());
    int32_t pos = 0;
    while (true) {
        int32_t q = pos;
        while (q < n && runs.hard[p[q]]) {
            ++q;
        }
        if (q == n) {
            return;
        }
        const bool word_start = runs.alnum[p[q]];
        if (word_start) {
            const int32_t end = run_end(s.alnum, q, n);
            if (end - q <= 255 && (end == n || runs.hard[p[end]])) {
                emit(q, end, 3);
                pos = end;
                continue;
            }
        }
        const int32_t start = (q > pos && !word_start) ? q - 1 : q;
        int32_t cur = start;
        int32_t marked = start;
        int state = table.initial;
        int matched = (table.attribute[state] & 1) != 0 ? state : -1;
        while (cur < n && (!kBuiltin || cur - start < 255)) {
            const uint8_t c = p[cur];
            int cls;
            int length = 1;
            if (c < 0x80) [[likely]] {
                cls = table.ascii_class[c];
            } else {
                cls = DenseDfa::character_class(decode(p + cur, &length));
            }
            if (!kBuiltin && cur + length - start > 255) {
                break;
            }
            const uint8_t to = table.next[state * DenseDfa::kClasses + cls];
            if (to == DenseDfa::kDead) {
                break;
            }
            cur += length;
            state = to;
            const uint8_t attribute = table.attribute[to];
            if ((attribute & 1) != 0) {
                matched = to;
                marked = cur;
                if ((attribute & 8) != 0) {
                    break;
                }
            }
        }
        pos = marked;
        const int action = table.action[matched];
        if (action != 1) {
            emit(start, marked, action);
        }
    }
}

inline void standard_runs(std::string_view text, Scratch& s) {
    s.tokens.clear();
    if (!accepted(text)) {
        return;
    }
    scan_runs<true>(text, s, [&s](int32_t start, int32_t end, int action) {
        const auto size = static_cast<uint32_t>(end - start);
        const char* data = s.lowered.data() + start;
        if (size <= 255 && (action != 3 || !is_stop_word({data, size}))) {
            s.tokens.push_back({data, size, 1});
        }
    });
}

inline void custom_standard_lower_runs(std::string_view text, Scratch& s) {
    if (!simdutf::validate_utf8(text.data(), text.size())) {
        custom_standard_lower(text, s);
        return;
    }
    s.tokens.clear();
    s.owned.clear();
    scan_runs<false>(text, s, [&s](int32_t start, int32_t end, int) {
        const auto size = static_cast<uint32_t>(end - start);
        const char* data = s.lowered.data() + start;
        if (has_high_byte(data, size) && !caseless_cjk(data, size)) {
            std::string lowered(data, size);
            unicode_lower(lowered);
            s.owned.push_back(std::move(lowered));
            s.tokens.push_back(
                    {s.owned.back().data(), static_cast<uint32_t>(s.owned.back().size()), 1});
        } else {
            s.tokens.push_back({data, size, 1});
        }
    });
}

using Kernel = void (*)(std::string_view, Scratch&);

inline void run_kernel(benchmark::State& state, Kernel kernel,
                       const std::shared_ptr<const analyzer_benchmark::Corpus>& corpus) {
    Scratch scratch;
    uint64_t tokens = 0;
    for (auto _ : state) {
        for (const std::string_view row : corpus->rows) {
            kernel(row, scratch);
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
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(corpus->bytes.size()));
    state.SetItemsProcessed(static_cast<int64_t>(tokens));
}

// Token-for-token comparison with the current implementation, term bytes and position increments.
inline void verify_kernel(benchmark::State& state, Kernel kernel,
                          const analyzer_benchmark::AnalyzerFactory& factory,
                          const std::shared_ptr<const analyzer_benchmark::Corpus>& corpus,
                          const std::string& label) {
    AnalyzerPtr analyzer = factory();
    ReaderPtr reader = InvertedIndexAnalyzer::create_reader({});
    Scratch scratch;
    int64_t mismatched_rows = 0;
    int64_t compared_tokens = 0;
    for (auto _ : state) {
        for (const std::string_view row : corpus->rows) {
            kernel(row, scratch);
            reader->init(row.data(), static_cast<int32_t>(row.size()), false);
            std::unique_ptr<lucene::analysis::TokenStream> stream(
                    analyzer->tokenStream(L"", reader));
            lucene::analysis::Token token;
            size_t index = 0;
            bool same = true;
            while (stream->next(&token)) {
                const std::string_view term(token.termBuffer<char>(), token.termLength<char>());
                if (index >= scratch.tokens.size() ||
                    term != std::string_view(scratch.tokens[index].data,
                                             scratch.tokens[index].size) ||
                    token.getPositionIncrement() != scratch.tokens[index].pos_inc) {
                    same = false;
                }
                ++index;
                ++compared_tokens;
            }
            stream->close();
            same = same && index == scratch.tokens.size();
            if (!same) {
                if (mismatched_rows == 0) {
                    std::cerr << "first mismatch " << label << ": old=" << index
                              << " new=" << scratch.tokens.size() << " row=" << row.substr(0, 200)
                              << '\n';
                }
                ++mismatched_rows;
            }
        }
    }
    state.counters["mismatched_rows"] = static_cast<double>(mismatched_rows);
    state.counters["compared_tokens"] = static_cast<double>(compared_tokens);
}

inline const bool registered = [] {
    const char* directory = std::getenv("ANALYZER_BENCH_CORPUS_DIR");
    if (directory == nullptr) {
        return false;
    }
    const char* megabytes = std::getenv("ANALYZER_BENCH_CORPUS_MB");
    const size_t max_bytes = static_cast<size_t>(megabytes == nullptr ? 8 : std::atoi(megabytes))
                             << 20;
    struct Case {
        const char* name;
        Kernel kernel;
        analyzer_benchmark::AnalyzerFactory factory;
    };
    using analyzer_benchmark::builtin;
    const std::vector<Case> cases {
            {"english", english, [] { return builtin(InvertedIndexParserType::PARSER_ENGLISH); }},
            {"basic", basic, [] { return builtin(InvertedIndexParserType::PARSER_BASIC); }},
            {"standard", standard,
             [] { return builtin(InvertedIndexParserType::PARSER_STANDARD); }},
            {"standard_runs", standard_runs,
             [] { return builtin(InvertedIndexParserType::PARSER_STANDARD); }},
            {"custom_standard_lower", custom_standard_lower,
             [] { return analyzer_benchmark::custom("standard", {"lowercase"}); }},
            {"custom_standard_lower_runs", custom_standard_lower_runs,
             [] { return analyzer_benchmark::custom("standard", {"lowercase"}); }}};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".rows") {
            continue;
        }
        auto corpus = analyzer_benchmark::load_corpus(entry.path(), max_bytes);
        const std::string corpus_name = entry.path().stem().string();
        for (const Case& item : cases) {
            const Kernel kernel = item.kernel;
            const auto factory = item.factory;
            const std::string label = std::string(item.name) + "/" + corpus_name;
            benchmark::RegisterBenchmark(("spike/" + label).c_str(),
                                         [kernel, corpus](benchmark::State& state) {
                                             run_kernel(state, kernel, corpus);
                                         })
                    ->Unit(benchmark::kMillisecond)
                    ->MinTime(1.0);
            benchmark::RegisterBenchmark(("verify/" + label).c_str(),
                                         [kernel, corpus, factory, label](benchmark::State& state) {
                                             verify_kernel(state, kernel, factory, corpus, label);
                                         })
                    ->Unit(benchmark::kMillisecond)
                    ->Iterations(1);
        }
    }
    return true;
}();

} // namespace doris::segment_v2::inverted_index::analyzer_spike
