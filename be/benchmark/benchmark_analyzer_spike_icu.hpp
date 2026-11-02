// Feasibility spike for the icu parser: evidence for the design, not for merging.
//
// RuleBasedBreakIterator stays: it is where the rules live and nothing here may disagree with it.
// Everything around it is what gets cheaper, one stream reused across rows:
//   - script and combining-mark lookups come from a table built once for the BMP, not from ICU per
//     character, and the emoji test reads a bitmap instead of a UnicodeSet;
//   - no source byte offsets are tracked, nobody downstream reads them on this path;
//   - a term is encoded straight into the batch's bytes instead of UnicodeString -> UTF-8, and only
//     a term that can actually have case goes through ICU's lowercasing.
#pragma once

#include <simdutf.h>
#include <unicode/ucasemap.h>
#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "storage/index/inverted/analysis/token_filter/lower_case_filter.h"
#include "storage/index/inverted/analysis/tokenizer/icu/break_iterator_wrapper.h"
#include "storage/index/inverted/analysis/tokenizer/icu/default_icu_tokenizer_config.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"

namespace doris::segment_v2::inverted_index::analyzer_spike {

struct IcuTables {
    std::vector<int16_t> script;
    std::vector<uint64_t> combining;
    std::vector<uint64_t> emoji;
    std::vector<uint64_t> emoji_rk;

    static int32_t mapped_script(UChar32 cp) {
        UErrorCode err = U_ZERO_ERROR;
        const int32_t script = uscript_getScript(cp, &err);
        if (cp < 128) {
            return script;
        }
        if (script == USCRIPT_HAN || script == USCRIPT_HIRAGANA || script == USCRIPT_KATAKANA) {
            return USCRIPT_JAPANESE;
        }
        if (cp >= 0xFF10 && cp <= 0xFF19) {
            return USCRIPT_LATIN;
        }
        return script;
    }

    static bool combining_mark(UChar32 cp) {
        const auto type = static_cast<UCharCategory>(u_charType(cp));
        return type == U_COMBINING_SPACING_MARK || type == U_NON_SPACING_MARK ||
               type == U_ENCLOSING_MARK;
    }

    IcuTables() {
        BreakIteratorWrapper probe(nullptr);
        probe.initialize();
        script.resize(0x10000);
        combining.assign(0x10000 / 64, 0);
        emoji.assign(0x10000 / 64, 0);
        emoji_rk.assign(0x10000 / 64, 0);
        for (UChar32 cp = 0; cp < 0x10000; ++cp) {
            script[cp] = static_cast<int16_t>(mapped_script(cp));
            if (combining_mark(cp)) {
                combining[cp >> 6] |= 1ULL << (cp & 63);
            }
            if (BreakIteratorWrapper::EMOJI.contains(cp)) {
                emoji[cp >> 6] |= 1ULL << (cp & 63);
            }
            if (BreakIteratorWrapper::EMOJI_RK.contains(cp)) {
                emoji_rk[cp >> 6] |= 1ULL << (cp & 63);
            }
        }
    }

    int32_t get_script(UChar32 cp) const { return cp < 0x10000 ? script[cp] : mapped_script(cp); }
    bool is_combining(UChar32 cp) const {
        return cp < 0x10000 ? ((combining[cp >> 6] >> (cp & 63)) & 1) != 0 : combining_mark(cp);
    }
    bool is_emoji(UChar32 cp) const {
        return cp < 0x10000 ? ((emoji[cp >> 6] >> (cp & 63)) & 1) != 0
                            : BreakIteratorWrapper::EMOJI.contains(cp);
    }
    bool is_emoji_rk(UChar32 cp) const {
        return cp < 0x10000 ? ((emoji_rk[cp >> 6] >> (cp & 63)) & 1) != 0
                            : BreakIteratorWrapper::EMOJI_RK.contains(cp);
    }
};

inline const IcuTables& icu_tables() {
    static const IcuTables instance;
    return instance;
}

// BreakIteratorWrapper with the same status rule.
struct FastBreaker {
    BreakIteratorPtr rbbi;
    const UChar* text = nullptr;
    int32_t start = 0;
    int32_t status = UBRK_WORD_NONE;
    UText* ut = nullptr;

