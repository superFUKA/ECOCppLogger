#include "ECOCppLogger/Reader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "FileFormat.h"
#include "Output.h"

namespace eco::log {

namespace {

namespace file = detail::file;

constexpr std::string_view kAbout =
    "JSON Lines decoded from an ECOCppLogger .ecolog file. t=local time (missing when record_time is false; "
    "then lines are ordered per thread, and across threads only roughly), f=frame number, lv=level, th=thread, "
    "src=file:line, fn=function, msg=message. sample=this site is sampled (n=call count, skipped=calls not logged "
    "since the previous line; on change=written only when the value changed, change=0 is the first value). "
    "lv=stat lines aggregate ECO_STAT/ECO_COUNT/ECO_SCOPE_TIME over period_ms: n=calls in the period, "
    "total=calls since start, unit=us means durations in microseconds. lv=history lines list the most recent "
    "values of ECO_HISTORY kept in memory, written on crash or dumpHistory(reason); points are "
    "[f, ms before t, value], oldest first. event=dropped means the thread's buffer was full and that many "
    "records were lost. event=crash has the reason, exception and stack. type=the kind of log given by the "
    "program (e.g. physics, ai, assert); missing means a plain log. batch lines (ECO_LOG_BATCH) hold many entries "
    "of one format: msg is the format, rows are the values of each entry in order, duration_us spans them.";

// 読んだ呼び出し位置（Site が指す文字列はここで持つ）。
struct OwnedSite {
    Site site{};
    std::string format;
    std::string fileName;
    std::string function;
    std::string type;
    std::vector<ArgTag> tags;
};

// Event の型（絞り込み用）：集計・最近の値・それ以外。
std::string_view eventType(std::string_view body) {
    if (body.find("\"lv\":\"stat\"") != std::string_view::npos) return "stat";
    if (body.find("\"lv\":\"history\"") != std::string_view::npos) return "history";
    return "event";
}

bool listed(const std::vector<std::string>& list, std::string_view name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

bool typeSelected(const ViewFilter& filter, std::string_view type) {
    return (filter.types.empty() || listed(filter.types, type)) && !listed(filter.excludeTypes, type);
}

// 記録の型での判定（書き出し先の絞り込みと同じ：まとめ書きは自分の型に加えて "batch" でもある）。
bool recordSelected(const ViewFilter& filter, std::string_view type, bool batch) {
    const bool included =
        filter.types.empty() || listed(filter.types, type) || (batch && listed(filter.types, "batch"));
    const bool excluded = listed(filter.excludeTypes, type) || (batch && listed(filter.excludeTypes, "batch"));
    return included && !excluded;
}

enum class EntryKind { Record, Batch, Frame, Event };

struct Entry {
    std::uint64_t order;  // 並べる時刻（時刻のない記録は、書き写した時刻）
    std::uint64_t tick;   // 時刻（0 ならなし）
    EntryKind kind;
    std::uint32_t thread;
    const OwnedSite* site;
    std::vector<std::uint64_t> record;  // 記録のバイト列（8 バイトにそろえて持つ）
    std::string body;                   // Event の中身
    std::uint64_t frame = 0;            // Frame の印のフレーム番号
};

Level eventLevel(std::string_view body) {
    if (body.find("\"lv\":\"fatal\"") != std::string_view::npos) return Level::Fatal;
    if (body.find("\"lv\":\"error\"") != std::string_view::npos) return Level::Error;
    if (body.find("\"lv\":\"warn\"") != std::string_view::npos) return Level::Warn;
    return Level::Info;
}

template <class T>
T read(const char*& cursor) {
    T value;
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return value;
}

std::string readString(const char*& cursor, const char* end) {
    if (end - cursor < 4) return {};
    const auto length = read<std::uint32_t>(cursor);
    if (static_cast<std::size_t>(end - cursor) < length) return {};
    std::string text(cursor, length);
    cursor += length;
    return text;
}

}  // namespace

struct LogReader::State {
    ViewFormat format;
    ViewFilter filter;
    detail::LineFormatter formatter;
    std::string pending;
    bool headerRead = false;
    bool invalid = false;
    std::unordered_map<std::uint64_t, std::unique_ptr<OwnedSite>> sites;
    std::unordered_map<std::uint32_t, std::string> threads;
    std::unordered_map<std::uint32_t, std::string> types;  // ログの型の番号 → 名前
    std::vector<Entry> entries;
    std::uint64_t currentFrame = 0;  // 時刻の順に並べたとき、最後に通った Frame の印

