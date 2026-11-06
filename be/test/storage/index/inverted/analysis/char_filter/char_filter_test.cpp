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

#include "storage/index/inverted/analysis/char_filter/char_filter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "storage/index/inverted/analysis/reader.h"
#include "storage/index/inverted/analysis/tokenizer/basic/basic_tokenizer_factory.h"

using namespace lucene::analysis;

namespace doris::segment_v2::inverted_index {

class MockDorisCharFilter : public DorisCharFilter {
public:
    MockDorisCharFilter(ReaderPtr reader) : DorisCharFilter(std::move(reader)) {}
    ~MockDorisCharFilter() override = default;

    void initialize() override {}

    void init(const void* _value, int32_t _length, bool copyData) override {
        if (_reader) {
            _reader->init(_value, _length, copyData);
        }
    }

    int32_t read(const void** start, int32_t min, int32_t max) override {
        if (_reader) {
            return _reader->read(start, min, max);
        }
        return -1;
    }

    int32_t readCopy(void* start, int32_t off, int32_t len) override {
        if (_reader) {
            return _reader->readCopy(start, off, len);
        }
        return -1;
    }
};

// Moves every offset it corrects by a fixed amount, so a test sees whether a tokenizer asked it:
// "ab" at [0, 2) comes out at [100, 102) with a shift of 100.
class ShiftingCharFilter final : public MockDorisCharFilter {
public:
    ShiftingCharFilter(ReaderPtr reader, int32_t shift)
            : MockDorisCharFilter(std::move(reader)), _shift(shift) {}

    size_t size() override { return _reader->size(); }

    int32_t correct_offset(int32_t current_offset) const override {
        return current_offset + _shift;
    }

    int32_t correct_start_offset(int32_t current_offset) const override {
        return current_offset + _shift;
    }

private:
    const int32_t _shift;
};

class DorisCharFilterTest : public ::testing::Test {
protected:
    void SetUp() override { _mock_reader = std::make_shared<lucene::util::SStringReader<char>>(); }

    ReaderPtr _mock_reader;
};

// A tokenizer looks its char filter up in reset(), once per value: the offsets of a value read
// through a char filter are corrected, and those of a value read straight are not, whatever the
// reader of the value before was.
TEST_F(DorisCharFilterTest, TokenizerCorrectsTheOffsetsOfEachValueByItsOwnReader) {
    using Offsets = std::vector<std::pair<int32_t, int32_t>>;
    const std::string text = "ab cd";
    BasicTokenizerFactory factory;
    factory.initialize(Settings());
    auto tokenizer = factory.create();
    auto plain_reader = [&text] {
        ReaderPtr reader = std::make_shared<lucene::util::SStringReader<char>>();
        reader->init(text.data(), static_cast<int32_t>(text.size()), false);
        return reader;
    };
    auto shifting_reader = [&plain_reader](int32_t shift) -> ReaderPtr {
        return std::make_shared<ShiftingCharFilter>(plain_reader(), shift);
    };
    auto offsets_of = [&tokenizer](const ReaderPtr& reader) {
        tokenizer->set_reader(reader);
        tokenizer->reset();
        Offsets offsets;
        Token token;
        while (tokenizer->next(&token) != nullptr) {
            offsets.emplace_back(token.startOffset(), token.endOffset());
        }
        return offsets;
    };

    EXPECT_EQ(offsets_of(shifting_reader(100)), (Offsets {{100, 102}, {103, 105}}));
    EXPECT_EQ(offsets_of(plain_reader()), (Offsets {{0, 2}, {3, 5}}));
    EXPECT_EQ(offsets_of(shifting_reader(7)), (Offsets {{7, 9}, {10, 12}}));
}

TEST_F(DorisCharFilterTest, ExceptionThrowing) {
    auto filter = std::make_shared<MockDorisCharFilter>(_mock_reader);

    EXPECT_THROW(filter->position(), doris::Exception);

    EXPECT_THROW(filter->skip(10), doris::Exception);

    EXPECT_THROW(filter->size(), doris::Exception);
}

} // namespace doris::segment_v2::inverted_index