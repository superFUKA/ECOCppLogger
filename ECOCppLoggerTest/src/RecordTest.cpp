#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ECOCppLogger/Record.h"

using lg::ArgTag;
using namespace lg::detail;

namespace {

enum class Color : unsigned char { Red, Green };

template <class... Args>
using Tags = decltype(tagList(std::declval<const Args&>()...));

}  // namespace

TEST(Record, HeaderIsEightBytes) {
    static_assert(sizeof(lg::RecordHeader) == 8);
    SUCCEED();
}

TEST(Record, CountsPlaceholdersWithNamesAndSpecs) {
    static_assert(countPlaceholders("") == 0);
    static_assert(countPlaceholders("{}") == 1);
    static_assert(countPlaceholders("hp={hp} max={max}") == 2);
    static_assert(countPlaceholders("ratio={ratio:.3} id={:x}") == 2);
    static_assert(countPlaceholders("{{literal}} {}") == 1);
    static_assert(countPlaceholders("{oops") == -1);   // 閉じていない
    static_assert(countPlaceholders("{a{b}}") == -1);  // 入れ子
    static_assert(countPlaceholders("a}b") == -1);     // 対のない }
    SUCCEED();
}

TEST(Record, OptionsCombineLevelAndType) {
    constexpr lg::LogType physics{"physics"};
    constexpr lg::SiteOptions plain = lg::Level::Warn;
    constexpr lg::SiteOptions typed = lg::Level::Warn | physics;
    constexpr lg::SiteOptions text = lg::Level::Info | "ai";
    static_assert(plain.level == lg::Level::Warn);
    EXPECT_STREQ(plain.type, "log");
    EXPECT_STREQ(typed.type, "physics");
    EXPECT_STREQ(text.type, "ai");
}

TEST(Record, NormalizesArgumentTypes) {
    using T = Tags<int, unsigned short, float, bool, char, const char*, std::string, Color, void*, char[4]>;
    static_assert(T::count == 10);
    EXPECT_EQ(T::values[0], ArgTag::Int);
    EXPECT_EQ(T::values[1], ArgTag::UInt);
    EXPECT_EQ(T::values[2], ArgTag::Double);
    EXPECT_EQ(T::values[3], ArgTag::Bool);
    EXPECT_EQ(T::values[4], ArgTag::Char);
    EXPECT_EQ(T::values[5], ArgTag::String);
    EXPECT_EQ(T::values[6], ArgTag::String);
    EXPECT_EQ(T::values[7], ArgTag::UInt);  // enum は元の整数型
    EXPECT_EQ(T::values[8], ArgTag::Pointer);
    EXPECT_EQ(T::values[9], ArgTag::String);
}

TEST(Record, EncodesStringsWithLength) {
    unsigned char buffer[64];
    unsigned char* end = encode(buffer, std::string_view("hello"));
    EXPECT_EQ(end - buffer, 4 + 5);
    uint32_t length;
    std::memcpy(&length, buffer, 4);
    EXPECT_EQ(length, 5u);
    EXPECT_EQ(std::memcmp(buffer + 4, "hello", 5), 0);
}

TEST(Record, TruncatesLongStrings) {
    const std::string text(lg::kMaxStringBytes + 100, 'a');
    std::vector<unsigned char> buffer(encodedSize(std::string_view(text)));
    encode(buffer.data(), std::string_view(text));
    uint32_t stored;
    std::memcpy(&stored, buffer.data(), 4);
    EXPECT_EQ(stored & 0x7FFFFFFFu, lg::kMaxStringBytes);
    EXPECT_NE(stored & 0x80000000u, 0u);
}

TEST(Record, ViewsOptionalParts) {
    alignas(8) unsigned char record[32] = {};
    const lg::RecordHeader header{32, lg::RecordKind::Log, lg::kRecordHasTick | lg::kRecordHasAux, 7};
    const uint64_t tick = 123;
    const uint64_t aux = 45;
    std::memcpy(record, &header, 8);
    std::memcpy(record + 8, &tick, 8);
    std::memcpy(record + 16, &aux, 8);
    const auto view = lg::viewRecord(*reinterpret_cast<const lg::RecordHeader*>(record));
    EXPECT_EQ(view.tick, 123u);
    EXPECT_EQ(view.aux, 45u);
    EXPECT_EQ(view.payload, record + 24);
}
