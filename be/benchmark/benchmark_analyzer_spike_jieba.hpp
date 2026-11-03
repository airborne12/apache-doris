// Feasibility spike for the chinese parser: evidence for the design, not for merging.
//
// Reimplements Cut and CutAll over flat arrays with the dictionaries the current code loaded, and
// is compared token for token with it. Nothing here changes what jieba decides; it changes what a
// decision costs:
//   - a rune is {codepoint, byte offset}, not the 20-byte RuneStr;
//   - the trie is laid out breadth first with a direct table for the first rune, so a step is one
//     array probe instead of an unordered_map lookup and a pointer chase;
//   - the DAG is one reused pair of arrays instead of a 340-byte Dag per rune with an inline
//     16-entry vector, and word weights live next to the trie instead of behind DictUnit;
//   - separators are compared, not looked up, and stop words are probed by an integer key.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "CLucene/analysis/LanguageBasedAnalyzer.h"
#include "CLucene/analysis/jieba/ChineseTokenizer.h"
#pragma clang diagnostic pop

#include "benchmark_analyzer.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"

namespace doris::segment_v2::inverted_index::analyzer_spike {

struct FlatJieba {
    inline static bool simd_child = false; // compares up to 8 children in one vector

    // Breadth first, so the children of a node are the consecutive ids
    // child_begin[n] .. child_begin[n] + child_count[n] - 1, ordered by rune.
    std::vector<uint32_t> child_begin;
    std::vector<uint32_t> child_count;
    std::vector<uint32_t> key;
    std::vector<double> weight;
    std::vector<uint8_t> has_value;
    std::vector<uint32_t> root_bmp;
    std::vector<uint64_t> user_single;
    std::unordered_set<uint32_t> user_single_high;
    double min_weight = 0;
    // HMM
    double start[4];
    double trans[4][4];
    std::vector<double> emit; // 4 * 0x10000, MIN_DOUBLE when absent
    const cppjieba::HMMModel* model = nullptr;

    struct StopKey {
        uint64_t bytes;
        uint32_t size;
        bool used;
    };
    std::vector<StopKey> mix_stop;
    std::vector<StopKey> full_stop;
    const std::unordered_set<std::string_view>* mix_long = nullptr;
    const std::unordered_set<std::string_view>* full_long = nullptr;
    size_t mix_longest = 0;
    size_t full_longest = 0;

    static uint64_t load(const char* p, size_t n) {
        uint64_t v = 0;
        memcpy(&v, p, n);
        return v;
    }
    static size_t slot(uint64_t bytes, uint32_t size, size_t mask) {
        return ((bytes ^ size) * 0x9E3779B97F4A7C15ULL >> 40) & mask;
    }

    static void build_stop(const std::unordered_set<std::string_view>& words,
                           std::vector<StopKey>* table, size_t* longest) {
        table->assign(1 << 14, StopKey {0, 0, false});
        const size_t mask = table->size() - 1;
        for (std::string_view word : words) {
            *longest = std::max(*longest, word.size());
            if (word.size() > 8) {
                continue;
            }
            const uint64_t bytes = load(word.data(), word.size());
            size_t s = slot(bytes, static_cast<uint32_t>(word.size()), mask);
            while ((*table)[s].used) {
                s = (s + 1) & mask;
            }
            (*table)[s] = {bytes, static_cast<uint32_t>(word.size()), true};
        }
    }

    bool is_stop(const std::vector<StopKey>& table, const std::unordered_set<std::string_view>& lng,
                 size_t longest, const char* p, size_t n) const {
        if (n > longest) {
            return false;
        }
        if (n > 8) {
            return lng.count(std::string_view(p, n)) != 0;
        }
        const uint64_t bytes = load(p, n);
        const size_t mask = table.size() - 1;
        size_t s = slot(bytes, static_cast<uint32_t>(n), mask);
        while (table[s].used) {
            if (table[s].bytes == bytes && table[s].size == n) {
                return true;
            }
            s = (s + 1) & mask;
        }
        return false;
    }

    uint32_t root(uint32_t rune) const { return rune < 0x10000 ? root_bmp[rune] : root_slow(rune); }