    State(ViewFormat format_, ViewFilter filter_)
        : format(format_), filter(std::move(filter_)),
          formatter(format_ == ViewFormat::Json ? detail::OutputFormat::Json : detail::OutputFormat::Text,
                    filter.fullPath) {}

    void frame(file::FrameType type, const char* data, std::size_t size);
};

void LogReader::State::frame(file::FrameType type, const char* data, std::size_t size) {
    const char* cursor = data;
    const char* end = data + size;
    switch (type) {
    case file::FrameType::Clock: {
        if (size < sizeof(file::ClockPayload)) return;
        const auto clock = read<file::ClockPayload>(cursor);
        formatter.setTimeBase({clock.tick, clock.unixNanoseconds, clock.ticksPerNanosecond, clock.utcOffsetMinutes});
        return;
    }
    case file::FrameType::SiteDef: {
        if (size < sizeof(file::SiteDefPayload)) return;
        const auto payload = read<file::SiteDefPayload>(cursor);
        auto owned = std::make_unique<OwnedSite>();
        if (static_cast<std::size_t>(end - cursor) < payload.argCount) return;
        owned->tags.assign(reinterpret_cast<const ArgTag*>(cursor), reinterpret_cast<const ArgTag*>(cursor) + payload.argCount);
        cursor += payload.argCount;
        owned->format = readString(cursor, end);
        owned->fileName = readString(cursor, end);
        owned->function = readString(cursor, end);
        auto type = types.find(payload.type);
        owned->type = type != types.end() ? type->second : std::string(types::Log.name);
        owned->tags.push_back(ArgTag::None);  // 空でも data() が使えるように
        owned->site = Site{static_cast<Level>(payload.level), owned->format, owned->fileName.c_str(),
                           owned->function.c_str(), payload.line, owned->tags.data(), payload.argCount,
                           payload.sampling, payload.samplingValue, owned->type.c_str()};
        sites[payload.id] = std::move(owned);
        return;
    }
    case file::FrameType::TypeDef: {
        if (size < 4) return;
        const auto id = read<std::uint32_t>(cursor);
        types[id] = std::string(cursor, end);
        return;
    }
    case file::FrameType::ThreadDef: {
        if (size < 4) return;
        const auto index = read<std::uint32_t>(cursor);
        threads[index] = std::string(cursor, end);
        return;
    }
    case file::FrameType::Block: {
        if (size < 16) return;
        const auto thread = read<std::uint32_t>(cursor);
        cursor += 4;
        const auto drainTick = read<std::uint64_t>(cursor);
        while (end - cursor >= static_cast<std::ptrdiff_t>(sizeof(RecordHeader))) {
            RecordHeader header;
            std::memcpy(&header, cursor, sizeof(header));
            if (header.size < sizeof(RecordHeader) || header.size > static_cast<std::size_t>(end - cursor)) {
                break;  // 壊れている
            }
            // 8 バイトにそろえて写してから読む（ファイルの中の位置はそろっていない）。
            std::vector<std::uint64_t> record((header.size + 7) / 8);
            std::memcpy(record.data(), cursor, header.size);
            const RecordView view = viewRecord(*reinterpret_cast<const RecordHeader*>(record.data()));
            const std::uint64_t order = view.tick != 0 ? view.tick : drainTick;
            if (header.kind == RecordKind::Frame) {
                Entry entry{order, view.tick, EntryKind::Frame, thread, nullptr, {}, {}};
                std::memcpy(&entry.frame, view.payload, 8);
                entries.push_back(std::move(entry));
            } else if (header.kind == RecordKind::Log || header.kind == RecordKind::Batch) {
                auto found = sites.find(header.site);
                if (found != sites.end() && found->second->site.level >= filter.level &&
                    recordSelected(filter, found->second->type, header.kind == RecordKind::Batch)) {
                    entries.push_back(Entry{order, view.tick,
                                            header.kind == RecordKind::Log ? EntryKind::Record : EntryKind::Batch,
                                            thread, found->second.get(), std::move(record), {}});
                }
            }
            cursor += header.size;
        }
        return;
    }
    case file::FrameType::Event: {
        if (size < 8) return;
        const auto tick = read<std::uint64_t>(cursor);
        std::string body(cursor, end);
        if (eventLevel(body) < filter.level || !typeSelected(filter, eventType(body))) return;
        entries.push_back(Entry{tick, tick, EntryKind::Event, 0, nullptr, {}, std::move(body)});
        return;
    }
    }
}

LogReader::LogReader(ViewFormat format, ViewFilter filter) : state_(std::make_unique<State>(format, std::move(filter))) {}

LogReader::~LogReader() = default;

bool LogReader::invalid() const noexcept {
    return state_->invalid;
}

void LogReader::feed(const void* data, std::size_t size) {
    State& s = *state_;
    if (s.invalid) return;
    s.pending.append(static_cast<const char*>(data), size);
    std::size_t offset = 0;
    if (!s.headerRead) {
        if (s.pending.size() < sizeof(file::kMagic) + 4) return;
        if (std::memcmp(s.pending.data(), file::kMagic, sizeof(file::kMagic)) != 0) {
            s.invalid = true;
            return;
        }
        offset = sizeof(file::kMagic) + 4;
        s.headerRead = true;
    }
    while (s.pending.size() - offset >= sizeof(file::FrameHeader)) {
        file::FrameHeader header;
        std::memcpy(&header, s.pending.data() + offset, sizeof(header));
        if (s.pending.size() - offset - sizeof(header) < header.size) {
            break;  // 書きかけ：次に回す
        }
        s.frame(header.type, s.pending.data() + offset + sizeof(header), header.size);
        offset += sizeof(header) + header.size;
    }
    s.pending.erase(0, offset);
}

void LogReader::take(std::string& out) {
    State& s = *state_;
    std::stable_sort(s.entries.begin(), s.entries.end(),
                     [](const Entry& a, const Entry& b) { return a.order < b.order; });
    std::string line;
    for (const Entry& entry : s.entries) {
        line.clear();
        if (entry.kind == EntryKind::Frame) {
            s.currentFrame = entry.frame;  // ここから後の記録はこのフレーム
            continue;
        }
        if (entry.kind == EntryKind::Event) {
            if (s.format == ViewFormat::Json && entry.body.find("\"event\":\"session.start\"") != std::string::npos) {
                std::string body = entry.body;
                body.append(",\"about\":");
                detail::appendJsonString(body, kAbout);
                s.formatter.appendEvent(line, entry.tick, body);
            } else {
                s.formatter.appendEvent(line, entry.tick, entry.body);
            }
        } else {
            auto name = s.threads.find(entry.thread);
            const std::string thread = name != s.threads.end() ? name->second : "T" + std::to_string(entry.thread);
            if (!s.filter.thread.empty() && thread != s.filter.thread) continue;
            const auto* header = reinterpret_cast<const RecordHeader*>(entry.record.data());
            const RecordView view = viewRecord(*header);
            if (entry.kind == EntryKind::Record) {
                s.formatter.appendRecord(line, view.tick, s.currentFrame, view.aux, entry.site->site, view.payload,
                                         thread);
            } else {
                BatchInfo info;
                std::memcpy(&info, view.payload, sizeof(info));
                s.formatter.appendBatch(line, view.tick, s.currentFrame, entry.site->site, info,
                                        view.payload + sizeof(info), thread, s.filter.batchRows);
            }
        }
        if (!s.filter.contains.empty() && line.find(s.filter.contains) == std::string::npos) continue;
        out.append(line);
    }
    s.entries.clear();
}

std::string readLog(const std::filesystem::path& path, ViewFormat format, const ViewFilter& filter) {
    LogReader reader(format, filter);
    std::ifstream input(path, std::ios::binary);
    std::vector<char> buffer(std::size_t{1} << 20);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        reader.feed(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    std::string out;
    reader.take(out);
    return out;
}

std::filesystem::path latestLog(const std::filesystem::path& directory) {
    std::filesystem::path latest;
    std::filesystem::file_time_type latestTime{};
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().extension() != ".ecolog") continue;
        const auto time = entry.last_write_time(error);
        if (latest.empty() || time > latestTime) {
            latest = entry.path();
            latestTime = time;
        }
    }
    return latest;
}

}  // namespace eco::log
