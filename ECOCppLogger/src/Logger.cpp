#include "ECOCppLogger/Logger.h"

#include "ECOCppLogger/Assert.h"

#include <cstdlib>
#include <mutex>
#include <new>

#include "Registry.h"
#include "Stack.h"
#include "Writer.h"

namespace lg {

namespace detail {

std::atomic<uint8_t> g_level{static_cast<uint8_t>(Level::Trace)};
std::atomic<uint64_t> g_frame{0};
std::atomic<bool> g_recordTime{true};
std::atomic<bool> g_recordFrame{true};
std::atomic<uint64_t> g_statEpoch{1};

uint32_t registerStat(StatSite& site) noexcept {
    return Registry::instance().registerStat(site);
}

HistoryRing* createHistory(StatSlot& slot, const StatSite& site) noexcept {
    auto* ring = new (std::nothrow) HistoryRing(site.historyCapacity);
    slot.history.store(ring, std::memory_order_release);
    return ring;
}

}  // namespace detail

namespace {

void stopAtExit() {
    stop();
}

}  // namespace

bool start(const Config& config) {
    detail::Registry::instance().setBufferSettings(static_cast<size_t>(config.threadBufferBytes),
                                                   config.importantReservePercent);
    detail::g_level.store(static_cast<uint8_t>(config.level), std::memory_order_relaxed);
    detail::g_recordTime.store(config.recordTime, std::memory_order_relaxed);
    detail::g_recordFrame.store(config.recordFrame, std::memory_order_relaxed);
    setAssertAction(config.assertAction);
    if (!detail::writer().start(config)) {
        return false;
    }
    static std::once_flag registerExit;
    std::call_once(registerExit, [] { std::atexit(stopAtExit); });
    return true;
}

void stop() {
    detail::writer().stop();
}

void writeOut() {
    detail::writer().writeOut();
}

bool isRunning() noexcept {
    return detail::writer().running();
}

const char* currentFile(const char* destination) {
    return detail::writer().currentFile(destination);
}

void setLevel(Level level) noexcept {
    detail::g_level.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

Level level() noexcept {
    return static_cast<Level>(detail::g_level.load(std::memory_order_relaxed));
}

void setThreadName(const char* name) {
    detail::Registry::instance().setName(detail::threadBuffer(), name ? name : "");
}

void nextFrame() noexcept {
    const uint64_t frame = detail::g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!detail::g_recordFrame.load(std::memory_order_relaxed)) {
        return;
    }
    // 1件ごとにフレーム番号を持たせる代わりに、「ここからフレームN」の印を1件書く（読む側が後の記録に当てる）。
    ThreadBuffer* buffer = detail::threadBuffer();
    const bool withTime = detail::recordTime();
    const size_t size = sizeof(RecordHeader) + (withTime ? 8 : 0) + 8;
    unsigned char* out = buffer->reserve(size, true);
    if (!out) {
        buffer->countDrop();
        return;
    }
    const RecordHeader header{static_cast<uint16_t>(size), RecordKind::Frame,
                              static_cast<uint8_t>(withTime ? kRecordHasTick : 0), 0};
    memcpy(out, &header, sizeof(header));
    unsigned char* cursor = out + sizeof(header);
    if (withTime) {
        const uint64_t tick = ticks();
        memcpy(cursor, &tick, 8);
        cursor += 8;
    }
    memcpy(cursor, &frame, 8);
    buffer->commit();
}

void dumpHistory(const char* reason) {
    detail::writer().dumpHistory(reason ? reason : "");
}

void writeStack(const char* reason) {
    uint64_t addresses[64];
    const size_t count = detail::captureStack(addresses, 64, 1);  // writeStack 自身は飛ばす
    std::string body = "\"lv\":\"error\",\"event\":\"stack\",\"reason\":";
    detail::appendJsonString(body, reason ? reason : "");
    body.append(",\"th\":");
    detail::appendJsonString(body, detail::Registry::instance().name(detail::threadBuffer()));
    body.append(",\"stack\":");
    detail::appendStackJson(body, addresses, count);
    detail::writer().queueEvent(detail::EventKind::Event, ticks(), std::move(body));
}

void onFiberSwitch(uint64_t fiberId) noexcept {
    // 間引きの情報の場所にファイバーの ID を入れた1件（必ず残す）。
    ThreadBuffer* buffer = detail::threadBuffer();
    const bool withTime = detail::recordTime();
    const size_t size = sizeof(RecordHeader) + (withTime ? 8 : 0) + 8;
    unsigned char* out = buffer->reserve(size, true);
    if (!out) {
        buffer->countDrop();
        return;
    }
    const RecordHeader header{static_cast<uint16_t>(size), RecordKind::FiberSwitch,
                              static_cast<uint8_t>((withTime ? kRecordHasTick : 0) | kRecordHasAux), 0};
    memcpy(out, &header, sizeof(header));
    unsigned char* cursor = out + sizeof(header);
    if (withTime) {
        const uint64_t tick = ticks();
        memcpy(cursor, &tick, 8);
        cursor += 8;
    }
    memcpy(cursor, &fiberId, 8);
    buffer->commit();
}

uint64_t frame() noexcept {
    return detail::g_frame.load(std::memory_order_relaxed);
}

}  // namespace lg