    ~FastBreaker() {
        if (ut != nullptr) {
            utext_close(ut);
        }
    }

    bool emoji_at(const IcuTables& t, int32_t current, int32_t next) const {
        const int32_t begin = start + current;
        const int32_t end = start + next;
        UChar32 cp = 0;
        U16_GET(text, 0, begin, end, cp);
        if (!t.is_emoji(cp)) {
            return false;
        }
        if (t.is_emoji_rk(cp)) {
            const int32_t trailer = begin + U16_LENGTH(cp);
            return trailer < end && (text[trailer] == 0xFE0F || text[trailer] == 0x20E3);
        }
        return true;
    }

    int32_t advance(const IcuTables& t) {
        const int32_t current = rbbi->current();
        const int32_t next = rbbi->next();
        if (next != UBRK_DONE && emoji_at(t, current, next)) {
            status = ICUTokenizerConfig::EMOJI_SEQUENCE_STATUS;
        } else {
            status = rbbi->getRuleStatus();
        }
        return next;
    }

    void set_text(const UChar* new_text, int32_t new_start, int32_t length) {
        text = new_text;
        start = new_start;
        UErrorCode error = U_ZERO_ERROR;
        ut = utext_openUChars(ut, new_text + new_start, length, &error);
        rbbi->setText(ut, error);
        status = UBRK_WORD_NONE;
    }
};

struct IcuStream {
    DefaultICUTokenizerConfig config {true, true};
    std::vector<std::unique_ptr<FastBreaker>> breakers;
    std::vector<UChar> text;
    FastBreaker* current_breaker = nullptr;
    const UChar* chars = nullptr;
    int32_t index = 0;
    int32_t limit = 0;
    int32_t script_start = 0;
    int32_t script_limit = 0;
    int32_t script_code = USCRIPT_INVALID_CODE;
    const IcuTables& tables = icu_tables();

    IcuStream() {
        const char* dict = std::getenv("ANALYZER_BENCH_DICT_DIR");
        config.initialize(std::string(dict == nullptr ? "" : dict) + "/icu");
        breakers.resize(u_getIntPropertyMaxValue(UCHAR_SCRIPT) + 1);
    }

    FastBreaker* breaker(int32_t code) {
        if (breakers[code] == nullptr) {
            auto b = std::make_unique<FastBreaker>();
            b->rbbi.reset(config.get_break_iterator(code));
            breakers[code] = std::move(b);
        }
        return breakers[code].get();
    }

    bool next_script() {
        if (script_limit >= limit) {
            return false;
        }
        script_code = USCRIPT_COMMON;
        script_start = script_limit;
        while (index < limit) {
            UChar32 ch = 0;
            U16_GET(chars, 0, index, limit, ch);
            const int32_t script = tables.get_script(ch);
            const bool same = script_code == script || script_code <= USCRIPT_INHERITED ||
                              script <= USCRIPT_INHERITED ||
                              uscript_hasScript(ch, static_cast<UScriptCode>(script_code));
            if (same || tables.is_combining(ch)) {
                index += U16_LENGTH(ch);
                if (script_code <= USCRIPT_INHERITED && script > USCRIPT_INHERITED) {
                    script_code = script;
                }
            } else {
                break;
            }
        }
        script_limit = index;
        return true;
    }

