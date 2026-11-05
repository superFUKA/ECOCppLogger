#include "ECOCppLogger/Logger.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "CrashHandler.h"
#include "FileFormat.h"
#include "Output.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace eco::log {

namespace detail {

std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::Trace)};
std::atomic<std::uint64_t> g_frame{0};
std::atomic<std::uint64_t> g_statEpoch{1};  // StatSlot::epoch の初期値 0 と違う値から始める
std::atomic<bool> g_recordTime{true};
std::atomic<bool> g_recordFrame{true};

}  // namespace detail

namespace {

namespace file = detail::file;

std::int64_t processId() {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

std::filesystem::path executablePath() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, length));
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string sourceOf(const char* fileName, std::uint32_t line, bool fullPath) {
    std::string source(detail::sourceFileName(fileName, fullPath));
    source.push_back(':');
    detail::appendUnsigned(source, line);
    return source;
}

bool listed(const std::vector<std::string>& list, std::string_view name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

struct StatTotal {
    const StatSite* site = nullptr;
    std::uint64_t count = 0;
    std::uint64_t total = 0;
    double sum = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double last = 0.0;
    std::uint32_t threads = 0;
};

// dumpHistory・落ちたときに写した、1つの呼び出し位置・スレッドの最近の値。
struct HistoryCapture {
    const StatSite* site;
    std::string thread;
    std::vector<HistoryRing::Point> points;
};

// 呼んだスレッドの上で、全スレッドの最近の値を写す（裏のスレッドが読むまで待つと、その間に上書きされるため）。
std::vector<HistoryCapture> captureHistories(std::uint64_t untilTick, bool emergency) {
    std::vector<HistoryCapture> captures;
    for (auto& entry : Registry::instance().snapshot(emergency)) {
        entry.buffer->forEachStat([&](StatSlot& slot) {
            const HistoryRing* ring = slot.history.load(std::memory_order_acquire);
            if (!ring) {
                return;
            }
            HistoryCapture capture{slot.site.load(std::memory_order_acquire), entry.name, {}};
            ring->read([&](const HistoryRing::Point& point) {
                if (point.tick <= untilTick) {  // 頼まれた後の値は含めない
                    capture.points.push_back(point);
                }
            });
            if (!capture.points.empty()) {
                captures.push_back(std::move(capture));
            }
        });
    }
    return captures;
}

// めったにない行（Event）の種類。書き出し先・見るときの絞り込みで、型と同じように扱う。
enum class EventKind { Stat, History, Event };

// 書き出し先1つ分（ファイル・分割・このファイルに書いた呼び出し位置 等）。
struct Output {
    Destination destination;
    std::filesystem::path directory;
    bool acceptsAll = true;  // 絞り込みなし：リングバッファの中身をそのまま書き写す

    std::FILE* file = nullptr;
    std::filesystem::path path;
    std::uint64_t fileBytes = 0;
    std::uint32_t part = 1;
    std::deque<std::filesystem::path> files;
    std::string out;
    std::string filtered;  // 絞り込んだ記録を並べ直す場所

    // このファイルに書いた呼び出し位置・型・スレッドの名前（ファイルを替えたら書き直す）
    std::vector<bool> knownSites;
    std::unordered_map<std::string, std::uint32_t> typeIds;
    std::unordered_map<std::uint32_t, std::string> knownThreads;
    std::vector<std::int8_t> accepts;  // 呼び出し位置ごとの判定（0：まだ、1：書く、-1：書かない）

    bool acceptsSite(std::uint32_t id, const Site& site, bool batch) {
        if (acceptsAll) {
            return true;
        }
        if (accepts.size() <= id) {
            accepts.resize(id + 1024, 0);
        }
        if (accepts[id] == 0) {
            const std::string_view type = site.type ? site.type : types::Log.name;
            const bool typeOk =
                (destination.types.empty() || listed(destination.types, type) || (batch && listed(destination.types, "batch"))) &&
                !listed(destination.excludeTypes, type) && !(batch && listed(destination.excludeTypes, "batch"));
            accepts[id] = typeOk && site.level >= destination.level ? 1 : -1;
        }
        return accepts[id] > 0;
    }

    bool acceptsEvent(EventKind kind) const {
        if (kind == EventKind::Event) {
            return true;  // 始め・終わり・落ちた・捨てた は、どの書き出し先にも残す
        }
        const std::string_view type = kind == EventKind::Stat ? "stat" : "history";
        return (destination.types.empty() || listed(destination.types, type)) &&
               !listed(destination.excludeTypes, type) && Level::Info >= destination.level;
    }
};

class Backend {
public:
    bool start(const Config& config);
    void stop();
    void flush(bool stats);
    void wake() noexcept;
    void writeNow();
    void emergency(std::string_view reason, const std::string& extraJson) noexcept;
    void requestDump(std::string_view reason);
    std::filesystem::path currentFile(std::string_view destination);

    std::atomic<bool> running{false};
    std::mutex controlMutex;  // start・stop を1つずつ

private:
    struct DumpRequest {
        std::string reason;
        std::uint64_t tick;
        std::vector<HistoryCapture> captures;
    };

    void run();
    bool pass(bool stats, bool stopping);
    void drain(bool emergency);
    void emitStats(bool emergency);
    void writeHistories(std::string_view reason, std::uint64_t untilTick, const std::vector<HistoryCapture>& captures);
    void writeSessionStart(Output& output);
    void writeSessionEnd(bool crashed);
    void writeConsole(const RecordHeader& header, std::string_view thread);

    // ---- ファイルへの書き込み（2進。文字列にするのはめったにない Event の中身だけ） ----
    bool openFile(Output& output);
    void writeOut(Output& output);
    void writeOutAll();
    void flushAll(bool close);
    static void appendFrame(Output& output, file::FrameType type,
                            std::initializer_list<std::pair<const void*, std::size_t>> parts);
    void writeClock(Output& output);
    void writeSiteDef(Output& output, std::uint32_t id, const Site& site);
    void ensureSite(Output& output, std::uint32_t id);
    void ensureThread(Output& output, std::uint32_t index, const std::string& name);
    void writeEvent(EventKind kind, std::uint64_t tick, std::string_view body);
    void writeEventTo(Output& output, std::uint64_t tick, std::string_view body);

    Config config_;
    std::unique_ptr<Clock> clock_;
    std::unique_ptr<detail::LineFormatter> console_;  // consoleLevel が Off でないときだけ

    std::timed_mutex io_;  // 取り出し・書き出しの間
    std::string baseName_;
    std::vector<std::unique_ptr<Output>> outputs_;
    std::string event_;

    std::mutex dumpMutex_;
    std::vector<DumpRequest> dumps_;

    std::thread thread_;
    std::mutex wakeMutex_;
    std::condition_variable wakeCondition_;
    std::condition_variable flushedCondition_;
    bool wakeRequested_ = false;
    bool statsRequested_ = false;
    std::uint64_t flushRequested_ = 0;
    std::uint64_t flushDone_ = 0;

    std::uint64_t lastStatTick_ = 0;
    std::uint64_t lastStatFrame_ = 0;
    std::uint64_t records_ = 0;
    std::uint64_t dropped_ = 0;
    std::atomic<bool> crashed_{false};
};

Backend& backend() {
    // わざと消さない：atexit・クラッシュ中に使うため。
    static Backend* instance = new Backend();
    return *instance;
}

void stopAtExit() {
    backend().stop();
}

void Backend::appendFrame(Output& output, file::FrameType type,
                          std::initializer_list<std::pair<const void*, std::size_t>> parts) {
    std::size_t size = 0;
    for (const auto& part : parts) {
        size += part.second;
    }
    const file::FrameHeader header{type, static_cast<std::uint32_t>(size)};
    output.out.append(reinterpret_cast<const char*>(&header), sizeof(header));
    for (const auto& part : parts) {
        output.out.append(static_cast<const char*>(part.first), part.second);
    }
}

void Backend::writeClock(Output& output) {
    const file::ClockPayload payload{clock_->baseTick(), clock_->baseUnixNanoseconds(), clock_->ticksPerNanosecond(),
                                     detail::localUtcOffsetMinutes(), 0};
    appendFrame(output, file::FrameType::Clock, {{&payload, sizeof(payload)}});
}

void Backend::writeSiteDef(Output& output, std::uint32_t id, const Site& site) {
    // 型の名前に、このファイルの中の連番を振る（初めての名前なら TypeDef を先に書く）。
    const std::string_view typeName = site.type ? site.type : types::Log.name;
    auto [type, inserted] =
        output.typeIds.try_emplace(std::string(typeName), static_cast<std::uint32_t>(output.typeIds.size() + 1));
    if (inserted) {
        appendFrame(output, file::FrameType::TypeDef, {{&type->second, 4}, {typeName.data(), typeName.size()}});
    }
    const file::SiteDefPayload payload{id, site.samplingValue, site.line, static_cast<std::uint8_t>(site.level),
                                       site.sampling, site.argCount, 0, type->second, 0};
    const std::string_view fileName = site.file;
    const std::string_view function = site.function;
    const auto formatLength = static_cast<std::uint32_t>(site.format.size());
    const auto fileLength = static_cast<std::uint32_t>(fileName.size());
    const auto functionLength = static_cast<std::uint32_t>(function.size());
    appendFrame(output, file::FrameType::SiteDef,
                {{&payload, sizeof(payload)},
                 {site.tags, site.argCount},
                 {&formatLength, 4},
                 {site.format.data(), formatLength},
                 {&fileLength, 4},
                 {fileName.data(), fileLength},
                 {&functionLength, 4},
                 {function.data(), functionLength}});
    if (output.knownSites.size() <= id) {
        output.knownSites.resize(id + 1024, false);
    }
    output.knownSites[id] = true;
}

void Backend::ensureSite(Output& output, std::uint32_t id) {
    if (id < output.knownSites.size() && output.knownSites[id]) {
        return;
    }
    if (const Site* site = Registry::instance().siteById(id)) {
        writeSiteDef(output, id, *site);
    }
}

void Backend::ensureThread(Output& output, std::uint32_t index, const std::string& name) {
    auto known = output.knownThreads.find(index);
    if (known == output.knownThreads.end() || known->second != name) {
        appendFrame(output, file::FrameType::ThreadDef, {{&index, 4}, {name.data(), name.size()}});
        output.knownThreads[index] = name;
    }
}

void Backend::writeEventTo(Output& output, std::uint64_t tick, std::string_view body) {
    appendFrame(output, file::FrameType::Event, {{&tick, 8}, {body.data(), body.size()}});
}

void Backend::writeEvent(EventKind kind, std::uint64_t tick, std::string_view body) {
    for (auto& output : outputs_) {
        if (output->acceptsEvent(kind)) {
            writeEventTo(*output, tick, body);
        }
    }
}

bool Backend::openFile(Output& output) {
    std::error_code error;
    std::filesystem::create_directories(output.directory, error);
    std::string name = baseName_;
    if (output.part > 1) {
        name += "_part" + std::to_string(output.part);
    }
    name += ".ecolog";
    output.path = output.directory / pathFromUtf8(name);
#if defined(_WIN32)
    output.file = _wfsopen(output.path.c_str(), L"wb", _SH_DENYWR);  // ほかのプログラムが読むのは許す
#else
    output.file = std::fopen(output.path.c_str(), "wb");
#endif
    if (!output.file) {
        return false;
    }
    std::setvbuf(output.file, nullptr, _IOFBF, 1 << 20);
    output.fileBytes = 0;
    output.files.push_back(output.path);
    while (config_.maxFiles > 0 && output.files.size() > config_.maxFiles) {
        std::filesystem::remove(output.files.front(), error);
        output.files.pop_front();
    }
    // ファイルごとに読めるように、頭・時刻の換算・始めの Event を書き、呼び出し位置・型・スレッドは書き直す。
    output.knownSites.clear();
    output.typeIds.clear();
    output.knownThreads.clear();
    output.out.append(file::kMagic, sizeof(file::kMagic));
    output.out.append(reinterpret_cast<const char*>(&file::kVersion), sizeof(file::kVersion));
    writeClock(output);
    writeSessionStart(output);
    return true;
}

void Backend::writeOut(Output& output) {
    if (output.out.empty() || !output.file) {
        output.out.clear();
        return;
    }
    std::fwrite(output.out.data(), 1, output.out.size(), output.file);
    output.fileBytes += output.out.size();
    output.out.clear();
    if (config_.maxFileBytes > 0 && output.fileBytes >= config_.maxFileBytes) {
        std::fclose(output.file);
        output.file = nullptr;
        ++output.part;
        if (openFile(output)) {
            writeOut(output);
        }
    }
}

void Backend::writeOutAll() {
    for (auto& output : outputs_) {
        writeOut(*output);
    }
}

void Backend::flushAll(bool close) {
    for (auto& output : outputs_) {
        writeOut(*output);
        if (output->file) {
            std::fflush(output->file);
            if (close) {
                std::fclose(output->file);
                output->file = nullptr;
            }
        }
    }
}

bool Backend::start(const Config& config) {
    std::lock_guard control(controlMutex);
    if (running.load()) {
        return false;
    }
    config_ = config;
    detail::setThreadBufferBytes(config_.threadBufferBytes);
    detail::g_level.store(static_cast<std::uint8_t>(config_.level), std::memory_order_relaxed);
    detail::g_recordTime.store(config_.recordTime, std::memory_order_relaxed);
    detail::g_recordFrame.store(config_.recordFrame, std::memory_order_relaxed);
    clock_ = std::make_unique<Clock>();
    console_.reset();
    if (config_.consoleLevel != Level::Off) {
        console_ = std::make_unique<detail::LineFormatter>(detail::OutputFormat::Text, false);
    }

    std::string stem = config_.stem.empty() ? pathUtf8(executablePath().stem()) : config_.stem;
    if (stem.empty()) {
        stem = "log";
    }
    std::string started;
    detail::appendTimestamp(started, clock_->toUnixNanoseconds(ticks()), detail::localUtcOffsetMinutes());
    // "2026-11-05T21:03:04.123456+09:00" → "20261105-210304"
    const std::string stamp = started.substr(0, 4) + started.substr(5, 2) + started.substr(8, 2) + "-" +
                              started.substr(11, 2) + started.substr(14, 2) + started.substr(17, 2);
    baseName_ = stem + "_" + stamp + "_" + std::to_string(processId());
    records_ = 0;
    dropped_ = 0;

    std::vector<Destination> destinations = config_.destinations;
    if (destinations.empty()) {
        destinations.push_back({});  // 1つ：絞り込みなしで directory へ
    }
    {
        std::lock_guard lock(io_);
        crashed_.store(false);
        outputs_.clear();
        for (const Destination& destination : destinations) {
            auto output = std::make_unique<Output>();
            output->destination = destination;
            output->directory =
                destination.name.empty() ? config_.directory : config_.directory / pathFromUtf8(destination.name);
            output->acceptsAll = destination.level == Level::Trace && destination.types.empty() &&
                                 destination.excludeTypes.empty();
            if (!openFile(*output)) {
                outputs_.clear();
                return false;
            }
            outputs_.push_back(std::move(output));
        }
        flushAll(false);
    }
    lastStatTick_ = ticks();
    lastStatFrame_ = detail::g_frame.load(std::memory_order_relaxed);
    static std::once_flag registerExit;
    std::call_once(registerExit, [] { std::atexit(stopAtExit); });
    if (config_.installCrashHandler) {
        detail::installCrashHandler();
    }
    running.store(true);
    if (config_.writeMode != WriteMode::Manual) {
        thread_ = std::thread([this] { run(); });
    }
    return true;
}

void Backend::stop() {
    std::lock_guard control(controlMutex);
    if (!running.exchange(false)) {
        return;
    }
    if (config_.writeMode == WriteMode::Manual) {
        pass(false, true);  // 残りを呼んだスレッドで書いて閉じる
        return;
    }
    wake();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::filesystem::path Backend::currentFile(std::string_view destination) {
    std::lock_guard control(controlMutex);
    std::lock_guard lock(io_);
    for (auto& output : outputs_) {
        if (destination.empty() || output->destination.name == destination) {
            return output->path;
        }
    }
    return {};
}

void Backend::wake() noexcept {
    {
        std::lock_guard lock(wakeMutex_);
        wakeRequested_ = true;
    }
    wakeCondition_.notify_one();
}

void Backend::flush(bool stats) {
    if (!running.load()) {
        return;
    }
    if (config_.writeMode == WriteMode::Manual) {
        pass(stats, false);
        return;
    }
    std::unique_lock lock(wakeMutex_);
    const std::uint64_t ticket = ++flushRequested_;
    wakeRequested_ = true;
    statsRequested_ = statsRequested_ || stats;
    wakeCondition_.notify_one();
    flushedCondition_.wait(lock, [&] { return flushDone_ >= ticket || !running.load(); });
}

// 1回の書き出し：全スレッドのバッファを書き写し、頼まれた最近の値・集計（間隔が来たら）を書き、ディスクへ出す。
// 裏のスレッド（Interval・OnRequest）か、writeOut() を呼んだスレッド（Manual）で動く。落ちた後なら何もしない。
bool Backend::pass(bool stats, bool stopping) {
    std::vector<DumpRequest> dumps;
    {
        std::lock_guard lock(dumpMutex_);
        dumps.swap(dumps_);
    }
    std::lock_guard lock(io_);
    if (crashed_.load() || outputs_.empty() || !outputs_.front()->file) {
        return false;
    }
    drain(false);
    for (const DumpRequest& dump : dumps) {
        writeHistories(dump.reason, dump.tick, dump.captures);
    }
    const double elapsedMs = clock_->toNanoseconds(static_cast<double>(ticks() - lastStatTick_)) / 1.0e6;
    if (stats || stopping || elapsedMs >= config_.statIntervalMs) {
        emitStats(false);
    }
    if (stopping) {
        drain(false);
        writeSessionEnd(false);
    }
    flushAll(stopping);  // 止めるときは閉じる（currentFile のために書き出し先は残す）
    return true;
}

void Backend::run() {
    const auto poll = std::chrono::milliseconds(std::max<std::uint32_t>(config_.pollIntervalMs, 1));
    while (true) {
        std::uint64_t ticket;
        bool stats;
        bool stopping;
        {
            std::unique_lock lock(wakeMutex_);
            const auto woken = [&] { return wakeRequested_ || !running.load(); };
            if (config_.writeMode == WriteMode::OnRequest) {
                wakeCondition_.wait(lock, woken);  // 頼まれるまで何もしない
            } else {
                wakeCondition_.wait_for(lock, poll, woken);
            }
            wakeRequested_ = false;
            ticket = flushRequested_;
            stats = statsRequested_;
            statsRequested_ = false;
            stopping = !running.load();
        }
        const bool alive = pass(stats, stopping);
        {
            std::lock_guard lock(wakeMutex_);
            flushDone_ = ticket;
        }
        flushedCondition_.notify_all();
        if (stopping || !alive) {
            return;
        }
    }
}

void Backend::writeNow() {
    if (!running.load()) {
        return;
    }
    if (config_.writeMode == WriteMode::Manual) {
        bool stats;
        {
            std::lock_guard lock(wakeMutex_);
            stats = statsRequested_;
            statsRequested_ = false;
        }
        pass(stats, false);
    } else {
        wake();  // 裏のスレッドに頼むだけ（待たない）
    }
}

void Backend::writeConsole(const RecordHeader& header, std::string_view thread) {
    const Site* site = Registry::instance().siteById(header.site);
    if (!site || site->level < config_.consoleLevel) {
        return;
    }
    std::string line;
    console_->setTimeBase({clock_->baseTick(), clock_->baseUnixNanoseconds(), clock_->ticksPerNanosecond(),
                           detail::localUtcOffsetMinutes()});
    const RecordView view = viewRecord(header);
    console_->appendRecord(line, view.tick, 0, view.aux, *site, view.payload, thread);
    std::fwrite(line.data(), 1, line.size(), stderr);
}

void Backend::drain(bool emergency) {
    Registry& registry = Registry::instance();
    for (auto& entry : registry.snapshot(emergency)) {
        ThreadBuffer* buffer = entry.buffer;
        const std::uint32_t index = buffer->index();
        const std::uint64_t drainTick = ticks();
        const std::uint32_t reserved = 0;
        buffer->consumeSpans([&](const std::byte* data, std::size_t length) {
            // 件数を数え、標準エラーへ出す設定なら出す（どちらも記録の頭を見るだけ）。
            for (std::size_t offset = 0; offset < length;) {
                const auto& header = *reinterpret_cast<const RecordHeader*>(data + offset);
                if (header.kind == RecordKind::Log || header.kind == RecordKind::Batch) {
                    ++records_;
                    if (console_ && header.kind == RecordKind::Log) {
                        writeConsole(header, entry.name);
                    }
                }
                offset += header.size;
            }
            for (auto& outputPointer : outputs_) {
                Output& output = *outputPointer;
                if (output.acceptsAll) {
                    // 絞り込みなし：初めての呼び出し位置だけ SiteDef を書き、中身はそのまま書き写す。
                    std::uint32_t lastSite = 0;
                    for (std::size_t offset = 0; offset < length;) {
                        const auto& header = *reinterpret_cast<const RecordHeader*>(data + offset);
                        if ((header.kind == RecordKind::Log || header.kind == RecordKind::Batch) &&
                            header.site != lastSite) {
                            ensureSite(output, header.site);
                            lastSite = header.site;
                        }
                        offset += header.size;
                    }
                    ensureThread(output, index, entry.name);
                    appendFrame(output, file::FrameType::Block,
                                {{&index, 4}, {&reserved, 4}, {&drainTick, 8}, {data, length}});
                } else {
                    // 絞り込みあり：この書き出し先に要る記録（とフレームの印）だけを並べ直す。文字列にはしない。
                    output.filtered.clear();
                    bool anyRecord = false;
                    for (std::size_t offset = 0; offset < length;) {
                        const auto& header = *reinterpret_cast<const RecordHeader*>(data + offset);
                        bool keep = header.kind == RecordKind::Frame;
                        if (header.kind == RecordKind::Log || header.kind == RecordKind::Batch) {
                            const Site* site = registry.siteById(header.site);
                            if (site && output.acceptsSite(header.site, *site, header.kind == RecordKind::Batch)) {
                                ensureSite(output, header.site);
                                keep = true;
                                anyRecord = true;
                            }
                        }
                        if (keep) {
                            output.filtered.append(reinterpret_cast<const char*>(data + offset), header.size);
                        }
                        offset += header.size;
                    }
                    if (anyRecord) {
                        ensureThread(output, index, entry.name);
                    }
                    if (!output.filtered.empty()) {
                        appendFrame(output, file::FrameType::Block,
                                    {{&index, 4},
                                     {&reserved, 4},
                                     {&drainTick, 8},
                                     {output.filtered.data(), output.filtered.size()}});
                    }
                }
                if (output.out.size() >= (std::size_t{4} << 20)) {
                    writeOut(output);
                }
            }
        });
        const std::uint64_t dropped = buffer->dropped();
        if (dropped > buffer->reportedDropped) {
            event_ = "\"lv\":\"warn\",\"event\":\"dropped\",\"th\":";
            detail::appendJsonString(event_, entry.name);
            event_.append(",\"count\":");
            detail::appendUnsigned(event_, dropped - buffer->reportedDropped);
            event_.append(",\"total\":");
            detail::appendUnsigned(event_, dropped);
            writeEvent(EventKind::Event, ticks(), event_);
            dropped_ += dropped - buffer->reportedDropped;
            buffer->reportedDropped = dropped;
        }
    }
}

void Backend::emitStats(bool emergency) {
    const std::uint64_t now = ticks();
    const std::uint64_t epoch = detail::g_statEpoch.fetch_add(1, std::memory_order_acq_rel);
    clock_->recalibrate();
    for (auto& output : outputs_) {
        writeClock(*output);  // 測る区間が長いほど正確になるので、読む側は最後の Clock を使う
    }

    std::map<std::string_view, StatTotal> totals;  // 名前の順に出す
    std::vector<ThreadBuffer*> finished;
    for (auto& entry : Registry::instance().snapshot(emergency)) {
        ThreadBuffer* buffer = entry.buffer;
        buffer->forEachStat([&](StatSlot& slot) {
            const StatSite* site = slot.site.load(std::memory_order_acquire);
            const std::uint64_t count = slot.count.load(std::memory_order_relaxed);
            const double sum = slot.sum.load(std::memory_order_relaxed);
            const std::uint64_t delta = count - slot.reportedCount;
            if (delta == 0) {
                return;
            }
            const double last = slot.last.load(std::memory_order_relaxed);
            const std::uint64_t written = slot.epoch.load(std::memory_order_acquire);
            double minimum = last;
            double maximum = last;
            // 終わった期間（epoch）の値は [epoch & 1] にある。持ち主が次の期間へ進んでいても（written == epoch + 1）、
            // 次の期間は反対側に書くので、こちらはそのまま残っている。
            if (written == epoch || written == epoch + 1) {
                minimum = slot.minimum[epoch & 1].load(std::memory_order_relaxed);
                maximum = slot.maximum[epoch & 1].load(std::memory_order_relaxed);
            }
            StatTotal& total = totals[std::string_view(site->name)];
            if (total.threads == 0) {
                total.site = site;
                total.minimum = minimum;
                total.maximum = maximum;
            } else {
                total.minimum = std::min(total.minimum, minimum);
                total.maximum = std::max(total.maximum, maximum);
            }
            total.count += delta;
            total.total += count;
            total.sum += sum - slot.reportedSum;
            total.last = last;
            ++total.threads;
            slot.reportedCount = count;
            slot.reportedSum = sum;
        });
        if (buffer->retired.load(std::memory_order_acquire) && buffer->empty()) {
            finished.push_back(buffer);
        }
    }

    const double periodMs = clock_->toNanoseconds(static_cast<double>(now - lastStatTick_)) / 1.0e6;
    const std::uint64_t frame = detail::g_frame.load(std::memory_order_relaxed);
    for (auto& [name, total] : totals) {
        const StatSite& site = *total.site;
        const double scale = site.kind == StatKind::Duration ? 1.0 / (clock_->ticksPerNanosecond() * 1000.0) : 1.0;
        event_ = "\"lv\":\"stat\",\"stat\":";
        detail::appendJsonString(event_, name);
        event_.append(",\"src\":");
        detail::appendJsonString(event_, sourceOf(site.file, site.line, config_.fullPath));
        event_.append(",\"n\":");
        detail::appendUnsigned(event_, total.count);
        if (site.kind != StatKind::Count) {
            event_.append(",\"sum\":");
            detail::appendDouble(event_, total.sum * scale);
            event_.append(",\"avg\":");
            detail::appendDouble(event_, total.sum * scale / static_cast<double>(total.count));
            event_.append(",\"min\":");
            detail::appendDouble(event_, total.minimum * scale);
            event_.append(",\"max\":");
            detail::appendDouble(event_, total.maximum * scale);
            event_.append(",\"last\":");
            detail::appendDouble(event_, total.last * scale);
        }
        if (site.kind == StatKind::Duration) {
            event_.append(",\"unit\":\"us\"");
        }
        event_.append(",\"total\":");
        detail::appendUnsigned(event_, total.total);
        event_.append(",\"threads\":");
        detail::appendUnsigned(event_, total.threads);
        event_.append(",\"period_ms\":");
        detail::appendUnsigned(event_, static_cast<std::uint64_t>(periodMs + 0.5));
        if (frame != lastStatFrame_) {
            event_.append(",\"frames\":");
            detail::appendUnsigned(event_, frame - lastStatFrame_);
        }
        writeEvent(EventKind::Stat, now, event_);
    }
    lastStatTick_ = now;
    lastStatFrame_ = frame;
    if (!emergency) {
        Registry::instance().removeRetired(finished);
    }
}

void Backend::writeSessionStart(Output& output) {
    event_ = "\"lv\":\"info\",\"event\":\"session.start\",\"pid\":";
    detail::appendSigned(event_, processId());
    event_.append(",\"exe\":");
    detail::appendJsonString(event_, pathUtf8(executablePath()));
    if (!output.destination.name.empty()) {
        event_.append(",\"destination\":");
        detail::appendJsonString(event_, output.destination.name);
    }
    event_.append(",\"part\":");
    detail::appendUnsigned(event_, output.part);
    event_.append(",\"level\":\"");
    event_.append(levelName(static_cast<Level>(detail::g_level.load())));
    event_.append("\",\"stat_interval_ms\":");
    detail::appendUnsigned(event_, config_.statIntervalMs);
    event_.append(",\"thread_buffer_bytes\":");
    detail::appendUnsigned(event_, config_.threadBufferBytes);
    event_.append(config_.recordTime ? ",\"record_time\":true" : ",\"record_time\":false");
    event_.append(config_.recordFrame ? ",\"record_frame\":true" : ",\"record_frame\":false");
    writeEventTo(output, ticks(), event_);
}

void Backend::writeSessionEnd(bool crashed) {
    event_ = crashed ? "\"lv\":\"fatal\"" : "\"lv\":\"info\"";
    event_.append(",\"event\":\"session.end\",\"crashed\":");
    event_.append(crashed ? "true" : "false");
    event_.append(",\"records\":");
    detail::appendUnsigned(event_, records_);
    event_.append(",\"dropped\":");
    detail::appendUnsigned(event_, dropped_);
    writeEvent(EventKind::Event, ticks(), event_);
}

void Backend::requestDump(std::string_view reason) {
    if (!running.load()) {
        return;
    }
    const std::uint64_t tick = ticks();
    auto captures = captureHistories(tick, false);
    {
        std::lock_guard lock(dumpMutex_);
        dumps_.push_back({std::string(reason), tick, std::move(captures)});
    }
    wake();
}

void Backend::writeHistories(std::string_view reason, std::uint64_t untilTick,
                             const std::vector<HistoryCapture>& captures) {
    const double ticksPerMs = clock_->ticksPerNanosecond() * 1.0e6;
    for (const HistoryCapture& capture : captures) {
        const StatSite* site = capture.site;
        event_ = "\"lv\":\"history\",\"history\":";
        detail::appendJsonString(event_, site->name);
        event_.append(",\"src\":");
        detail::appendJsonString(event_, sourceOf(site->file, site->line, config_.fullPath));
        event_.append(",\"th\":");
        detail::appendJsonString(event_, capture.thread);
        event_.append(",\"reason\":");
        detail::appendJsonString(event_, reason);
        event_.append(",\"points\":[");
        bool first = true;
        for (const HistoryRing::Point& point : capture.points) {
            if (!first) {
                event_.push_back(',');
            }
            first = false;
            event_.push_back('[');
            detail::appendUnsigned(event_, point.frame);
            event_.push_back(',');
            const double before = static_cast<double>(untilTick - point.tick) / ticksPerMs;
            detail::appendDouble(event_, std::round(before * 1000.0) / 1000.0);
            event_.push_back(',');
            detail::appendDouble(event_, point.value);
            event_.push_back(']');
        }
        event_.push_back(']');
        writeEvent(EventKind::History, untilTick, event_);
    }
}

void Backend::emergency(std::string_view reason, const std::string& extraJson) noexcept {
    if (crashed_.exchange(true) || outputs_.empty()) {
        return;
    }
    // 裏のスレッドが書いている途中なら少し待つ。取れなくても書く（落ちる前に残すことを優先する）。
    const bool locked = io_.try_lock_for(std::chrono::milliseconds(500));
    try {
        writeOutAll();
        // まず落ちたことと場所を書いて、ディスクへ出す（この後の取り出しで落ちても残るように）。
        event_ = "\"lv\":\"fatal\",\"event\":\"crash\",\"reason\":";
        detail::appendJsonString(event_, reason);
        event_.append(",\"f\":");
        detail::appendUnsigned(event_, detail::g_frame.load(std::memory_order_relaxed));
        event_.append(extraJson);
        writeEvent(EventKind::Event, ticks(), event_);
        flushAll(false);
        // 残っている記録はバッファから書き写すだけ（落ちている最中に文字列を作らない）。
        drain(true);
        emitStats(true);
        const std::uint64_t crashTick = ticks();
        writeHistories("crash", crashTick, captureHistories(crashTick, true));
        writeSessionEnd(true);
        flushAll(false);
    } catch (...) {
    }
    if (locked) {
        io_.unlock();
    }
}

}  // namespace

namespace detail {

void wakeBackend() noexcept {
    Backend& instance = backend();
    if (instance.running.load(std::memory_order_relaxed)) {
        instance.wake();
    }
}

void emergencyFlush(std::string_view reason, const std::string& extraJson) noexcept {
    backend().emergency(reason, extraJson);
}

}  // namespace detail

bool start(const Config& config) {
    return backend().start(config);
}

void stop() {
    backend().stop();
}

void flush() {
    backend().flush(false);
}

void flushStats() {
    backend().flush(true);
}

void writeOut() {
    backend().writeNow();
}

bool isRunning() noexcept {
    return backend().running.load();
}

std::filesystem::path currentFile(std::string_view destination) {
    return backend().currentFile(destination);
}

void setLevel(Level level) noexcept {
    detail::g_level.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
}

Level level() noexcept {
    return static_cast<Level>(detail::g_level.load(std::memory_order_relaxed));
}

void dumpHistory(std::string_view reason) {
    backend().requestDump(reason);
}

void setThreadName(std::string_view name) {
    Registry::instance().setName(detail::threadBuffer(), name);
}

void nextFrame() noexcept {
    const std::uint64_t frame = detail::g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!detail::g_recordFrame.load(std::memory_order_relaxed)) {
        return;
    }
    // 1件ごとにフレーム番号を持たせる代わりに、「ここからフレームN」という印を1件書く（読む側が後の記録に当てる）。
    ThreadBuffer* buffer = detail::threadBuffer();
    const bool withTime = detail::recordTime();
    const std::size_t size = sizeof(RecordHeader) + (withTime ? 8 : 0) + 8;
    std::byte* out = buffer->reserve(size);
    if (!out) {
        buffer->countDrop();
        return;
    }
    const RecordHeader header{static_cast<std::uint16_t>(size), RecordKind::Frame,
                              static_cast<std::uint8_t>(withTime ? kRecordHasTick : 0), 0};
    std::memcpy(out, &header, sizeof(header));
    std::byte* cursor = out + sizeof(header);
    if (withTime) {
        const std::uint64_t tick = ticks();
        std::memcpy(cursor, &tick, 8);
        cursor += 8;
    }
    std::memcpy(cursor, &frame, 8);
    buffer->commit();
}

std::uint64_t frame() noexcept {
    return detail::g_frame.load(std::memory_order_relaxed);
}

}  // namespace eco::log
