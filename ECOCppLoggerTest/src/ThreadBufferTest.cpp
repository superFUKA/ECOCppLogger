#include <gtest/gtest.h>

#include <cstring>
#include <thread>
#include <vector>

#include "ECOCppLogger/ThreadBuffer.h"

using eco::log::RecordHeader;
using eco::log::RecordKind;
using eco::log::ThreadBuffer;

namespace {

// 頭（8）＋値（8）の記録を size バイトで書く。
bool writeRecord(ThreadBuffer& buffer, std::uint64_t value, std::size_t size) {
    std::byte* out = buffer.reserve(size);
    if (!out) {
        return false;
    }
    const RecordHeader header{static_cast<std::uint16_t>(size), RecordKind::Log, eco::log::kRecordHasTick, 1};
    std::memcpy(out, &header, sizeof(header));
    std::memcpy(out + sizeof(header), &value, 8);
    buffer.commit();
    return true;
}

std::uint64_t valueOf(const RecordHeader& header) {
    return eco::log::viewRecord(header).tick;
}

}  // namespace

TEST(ThreadBuffer, RoundsCapacityToPowerOfTwo) {
    ThreadBuffer buffer(5000, 0);
    EXPECT_EQ(buffer.capacity(), 8192u);
}

TEST(ThreadBuffer, ReturnsNullWhenFull) {
    ThreadBuffer buffer(4096, 0);
    int written = 0;
    while (writeRecord(buffer, static_cast<std::uint64_t>(written), 64)) {
        ++written;
    }
    EXPECT_EQ(written, 4096 / 64);
    std::size_t consumed = buffer.consume([](const RecordHeader&) {});
    EXPECT_EQ(consumed, static_cast<std::size_t>(written));
    EXPECT_TRUE(writeRecord(buffer, 0, 64));
}

TEST(ThreadBuffer, RejectsRecordsLargerThanHalf) {
    ThreadBuffer buffer(4096, 0);
    EXPECT_EQ(buffer.reserve(2048 + 8), nullptr);
}

TEST(ThreadBuffer, WrapsAroundKeepingOrder) {
    ThreadBuffer buffer(4096, 0);
    std::uint64_t next = 0;
    std::uint64_t expected = 0;
    // 大きさの違う記録で何周もさせ、詰め物を挟んでも順番が保たれることを確かめる。
    for (int round = 0; round < 2000; ++round) {
        const std::size_t size = 40 + static_cast<std::size_t>((round * 24) % 400);
        while (writeRecord(buffer, next, size)) {
            ++next;
        }
        buffer.consume([&](const RecordHeader& header) {
            EXPECT_EQ(valueOf(header), expected);
            ++expected;
        });
    }
    EXPECT_EQ(expected, next);
    EXPECT_GT(next, 4096u);
}

TEST(ThreadBuffer, ProducerAndConsumerOnDifferentThreads) {
    ThreadBuffer buffer(1 << 14, 0);
    constexpr std::uint64_t kCount = 200000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount;) {
            if (writeRecord(buffer, i, 48)) {
                ++i;
            }
        }
    });
    std::uint64_t expected = 0;
    while (expected < kCount) {
        buffer.consume([&](const RecordHeader& header) {
            ASSERT_EQ(valueOf(header), expected);
            ++expected;
        });
    }
    producer.join();
    EXPECT_TRUE(buffer.empty());
}