    void set_row(std::string_view row) {
        const auto n = static_cast<int32_t>(row.size());
        if (text.size() < row.size() + 1) {
            text.resize((row.size() + 1) * 2);
        }
        int32_t units = 0;
        if (simdutf::validate_ascii(row.data(), row.size())) {
            for (int32_t i = 0; i < n; ++i) {
                text[i] = static_cast<uint8_t>(row[i]);
            }
            units = n;
        } else {
            UErrorCode error = U_ZERO_ERROR;
            u_strFromUTF8WithSub(text.data(), static_cast<int32_t>(text.size()), &units, row.data(), n,
                                 0xFFFD, nullptr, &error);
        }
        chars = text.data();
        index = 0;
        limit = units;
        script_start = 0;
        script_limit = 0;
        script_code = USCRIPT_INVALID_CODE;
        if (next_script()) {
            current_breaker = breaker(script_code);
            current_breaker->set_text(chars, script_start, script_limit - script_start);
        } else {
            current_breaker = breaker(USCRIPT_COMMON);
            current_breaker->set_text(chars, 0, 0);
        }
    }

    int32_t next() {
        int32_t n = current_breaker->advance(tables);
        while (n == UBRK_DONE && next_script()) {
            current_breaker = breaker(script_code);
            current_breaker->set_text(chars, script_start, script_limit - script_start);
            n = current_breaker->advance(tables);
        }
        return n == UBRK_DONE ? UBRK_DONE : n + script_start;
    }

    int32_t current() {
        const int32_t c = current_breaker->rbbi->current();
        return c == UBRK_DONE ? UBRK_DONE : c + script_start;
    }
};

inline void icu_kernel(std::string_view row, Scratch& s) {
    static thread_local IcuStream stream;
    s.tokens.clear();
    s.owned.clear();
    stream.set_row(row);
    if (s.lowered.size() < row.size() * 3 + 16) {
        s.lowered.resize((row.size() * 3 + 16) * 2);
    }
    char* out = s.lowered.data();
    while (true) {
        int32_t start = stream.current();
        int32_t end = stream.next();
        while (end != UBRK_DONE && stream.current_breaker->status == 0) {
            start = end;
            end = stream.next();
        }
        if (end == UBRK_DONE) {
            return;
        }
        const UChar* chars = stream.chars;
        int32_t length = std::min(end - start, 255);
        if (length < end - start && length > 0 && U16_IS_LEAD(chars[start + length - 1]) &&
            U16_IS_TRAIL(chars[start + length])) {
            --length;
        }
        char* term = out;
        bool all_ascii = true;
        bool caseless = true;
        for (int32_t i = start; i < start + length; ++i) {
            const UChar c = chars[i];
            if (c < 0x80) {
                caseless = false;
                *out++ = static_cast<char>(static_cast<uint8_t>(c - 'A') < 26 ? c + 32 : c);
                continue;
            }
            all_ascii = false;
            caseless = caseless && c >= 0x3000 && c <= 0x9FFF;
            if (c < 0x800) {
                *out++ = static_cast<char>(0xC0 | (c >> 6));
                *out++ = static_cast<char>(0x80 | (c & 0x3F));
            } else if (U16_IS_LEAD(c) && i + 1 < start + length && U16_IS_TRAIL(chars[i + 1])) {
                const UChar32 cp = U16_GET_SUPPLEMENTARY(c, chars[i + 1]);
                ++i;
                *out++ = static_cast<char>(0xF0 | (cp >> 18));
                *out++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                *out++ = static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                *out++ = static_cast<char>(0xE0 | (c >> 12));
                *out++ = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                *out++ = static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        const auto size = static_cast<uint32_t>(out - term);
        if (all_ascii || caseless) {
            s.tokens.push_back({term, size, 1});
            continue;
        }
        // ASCII letters were folded above, which ICU's lowercasing would have done anyway.
        std::string lowered(term, size);
        unicode_lower(lowered);
        s.owned.push_back(std::move(lowered));
        s.tokens.push_back({s.owned.back().data(), static_cast<uint32_t>(s.owned.back().size()), 1});
    }
}

} // namespace doris::segment_v2::inverted_index::analyzer_spike

#pragma clang diagnostic pop
