#include "ECOCppLogger/Logger.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "CrashHandler.h"
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

}  // namespace detail

namespace {

constexpr std::string_view kAbout =
    "JSON Lines log from ECOCppLogger. t=local time, f=frame number, lv=level, th=thread, src=file:line, "
    "fn=function, msg=message. sample=this site is sampled (n=call count, skipped=calls not logged since the "
    "previous line). lv=stat lines aggregate ECO_STAT/ECO_COUNT/ECO_SCOPE_TIME over period_ms: n=calls in "
    "the period, total=calls since start, unit=us means durations in microseconds. event=dropped means the "
    "thread's buffer was full and that many records were lost.";

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

// 1回の取り出しで集めた行（スレッドをまたいで時刻の順に並べ直すため）。
struct PendingLine {
    std::uint64_t tick;
    std::uint32_t offset;
    std::uint32_t length;
};

class Backend {
public:
    bool start(const Config& config);
    void stop();
    void flush(bool stats);
    void wake() noexcept;
    void emergency(std::string_view reason, const std::string& extraJson) noexcept;

    std::atomic<bool> running{false};
    std::filesystem::path path;
    std::mutex controlMutex;  // start・stop を1つずつ

private:
    void run();
    void drain(bool emergency);
    void emitStats(bool emergency);
    void writeOut();
    bool openFile();
    void writeSessionStart();
    void writeSessionEnd(bool crashed);

    Config config_;
    std::unique_ptr<Clock> clock_;
    std::unique_ptr<detail::LineFormatter> formatter_;