    uint32_t root_slow(uint32_t rune) const {
        const uint32_t n = child_count[0];
        const uint32_t* begin = key.data() + child_begin[0];
        const uint32_t* it = std::lower_bound(begin, begin + n, rune);
        return it != begin + n && *it == rune ? child_begin[0] + (it - begin) : 0;
    }

    uint32_t child(uint32_t node, uint32_t rune) const {
        const uint32_t n = child_count[node];
        if (n == 0) {
            return 0;
        }
        const uint32_t first = child_begin[node];
        const uint32_t* base = key.data() + first;
        if (n <= 8) {
            if (simd_child) {
                const __m256i keys = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(base));
                const __m256i wanted = _mm256_set1_epi32(static_cast<int>(rune));
                const auto hits = static_cast<uint32_t>(
                        _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(keys, wanted))));
                const uint32_t live = hits & ((1U << n) - 1);
                return live != 0 ? first + static_cast<uint32_t>(__builtin_ctz(live)) : 0;
            }
            for (uint32_t i = 0; i < n; ++i) {
                if (base[i] == rune) {
                    return first + i;
                }
            }
            return 0;
        }
        const uint32_t* it = std::lower_bound(base, base + n, rune);
        return it != base + n && *it == rune ? first + (it - base) : 0;
    }

    bool is_user_single(uint32_t rune) const {
        if (rune < 0x10000) {
            return (user_single[rune >> 6] >> (rune & 63)) & 1;
        }
        return user_single_high.count(rune) != 0;
    }

    double emit_prob(int y, uint32_t rune) const {
        if (rune < 0x10000) {
            return emit[(static_cast<size_t>(y) << 16) | rune];
        }
        return model->GetEmitProb(model->emitProbVec[y], rune, cppjieba::MIN_DOUBLE);
    }
};

inline const FlatJieba& flat_jieba() {
    static const FlatJieba instance = [] {
        const char* dict = std::getenv("ANALYZER_BENCH_DICT_DIR");
        config::inverted_index_dict_path = dict == nullptr ? "" : dict;
        (void)analyzer_benchmark::builtin(InvertedIndexParserType::PARSER_CHINESE,
                                          INVERTED_INDEX_PARSER_COARSE_GRANULARITY);
        cppjieba::Jieba& jieba = lucene::analysis::jieba::JiebaSingleton::getInstance();
        FlatJieba f;
        const cppjieba::TrieNode* root = jieba.dict_trie_.trie_->root_;
        std::vector<const cppjieba::TrieNode*> order {root};
        f.child_begin.push_back(0);
        f.child_count.push_back(0);
        f.key.push_back(0);
        f.weight.push_back(0);
        f.has_value.push_back(0);
        for (size_t i = 0; i < order.size(); ++i) {
            const cppjieba::TrieNode* node = order[i];
            std::vector<std::pair<uint32_t, const cppjieba::TrieNode*>> children;
            if (node->next != nullptr) {
                children.assign(node->next->begin(), node->next->end());
                std::sort(children.begin(), children.end());
            }
            f.child_begin[i] = static_cast<uint32_t>(order.size());
            f.child_count[i] = static_cast<uint32_t>(children.size());
            for (const auto& [rune, child] : children) {
                order.push_back(child);
                f.child_begin.push_back(0);
                f.child_count.push_back(0);
                f.key.push_back(rune);
                f.weight.push_back(child->ptValue == nullptr ? 0 : child->ptValue->weight);
                f.has_value.push_back(child->ptValue != nullptr);
            }
        }
        f.root_bmp.assign(0x10000, 0);
        f.key.resize(f.key.size() + 8); // the vector compare may read past the last node
        for (uint32_t i = 0; i < f.child_count[0]; ++i) {
            const uint32_t id = f.child_begin[0] + i;
            if (f.key[id] < 0x10000) {
                f.root_bmp[f.key[id]] = id;
            }
        }
        f.user_single.assign(0x10000 / 64, 0);
        for (cppjieba::Rune rune : jieba.dict_trie_.user_dict_single_chinese_word_) {
            if (rune < 0x10000) {
                f.user_single[rune >> 6] |= 1ULL << (rune & 63);
            } else {
                f.user_single_high.insert(rune);
            }
        }
        f.min_weight = jieba.dict_trie_.GetMinWeight();
        const cppjieba::HMMModel& model = jieba.model_;
        f.model = &model;
        for (int y = 0; y < 4; ++y) {
            f.start[y] = model.startProb[y];
            for (int z = 0; z < 4; ++z) {
                f.trans[y][z] = model.transProb[y][z];
            }
        }
        f.emit.assign(4 * 0x10000, cppjieba::MIN_DOUBLE);
        for (int y = 0; y < 4; ++y) {
            for (const auto& [rune, prob] : *model.emitProbVec[y]) {
                if (rune < 0x10000) {
                    f.emit[(static_cast<size_t>(y) << 16) | rune] = prob;
                }
            }
        }
        f.mix_long = &jieba.mix_seg_.stopWords_;
        f.full_long = &jieba.full_seg_.stopWords_;
        FlatJieba::build_stop(jieba.mix_seg_.stopWords_, &f.mix_stop, &f.mix_longest);
        FlatJieba::build_stop(jieba.full_seg_.stopWords_, &f.full_stop, &f.full_longest);
        return f;
    }();
    return instance;
}

