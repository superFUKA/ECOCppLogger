#pragma once

// 書き出し：すべてのスレッドのバッファを、呼んだスレッドで .ecolog へ書き写す（裏のスレッドはない）。
// 書き出し先（Destination）ごとにファイルを持ち、呼び出し位置ごとに1回判定して、合う記録だけを写す。

#include <stdint.h>
#include <stdio.h>

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ECOCppLogger/Clock.h"
#include "ECOCppLogger/Config.h"
#include "ECOCppLogger/ThreadBuffer.h"
#include "FileFormat.h"
#include "Format.h"

namespace lg::detail {

// めったにない行（Event）の種類。書き出し先・見るときの絞り込みで、型と同じように扱う。
enum class EventKind { Stat, History, Event };

class Writer {
public:
    bool start(const Config& config);
    void stop();
    void writeOut();
    bool running() const noexcept { return running_; }
    const char* currentFile(const char* destination);

    // めったにない行を書き出し先へ（集計・最近の値 等は後の工程で使う）。io の中で呼ぶ。
    void writeEvent(EventKind kind, uint64_t tick, std::string_view body);

    // 最近の値を写し取り、次の writeOut()・stop() で書く（lg::dumpHistory）。
    void dumpHistory(std::string_view reason);

    // めったにない行を取っておき、次の writeOut()・stop() で書く（どのスレッドからでも）。
    void queueEvent(EventKind kind, uint64_t tick, std::string body);

private:
    struct Output {
        std::string name;
        Level level = Level::Trace;
        std::vector<std::string> types;
        std::vector<std::string> excludeTypes;
        bool acceptsAll = true;  // 絞り込みなし：バッファの中身をそのまま書き写す

        std::filesystem::path directory;
        FILE* file = nullptr;
        std::filesystem::path path;
        std::string pathUtf8;
        uint64_t fileBytes = 0;
        uint32_t part = 1;
        std::deque<std::filesystem::path> files;
        std::string out;
        std::string filtered;

        // このファイルに書いた呼び出し位置・型・スレッド（ファイルを替えたら書き直す）
        std::vector<bool> knownSites;
        std::unordered_map<std::string, uint32_t> typeIds;
        std::unordered_map<uint32_t, std::string> knownThreads;
        std::vector<int8_t> accepts;  // 呼び出し位置ごとの判定（0：まだ、1：書く、-1：書かない）

        bool acceptsSite(uint32_t id, const Site& site, bool batch);
        bool acceptsEvent(EventKind kind) const;
    };

    bool openFile(Output& output);
    void writeOutFile(Output& output);
    void flushAll(bool close);
    static void appendFrame(Output& output, file::FrameType type,
                            std::initializer_list<std::pair<const void*, size_t>> parts);
    void writeClock(Output& output);
    void ensureSite(Output& output, uint32_t id);
    void ensureThread(Output& output, uint32_t index, uint64_t osThreadId, const std::string& name);
    void writeEventTo(Output& output, uint64_t tick, std::string_view body);
    void writeSessionStart(Output& output);
    void writeSessionEnd(bool crashed);
    void drain(bool emergency);
    void writeConsole(const RecordHeader& header, std::string_view thread);

    // 集計（WriterStats.cpp）
    struct StatTotal {
        const StatSite* site = nullptr;  // 同じ名前の集計は、呼び出し位置が違ってもまとめる（src は最初の位置）
        uint64_t count = 0;
        double sum = 0.0;
        double minimum = 0.0;
        double maximum = 0.0;
        double last = 0.0;
    };
    using StatTotals = std::unordered_map<std::string_view, StatTotal>;
    static void collectStats(ThreadBuffer& buffer, uint64_t epoch, StatTotals& totals);
    void foldRetiredStats(ThreadBuffer& buffer);  // 終わったスレッドの分を、次に出すまで取っておく
    void reportStats(bool force);                 // 期間が来ていれば（force なら必ず）集計を出す
    void writePending();                          // 取っておいた行（dumpHistory・queueEvent）を書く
    double ticksToMicroseconds(double ticks) const;

    std::mutex io_;
    bool running_ = false;
    Config config_;
    std::string directory_;
    std::string baseName_;
    std::unique_ptr<Clock> clock_;
    std::unique_ptr<LineFormatter> console_;
    std::vector<std::unique_ptr<Output>> outputs_;
    std::string event_;
    uint64_t records_ = 0;
    uint64_t dropped_ = 0;

    StatTotals carried_;
    uint64_t lastStatNanoseconds_ = 0;
    struct PendingEvent {
        EventKind kind;
        uint64_t tick;
        std::string body;
    };
    std::vector<PendingEvent> pending_;
};

Writer& writer();

}  // namespace lg::detail
