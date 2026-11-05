#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ECOCppLogger/Record.h"

using eco::log::ArgTag;
using namespace eco::log::detail;

namespace {

enum class Color : unsigned char { Red, Green };

template <class... Args>
using Tags = decltype(tagList(std::declval<const Args&>()...));

}  // namespace

TEST(Record, CountsPlaceholders) {
    static_assert(countPlaceholders("") == 0);
    static_assert(countPlaceholders("{}") == 1);
    static_assert(countPlaceholders("a={} b={}") == 2);
    static_assert(countPlaceholders("{{}} {}") == 1);  // {{ と }} は文字そのもの
    static_assert(countPlaceholders("{x}") == 0);
    SUCCEED();
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
    std::byte buffer[64];
    std::byte* end = encode(buffer, std::string_view("hello"));
    EXPECT_EQ(end - buffer, 4 + 5);
    std::uint32_t length;
    std::memcpy(&length, buffer, 4);
    EXPECT_EQ(length, 5u);
    EXPECT_EQ(std::memcmp(buffer + 4, "hello", 5), 0);
}

TEST(Record, TruncatesLongStrings) {
    const std::string text(eco::log::kMaxStringBytes + 100, 'a');
    std::vector<std::byte> buffer(encodedSize(std::string_view(text)));
    encode(buffer.data(), std::string_view(text));
    std::uint32_t stored;
    std::memcpy(&stored, buffer.data(), 4);
    EXPECT_EQ(stored & 0x7FFFFFFFu, eco::log::kMaxStringBytes);
    EXPECT_NE(stored & 0x80000000u, 0u);
}

TEST(Record, CharArrayStopsAtNul) {
    char name[16] = "abc";
    EXPECT_EQ(normalize(name), std::string_view("abc"));
    const char* missing = nullptr;
    EXPECT_EQ(normalize(missing), std::string_view("(null)"));
}