    std::timed_mutex io_;  // 取り出し・書き出しの間
    std::FILE* file_ = nullptr;
    std::uint64_t fileBytes_ = 0;
    std::uint32_t part_ = 1;
    std::string baseName_;
    std::deque<std::filesystem::path> files_;
    std::string out_;
    std::string lines_;
    std::vector<PendingLine> pending_;

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

bool Backend::openFile() {
    std::error_code error;
    std::filesystem::create_directories(config_.directory, error);
    std::string name = baseName_;
    if (part_ > 1) {
        name += "_part" + std::to_string(part_);
    }
    name += ".jsonl";
    path = config_.directory / std::filesystem::path(std::u8string(name.begin(), name.end()));
#if defined(_WIN32)
    file_ = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
#else
    file_ = std::fopen(path.c_str(), "wb");
#endif
    if (!file_) {
        return false;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
    fileBytes_ = 0;
    files_.push_back(path);
    while (config_.maxFiles > 0 && files_.size() > config_.maxFiles) {
        std::filesystem::remove(files_.front(), error);
        files_.pop_front();
    }
    return true;
}

bool Backend::start(const Config& config) {
    std::lock_guard control(controlMutex);
    if (running.load()) {
        return false;
    }
    config_ = config;
    detail::setThreadBufferBytes(config_.threadBufferBytes);
    detail::g_level.store(static_cast<std::uint8_t>(config_.level), std::memory_order_relaxed);
    clock_ = std::make_unique<Clock>();
    formatter_ = std::make_unique<detail::LineFormatter>(*clock_, config_.fullPath);

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
    part_ = 1;
    files_.clear();
    if (!openFile()) {
        return false;
    }
    records_ = 0;
    dropped_ = 0;
    lastStatTick_ = ticks();
    lastStatFrame_ = detail::g_frame.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(io_);
        writeSessionStart();
        writeOut();
        std::fflush(file_);
    }
    static std::once_flag registerExit;
    std::call_once(registerExit, [] { std::atexit(stopAtExit); });
    if (config_.installCrashHandler) {
        detail::installCrashHandler();
    }
    running.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void Backend::stop() {
    std::lock_guard control(controlMutex);
    if (!running.exchange(false)) {
        return;
    }
    wake();
    if (thread_.joinable()) {
        thread_.join();
    }
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
    std::unique_lock lock(wakeMutex_);
    const std::uint64_t ticket = ++flushRequested_;
    wakeRequested_ = true;
    statsRequested_ = statsRequested_ || stats;
    wakeCondition_.notify_one();
    flushedCondition_.wait(lock, [&] { return flushDone_ >= ticket || !running.load(); });
}

void Backend::run() {
    const auto poll = std::chrono::milliseconds(std::max<std::uint32_t>(config_.pollIntervalMs, 1));
    while (true) {
        std::uint64_t ticket;
        bool stats;
        bool stopping;
        {
            std::unique_lock lock(wakeMutex_);
            wakeCondition_.wait_for(lock, poll, [&] { return wakeRequested_ || !running.load(); });
            wakeRequested_ = false;
            ticket = flushRequested_;
            stats = statsRequested_;
            statsRequested_ = false;
            stopping = !running.load();
        }
        {
            std::lock_guard lock(io_);
            if (crashed_.load()) {
                return;
            }
            drain(false);
            const double elapsedMs = clock_->toNanoseconds(static_cast<double>(ticks() - lastStatTick_)) / 1.0e6;
            if (stats || stopping || elapsedMs >= config_.statIntervalMs) {
                emitStats(false);
            }
            if (stopping) {
                drain(false);
                writeSessionEnd(false);
            }
            writeOut();
            if (file_) {
                std::fflush(file_);
            }
            if (stopping && file_) {
                std::fclose(file_);
                file_ = nullptr;
            }
        }
        {
            std::lock_guard lock(wakeMutex_);
            flushDone_ = ticket;
        }
        flushedCondition_.notify_all();
        if (stopping) {
            return;
        }
    }
}

void Backend::drain(bool emergency) {
    auto entries = Registry::instance().snapshot(emergency);
    lines_.clear();
    pending_.clear();
    for (auto& entry : entries) {
        ThreadBuffer* buffer = entry.buffer;
        buffer->consume([&](const RecordHeader& header) {
            const auto offset = static_cast<std::uint32_t>(lines_.size());
            formatter_->appendRecord(lines_, header, entry.name);
            pending_.push_back({header.tick, offset, static_cast<std::uint32_t>(lines_.size() - offset)});
            if (header.site->level >= config_.consoleLevel && config_.consoleLevel != Level::Off) {
                std::string console = "[";
                console.append(levelName(header.site->level));
                console.append("] ");
                console.append(detail::sourceFileName(header.site->file, false));
                console.push_back(':');
                detail::appendUnsigned(console, header.site->line);
                console.push_back(' ');
                console.append(formatter_->lastMessage());
                console.push_back('\n');
                std::fwrite(console.data(), 1, console.size(), stderr);
            }
        });
        const std::uint64_t dropped = buffer->dropped();
        if (dropped > buffer->reportedDropped) {
            const std::uint64_t tick = ticks();
            const auto offset = static_cast<std::uint32_t>(lines_.size());
            formatter_->appendLineStart(lines_, tick);
            lines_.append(",\"lv\":\"warn\",\"event\":\"dropped\",\"th\":");
            detail::appendJsonString(lines_, entry.name);
            lines_.append(",\"count\":");
            detail::appendUnsigned(lines_, dropped - buffer->reportedDropped);
            lines_.append(",\"total\":");
            detail::appendUnsigned(lines_, dropped);
            lines_.append("}\n");
            pending_.push_back({tick, offset, static_cast<std::uint32_t>(lines_.size() - offset)});
            dropped_ += dropped - buffer->reportedDropped;
            buffer->reportedDropped = dropped;
        }
    }
    // スレッドをまたいで時刻の順に（同じ時刻ならスレッドの中の順のまま）。
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const PendingLine& a, const PendingLine& b) { return a.tick < b.tick; });
    for (const PendingLine& line : pending_) {
        out_.append(lines_, line.offset, line.length);
        if (out_.size() >= (1 << 20)) {
            writeOut();
        }
    }
    records_ += pending_.size();
}

void Backend::emitStats(bool emergency) {
    const std::uint64_t now = ticks();
    const std::uint64_t epoch = detail::g_statEpoch.fetch_add(1, std::memory_order_acq_rel);
    clock_->recalibrate();

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
        formatter_->appendLineStart(out_, now);
        out_.append(",\"lv\":\"stat\",\"stat\":");
        detail::appendJsonString(out_, name);
        std::string source(detail::sourceFileName(site.file, config_.fullPath));
        source.push_back(':');
        detail::appendUnsigned(source, site.line);
        out_.append(",\"src\":");
        detail::appendJsonString(out_, source);
        out_.append(",\"n\":");
        detail::appendUnsigned(out_, total.count);
        if (site.kind != StatKind::Count) {
            out_.append(",\"sum\":");
            detail::appendDouble(out_, total.sum * scale);
            out_.append(",\"avg\":");
            detail::appendDouble(out_, total.sum * scale / static_cast<double>(total.count));
            out_.append(",\"min\":");
            detail::appendDouble(out_, total.minimum * scale);
            out_.append(",\"max\":");
            detail::appendDouble(out_, total.maximum * scale);
            out_.append(",\"last\":");
            detail::appendDouble(out_, total.last * scale);
        }
        if (site.kind == StatKind::Duration) {
            out_.append(",\"unit\":\"us\"");
        }
        out_.append(",\"total\":");
        detail::appendUnsigned(out_, total.total);
        out_.append(",\"threads\":");
        detail::appendUnsigned(out_, total.threads);
        out_.append(",\"period_ms\":");
        detail::appendUnsigned(out_, static_cast<std::uint64_t>(periodMs + 0.5));
        if (frame != lastStatFrame_) {
            out_.append(",\"frames\":");
            detail::appendUnsigned(out_, frame - lastStatFrame_);
        }
        out_.append("}\n");
    }
    lastStatTick_ = now;
    lastStatFrame_ = frame;
    if (!emergency) {
        Registry::instance().removeRetired(finished);
    }
}