struct JiebaScratch {
    std::vector<uint32_t> rune;
    std::vector<uint32_t> offset; // one past the last rune too
    std::vector<uint32_t> dag_begin;
    std::vector<uint32_t> next_pos;
    std::vector<uint32_t> next_node;
    std::vector<double> dp;
    std::vector<uint32_t> best_len;
    std::vector<std::pair<uint32_t, uint32_t>> words;
    std::vector<std::pair<uint32_t, uint32_t>> mp;
    std::vector<int> path;
    std::vector<double> viterbi;
    std::string text;
    std::deque<std::string> owned;
};

// The exact leniency of DecodeRuneInString: only the lead byte and the remaining length are looked
// at, so a stray continuation byte reads as a two-byte lead.
inline bool decode_runes(std::string_view text, JiebaScratch& j) {
    const auto* s = reinterpret_cast<const uint8_t*>(text.data());
    const size_t n = text.size();
    j.rune.resize(n + 1);
    j.offset.resize(n + 2);
    uint32_t* runes = j.rune.data();
    uint32_t* offsets = j.offset.data();
    size_t count = 0;
    size_t i = 0;
    while (i < n) {
        const uint8_t c = s[i];
        uint32_t r;
        size_t len;
        const size_t rest = n - i;
        if (c < 0x80) {
            r = c;
            len = 1;
        } else if (c <= 0xdf && 1 < rest) {
            r = (static_cast<uint32_t>(c & 0x1f) << 6) | (s[i + 1] & 0x3f);
            len = 2;
        } else if (c <= 0xef && 2 < rest) {
            r = (static_cast<uint32_t>(c & 0x0f) << 12) |
                (static_cast<uint32_t>(s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);
            len = 3;
        } else if (c <= 0xf7 && 3 < rest) {
            r = (static_cast<uint32_t>(c & 0x07) << 18) |
                (static_cast<uint32_t>(s[i + 1] & 0x3f) << 12) |
                (static_cast<uint32_t>(s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f);
            len = 4;
        } else {
            return false;
        }
        runes[count] = r;
        offsets[count] = static_cast<uint32_t>(i);
        ++count;
        i += len;
    }
    offsets[count] = static_cast<uint32_t>(n);
    j.rune.resize(count);
    return true;
}

// Trie::Find over [lo, hi).
inline void build_dag(const FlatJieba& f, JiebaScratch& j, uint32_t lo, uint32_t hi) {
    const size_t n = hi - lo;
    j.dag_begin.resize(n + 1);
    j.next_pos.clear();
    j.next_node.clear();
    const uint32_t* runes = j.rune.data() + lo;
    for (size_t i = 0; i < n; ++i) {
        j.dag_begin[i] = static_cast<uint32_t>(j.next_pos.size());
        uint32_t node = f.root(runes[i]);
        j.next_pos.push_back(static_cast<uint32_t>(i));
        j.next_node.push_back(node != 0 && f.has_value[node] ? node : 0);
        for (size_t k = i + 1; k < n && (k - i + 1) <= cppjieba::MAX_WORD_LENGTH; ++k) {
            if (node == 0) {
                break;
            }
            node = f.child(node, runes[k]);
            if (node == 0) {
                break;
            }
            if (f.has_value[node]) {
                j.next_pos.push_back(static_cast<uint32_t>(k));
                j.next_node.push_back(node);
            }
        }
    }
    j.dag_begin[n] = static_cast<uint32_t>(j.next_pos.size());
}

// MPSegment::Cut over [lo, hi): CalcDP then CutByDag. The weights are added in the original order
// and the first maximum wins, because a word boundary is decided by that exact comparison.
inline void mp_cut(const FlatJieba& f, JiebaScratch& j, uint32_t lo, uint32_t hi) {
    build_dag(f, j, lo, hi);
    const size_t n = hi - lo;
    j.dp.resize(n + 1);
    j.best_len.resize(n + 1);
    for (size_t i = n; i-- > 0;) {
        double best = cppjieba::MIN_DOUBLE;
        uint32_t best_len = 1;
        for (uint32_t e = j.dag_begin[i]; e < j.dag_begin[i + 1]; ++e) {
            const uint32_t pos = j.next_pos[e];
            const uint32_t node = j.next_node[e];
            double val = 0.0;
            if (pos + 1 < n) {
                val += j.dp[pos + 1];
            }
            val += node != 0 ? f.weight[node] : f.min_weight;
            if (val > best) {
                best = val;
                best_len = pos - static_cast<uint32_t>(i) + 1;
            }
        }
        j.dp[i] = best;
        j.best_len[i] = best_len;
    }
    j.mp.clear();
    size_t i = 0;
    while (i < n) {
        const uint32_t len = j.best_len[i];
        j.mp.emplace_back(lo + i, lo + i + len - 1);
        i += len;
    }
}

inline void viterbi_cut(const FlatJieba& f, JiebaScratch& j, uint32_t lo, uint32_t hi) {
    constexpr size_t Y = 4;
    const size_t X = hi - lo;
    const size_t XY = X * Y;
    j.path.resize(XY);
    j.viterbi.resize(XY);
    int* path = j.path.data();
    double* weight = j.viterbi.data();
    const uint32_t* runes = j.rune.data() + lo;
    for (size_t y = 0; y < Y; ++y) {
        weight[0 + y * X] = f.start[y] + f.emit_prob(static_cast<int>(y), runes[0]);
        path[0 + y * X] = -1;
    }
    for (size_t x = 1; x < X; ++x) {
        for (size_t y = 0; y < Y; ++y) {
            const size_t now = x + y * X;
            double best = cppjieba::MIN_DOUBLE;
            int from = cppjieba::HMMModel::E;
            const double emit = f.emit_prob(static_cast<int>(y), runes[x]);
            for (size_t pre = 0; pre < Y; ++pre) {
                const double tmp = weight[x - 1 + pre * X] + f.trans[pre][y] + emit;
                if (tmp > best) {
                    best = tmp;
                    from = static_cast<int>(pre);
                }
            }
            weight[now] = best;
            path[now] = from;
        }
    }
    const double end_e = weight[X - 1 + cppjieba::HMMModel::E * X];
    const double end_s = weight[X - 1 + cppjieba::HMMModel::S * X];
    size_t stat = end_e >= end_s ? cppjieba::HMMModel::E : cppjieba::HMMModel::S;
    // Walk the statuses backwards, then emit word ranges forwards: status[x] is odd at a word end.
    thread_local std::vector<uint8_t> status;
    status.resize(X);
    for (size_t x = X; x-- > 0;) {
        status[x] = static_cast<uint8_t>(stat);
        stat = static_cast<size_t>(path[x + stat * X]);
    }
    uint32_t left = lo;
    for (size_t x = 0; x < X; ++x) {
        if ((status[x] % 2) != 0) {
            const uint32_t right = lo + static_cast<uint32_t>(x) + 1;
            j.words.emplace_back(left, right - 1);
            left = right;
        }
    }
}

inline bool is_letter(uint32_t r) {
    return (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z');
}
inline bool is_digit(uint32_t r) {
    return r >= '0' && r <= '9';
}

inline void hmm_cut(const FlatJieba& f, JiebaScratch& j, uint32_t lo, uint32_t hi) {
    const uint32_t* runes = j.rune.data();
    uint32_t left = lo;
    uint32_t right = lo;
    while (right != hi) {
        if (runes[right] < 0x80) {
            if (left != right) {
                viterbi_cut(f, j, left, right);
            }
            left = right;
            uint32_t r = left;
            if (is_letter(runes[r])) {
                ++r;
                while (r != hi && (is_letter(runes[r]) || is_digit(runes[r]))) {
                    ++r;
                }
            } else if (is_digit(runes[r])) {
                ++r;
                while (r != hi && (is_digit(runes[r]) || runes[r] == '.')) {
                    ++r;
                }
            } else {
                ++r;
            }
            right = r;
            j.words.emplace_back(left, right - 1);
            left = right;
        } else {
            ++right;
        }
    }
    if (left != right) {
        viterbi_cut(f, j, left, right);
    }
}

// MixSegment::Cut over [lo, hi).
inline void mix_cut(const FlatJieba& f, JiebaScratch& j, uint32_t lo, uint32_t hi) {
    mp_cut(f, j, lo, hi);
    const auto& mp = j.mp;
    for (size_t i = 0; i < mp.size(); ++i) {
        const auto [l, r] = mp[i];
        if (l != r || f.is_user_single(j.rune[l])) {
            j.words.emplace_back(l, r);
            continue;
        }
        size_t k = i;
        while (k < mp.size() && mp[k].first == mp[k].second &&
               !f.is_user_single(j.rune[mp[k].first])) {
            ++k;
        }
        hmm_cut(f, j, mp[i].first, mp[k - 1].first + 1);
        i = k - 1;
    }
}

inline bool is_symbol(uint32_t r) {
    return r == ' ' || r == '\t' || r == '\n' || r == 0xFF0C || r == 0x3002;
}

inline bool has_ascii_upper(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (static_cast<uint8_t>(p[i] - 'A') < 26) {
            return true;
        }
    }
    return false;
}

// The old tokenizer lowers a token whose first byte is ASCII IN PLACE, in the row buffer, when the
// token is handed out, and CutAll's tokens overlap. So what a later token reads depends on the
// tokens before it, and a token held in a batch could change after it was produced. Replaying the
// in-place write in order, and copying every token that still has an uppercase letter, gives the
// old bytes without letting a later write reach into a pending token.
inline void emit_words(const FlatJieba& f, Scratch& s, JiebaScratch& j, bool full) {
    j.owned.clear();
    char* row = j.text.data();
    const auto& stop = full ? f.full_stop : f.mix_stop;
    const auto& lng = full ? *f.full_long : *f.mix_long;
    const size_t longest = full ? f.full_longest : f.mix_longest;
    for (const auto& [l, r] : j.words) {
        const uint32_t begin = j.offset[l];
        const uint32_t size = j.offset[r + 1] - begin;
        char* data = row + begin;
        // Stop words are matched on the row as Cut left it, before any lowering.
        const char* original = s.lowered.data() + begin;
        if (f.is_stop(stop, lng, longest, original, size)) {
            continue;
        }
        const uint32_t shown = std::min<uint32_t>(size, 255);
        if (has_ascii_upper(data, size)) {
            if (static_cast<uint8_t>(data[0]) < 0x80) {
                for (uint32_t i = 0; i < size; ++i) {
                    if (static_cast<uint8_t>(data[i] - 'A') < 26) {
                        data[i] = static_cast<char>(data[i] + ('a' - 'A'));
                    }
                }
                s.tokens.push_back({data, shown, 1});
                if (has_ascii_upper(data, shown)) {
                    j.owned.emplace_back(data, shown);
                    s.tokens.back().data = j.owned.back().data();
                }
                continue;
            }
            j.owned.emplace_back(data, shown);
            s.tokens.push_back({j.owned.back().data(), shown, 1});
            continue;
        }
        s.tokens.push_back({data, shown, 1});
    }
}

inline void prepare_row(std::string_view text, Scratch& s, JiebaScratch& j) {
    j.text.assign(text);
    if (s.lowered.size() < text.size() + 1) {
        s.lowered.resize((text.size() + 1) * 2);
    }
    memcpy(s.lowered.data(), text.data(), text.size());
    j.words.clear();
}

inline void chinese_coarse(std::string_view text, Scratch& s) {
    static thread_local JiebaScratch j;
    const FlatJieba& f = flat_jieba();
    s.tokens.clear();
    prepare_row(text, s, j);
    if (!decode_runes(text, j)) {
        return;
    }
    const auto n = static_cast<uint32_t>(j.rune.size());
    uint32_t begin = 0;
    uint32_t cursor = 0;
    while (cursor < n) {
        if (is_symbol(j.rune[cursor])) {
            if (begin == cursor) {
                ++cursor;
            }
            mix_cut(f, j, begin, cursor);
            begin = cursor;
            continue;
        }
        ++cursor;
    }
    if (begin != n) {
        mix_cut(f, j, begin, n);
    }
    emit_words(f, s, j, false);
}

inline bool is_alnum_rune(uint32_t r) {
    return is_digit(r) || is_letter(r);
}

template <typename Pred>
inline void alnum_part(JiebaScratch& j, Pred pred, uint32_t& cursor, uint32_t lo, uint32_t hi) {
    const uint32_t start = cursor;
    while (cursor != hi && pred(j.rune[cursor])) {
        ++cursor;
    }
    if ((start == lo && cursor == hi) || cursor - start <= 1) {
        return;
    }
    j.words.emplace_back(start, cursor - 1);
}

inline void cut_alnum(JiebaScratch& j, uint32_t lo, uint32_t hi) {
    j.words.emplace_back(lo, hi - 1);
    uint32_t cursor = lo;
    while (cursor != hi) {
        if (is_digit(j.rune[cursor])) {
            alnum_part(j, is_digit, cursor, lo, hi);
        } else if (is_letter(j.rune[cursor])) {
            alnum_part(j, is_letter, cursor, lo, hi);
        } else {
            ++cursor;
        }
    }
}

// FullSegment::Cut over [lo, hi), including its word_len that survives from one entry to the next.
inline void full_cut(const FlatJieba& f, JiebaScratch& j, uint32_t lo, uint32_t hi) {
    build_dag(f, j, lo, hi);
    const size_t n = hi - lo;
    size_t max_idx = 0;
    size_t u_idx = 0;
    size_t word_len = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t count = j.dag_begin[i + 1] - j.dag_begin[i];
        for (uint32_t e = j.dag_begin[i]; e < j.dag_begin[i + 1]; ++e) {
            const uint32_t pos = j.next_pos[e];
            const uint32_t node = j.next_node[e];
            if (node == 0) {
                if (count == 1 && max_idx <= u_idx) {
                    j.words.emplace_back(lo + i, lo + pos);
                }
            } else {
                word_len = pos - i + 1;
                if (word_len >= 2 || (count == 1 && max_idx <= u_idx)) {
                    j.words.emplace_back(lo + i, lo + pos);
                }
            }
            max_idx = u_idx + word_len > max_idx ? u_idx + word_len : max_idx;
        }
        ++u_idx;
    }
}

inline void chinese_fine(std::string_view text, Scratch& s) {
    static thread_local JiebaScratch j;
    const FlatJieba& f = flat_jieba();
    s.tokens.clear();
    prepare_row(text, s, j);
    if (!decode_runes(text, j)) {
        return;
    }
    const auto n = static_cast<uint32_t>(j.rune.size());
    uint32_t cursor = 0;
    while (cursor != n) {
        const uint32_t begin = cursor;
        bool done = false;
        while (cursor != n) {
            const uint32_t r = j.rune[cursor];
            if (is_symbol(r)) {
                if (begin == cursor) {
                    ++cursor;
                }
                full_cut(f, j, begin, cursor);
                done = true;
                break;
            }
            if (is_alnum_rune(r)) {
                if (!is_alnum_rune(j.rune[begin])) {
                    full_cut(f, j, begin, cursor);
                } else {
                    while (cursor != n && is_alnum_rune(j.rune[cursor])) {
                        ++cursor;
                    }
                    cut_alnum(j, begin, cursor);
                }
                done = true;
                break;
            }
            ++cursor;
        }
        if (!done) {
            full_cut(f, j, begin, n);
        }
    }
    emit_words(f, s, j, true);
}

} // namespace doris::segment_v2::inverted_index::analyzer_spike

#pragma clang diagnostic pop
