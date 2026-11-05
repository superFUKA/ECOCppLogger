#include <gtest/gtest.h>

#include <cstring>
#include <thread>

#include "ECOCppLogger/ThreadBuffer.h"

using lg::RecordHeader;
using lg::RecordKind;
using lg::ThreadBuffer;

namespace {

// 頭（8）＋値（8）の記録を size バイトで書く。
bool writeRecord(ThreadBuffer& buffer, uint64_t value, size_t size, bool important = true) {
    unsigned char* out = buffer.reserve(size, important);
    if (!out) {
        return false;
    }
    const RecordHeader header{static_cast<uint16_t>(size), RecordKind::Log, lg::kRecordHasTick, 1};
    std::memcpy(out, &header, sizeof(header));
    std::memcpy(out + sizeof(header), &value, 8);
    buffer.commit();
    return true;
}

// 読み出した記録の値を順に callback へ。
template <class Callback>
void consumeValues(ThreadBuffer& buffer, Callback&& callback) {
    buffer.consume([&](const unsigned char* data, size_t length) {
        for (size_t offset = 0; offset < length;) {
            const auto& header = *reinterpret_cast<const RecordHeader*>(data + offset);
            if (header.kind == RecordKind::Log) {
                callback(lg::viewRecord(header).tick);
            }
            offset += header.size;
        }
    });
}

}  // namespace

TEST(ThreadBuffer, RoundsCapacityToPowerOfTwo) {
    ThreadBuffer buffer(5000, 0, 0, 0);
    EXPECT_EQ(buffer.capacity(), 8192u);
}

TEST(ThreadBuffer, ReturnsNullWhenFull) {
    ThreadBuffer buffer(4096, 0, 0, 0);
    int written = 0;
    while (writeRecord(buffer, static_cast<uint64_t>(written), 64)) {
        ++written;
    }
    EXPECT_EQ(written, 4096 / 64);
    int consumed = 0;
    consumeValues(buffer, [&](uint64_t) { ++consumed; });
    EXPECT_EQ(consumed, written);
    EXPECT_TRUE(writeRecord(buffer, 0, 64));
}

TEST(ThreadBuffer, KeepsRoomForImportantRecords) {
    ThreadBuffer buffer(4096, 25, 0, 0);  // 4 分の 1 は大事な記録のために取っておく
    int normal = 0;
    while (writeRecord(buffer, 0, 64, false)) {
        ++normal;
    }
    EXPECT_EQ(normal, 3072 / 64);  // 軽い記録は 3/4 まで
    int important = 0;
    while (writeRecord(buffer, 0, 64, true)) {
        ++important;
    }
    EXPECT_EQ(important, 1024 / 64);  // 残りは大事な記録が使える
}

TEST(ThreadBuffer, RejectsRecordsLargerThanHalf) {
    ThreadBuffer buffer(4096, 0, 0, 0);
    EXPECT_EQ(buffer.reserve(2048 + 8, true), nullptr);
}

TEST(ThreadBuffer, WrapsAroundKeepingOrder) {
    ThreadBuffer buffer(4096, 0, 0, 0);
    uint64_t next = 0;
    uint64_t expected = 0;
    for (int round = 0; round < 2000; ++round) {
        const size_t size = 16 + static_cast<size_t>((round * 24) % 400);
        while (writeRecord(buffer, next, size)) {
            ++next;
        }
        consumeValues(buffer, [&](uint64_t value) {
            EXPECT_EQ(value, expected);
            ++expected;
        });
    }
    EXPECT_EQ(expected, next);
    EXPECT_GT(next, 4096u);
}

TEST(ThreadBuffer, ProducerAndConsumerOnDifferentThreads) {
    ThreadBuffer buffer(1 << 14, 0, 0, 0);
    constexpr uint64_t kCount = 200000;
    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount;) {
            if (writeRecord(buffer, i, 16)) {
                ++i;
            }
        }
    });
    uint64_t expected = 0;
    while (expected < kCount) {
        consumeValues(buffer, [&](uint64_t value) {
            ASSERT_EQ(value, expected);
            ++expected;
        });
    }
    producer.join();
    EXPECT_TRUE(buffer.empty());
}

TEST(ThreadBuffer, EachThreadHasItsOwnBuffer) {
    ThreadBuffer* mine = lg::detail::threadBuffer();
    EXPECT_EQ(lg::detail::threadBuffer(), mine);
    ThreadBuffer* other = nullptr;
    std::thread([&] { other = lg::detail::threadBuffer(); }).join();
    EXPECT_NE(other, mine);
#if ECO_LOG_TLS_SLOT
    EXPECT_NE(lg::detail::g_tlsSlotOffset, 0u);  // 速い道（gs からの直接読み）が使える
#endif
}