void Backend::writeOut() {
    if (out_.empty() || !file_) {
        out_.clear();
        return;
    }
    std::fwrite(out_.data(), 1, out_.size(), file_);
    fileBytes_ += out_.size();
    out_.clear();
    if (config_.maxFileBytes > 0 && fileBytes_ >= config_.maxFileBytes) {
        std::fclose(file_);
        file_ = nullptr;
        ++part_;
        if (openFile()) {
            writeSessionStart();  // 新しいファイルの頭にも、何のログかを書く
            writeOut();
        }
    }
}

void Backend::writeSessionStart() {
    formatter_->appendLineStart(out_, ticks());
    out_.append(",\"lv\":\"info\",\"event\":\"session.start\",\"pid\":");
    detail::appendSigned(out_, processId());
    out_.append(",\"exe\":");
    detail::appendJsonString(out_, pathUtf8(executablePath()));
    out_.append(",\"part\":");
    detail::appendUnsigned(out_, part_);
    out_.append(",\"level\":\"");
    out_.append(levelName(static_cast<Level>(detail::g_level.load())));
    out_.append("\",\"stat_interval_ms\":");
    detail::appendUnsigned(out_, config_.statIntervalMs);
    out_.append(",\"thread_buffer_bytes\":");
    detail::appendUnsigned(out_, config_.threadBufferBytes);
    out_.append(",\"about\":");
    detail::appendJsonString(out_, kAbout);
    out_.append("}\n");
}

void Backend::writeSessionEnd(bool crashed) {
    formatter_->appendLineStart(out_, ticks());
    out_.append(crashed ? ",\"lv\":\"fatal\"" : ",\"lv\":\"info\"");
    out_.append(",\"event\":\"session.end\",\"crashed\":");
    out_.append(crashed ? "true" : "false");
    out_.append(",\"records\":");
    detail::appendUnsigned(out_, records_);
    out_.append(",\"dropped\":");
    detail::appendUnsigned(out_, dropped_);
    out_.append("}\n");
}

void Backend::emergency(std::string_view reason, const std::string& extraJson) noexcept {
    if (crashed_.exchange(true) || !file_) {
        return;
    }
    // 裏のスレッドが書いている途中なら少し待つ。取れなくても書く（落ちる前に残すことを優先する）。
    const bool locked = io_.try_lock_for(std::chrono::milliseconds(500));
    try {
        writeOut();
        // まず落ちたことと場所を書いて、ディスクへ出す（この後の取り出しで落ちても残るように）。
        formatter_->appendLineStart(out_, ticks());
        out_.append(",\"lv\":\"fatal\",\"event\":\"crash\",\"reason\":");
        detail::appendJsonString(out_, reason);
        out_.append(",\"f\":");
        detail::appendUnsigned(out_, detail::g_frame.load(std::memory_order_relaxed));
        out_.append(extraJson);
        out_.append("}\n");
        writeOut();
        std::fflush(file_);
        drain(true);
        emitStats(true);
        writeSessionEnd(true);
        writeOut();
        if (file_) {
            std::fflush(file_);
        }
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

bool isRunning() noexcept {
    return backend().running.load();
}

std::filesystem::path currentFile() {
    std::lock_guard control(backend().controlMutex);
    return backend().path;
}

void setLevel(Level level) noexcept {
    detail::g_level.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
}

Level level() noexcept {
    return static_cast<Level>(detail::g_level.load(std::memory_order_relaxed));
}

void setThreadName(std::string_view name) {
    Registry::instance().setName(detail::threadBuffer(), name);
}

void nextFrame() noexcept {
    detail::g_frame.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t frame() noexcept {
    return detail::g_frame.load(std::memory_order_relaxed);
}

}  // namespace eco::log
