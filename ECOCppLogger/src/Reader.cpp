#include "ECOCppLogger/Reader.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "FileFormat.h"
#include "Format.h"

namespace lg {

namespace {

namespace file = detail::file;

constexpr std::string_view kAbout =
    "JSON Lines decoded from an ECOCppLogger .ecolog file. t=local time (missing when record_time is false; then "
    "lines keep the order within each thread, and across threads only roughly), f=frame number, lv=level, "
    "type=kind of log given by the program (missing means a plain log), th=thread, src=file:line, fn=function, "
    "msg=message, args=named values of the message. sample=this site is sampled (n=call count, skipped=calls not "
    "logged since the previous line, on change=written only when the value changed, change=0 is the first value). "
    "batch=a loop logged in one block (rows=values of each iteration, us=time the block took, omitted=rows not shown). "
    "event=dropped means the thread's buffer was full and that many records were lost. event=crash has the reason, "
    "exception and stack.";

struct OwnedSite {
    Site site{};
    std::string format;
    std::string fileName;
    std::string function;
    std::string type;
    std::vector<ArgTag> tags;
    std::vector<std::string> structNames;
    std::vector<const char*> structNamePointers;
};

enum class EntryKind { Record, Batch, Frame, Event };

struct Entry {
    uint64_t order;  // 並べる時刻（時刻のない記録は書き写した時刻）
    uint64_t tick;
    EntryKind kind;
    uint32_t thread;
    const OwnedSite* site;
    std::vector<uint64_t> record;  // 記録のバイト列（8 バイトにそろえて持つ）
    std::string body;              // Event の中身
    uint64_t frame = 0;            // Frame の印
};

std::vector<std::string> splitList(const char* text) {
    std::vector<std::string> list;
    if (!text) return list;
    std::string_view rest = text;
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        std::string_view item = rest.substr(0, comma);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty()) list.emplace_back(item);
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    }
    return list;
}

bool listed(const std::vector<std::string>& list, std::string_view name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

Level eventLevel(std::string_view body) {
    if (body.find("\"lv\":\"fatal\"") != std::string_view::npos) return Level::Fatal;
    if (body.find("\"lv\":\"error\"") != std::string_view::npos) return Level::Error;
    if (body.find("\"lv\":\"warn\"") != std::string_view::npos) return Level::Warn;
    return Level::Info;
}

std::string_view eventType(std::string_view body) {
    if (body.find("\"lv\":\"stat\"") != std::string_view::npos) return "stat";
    if (body.find("\"lv\":\"history\"") != std::string_view::npos) return "history";
    return "event";
}

template <class T>
T readAt(const char*& cursor) {
    T value;
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return value;
}

std::string readString(const char*& cursor, const char* end) {
    if (end - cursor < 4) return {};
    const auto length = readAt<uint32_t>(cursor);
    if (static_cast<size_t>(end - cursor) < length) return {};
    std::string text(cursor, length);
    cursor += length;
    return text;
}

}  // namespace

struct LogReader::State {
    ViewFormat format;
    detail::LineFormatter formatter;
    Level level;
    std::vector<std::string> threadNames;
    std::vector<std::string> types;
    std::vector<std::string> excludeTypes;
    std::string contains;
    uint64_t firstFrame;
    uint64_t lastFrame;
    uint32_t batchRows;

    std::string pending;
    bool headerRead = false;
    bool invalid = false;
    std::unordered_map<uint32_t, std::unique_ptr<OwnedSite>> sites;
    std::unordered_map<uint32_t, std::string> typeNames;
    std::unordered_map<uint32_t, std::string> threads;
    std::vector<Entry> entries;
    uint64_t currentFrame = 0;
    std::string out;

    State(ViewFormat format_, const ViewFilter& filter)
        : format(format_),
          formatter(format_ == ViewFormat::Json ? detail::OutputFormat::Json : detail::OutputFormat::Text,
                    filter.fullPath),
          level(filter.level),
          threadNames(splitList(filter.threads)),
          types(splitList(filter.types)),
          excludeTypes(splitList(filter.excludeTypes)),
          contains(filter.contains ? filter.contains : ""),
          firstFrame(filter.firstFrame),
          lastFrame(filter.lastFrame),
          batchRows(filter.batchRows) {}

    bool typeSelected(std::string_view type, bool batch) const {
        const bool included = types.empty() || listed(types, type) || (batch && listed(types, "batch"));
        const bool excluded = listed(excludeTypes, type) || (batch && listed(excludeTypes, "batch"));
        return included && !excluded;
    }

    void frame(file::FrameType type, const char* data, size_t size);
};

void LogReader::State::frame(file::FrameType type, const char* data, size_t size) {
    const char* cursor = data;
    const char* end = data + size;
    switch (type) {
    case file::FrameType::Clock: {
        if (size < sizeof(file::ClockPayload)) return;
        const auto clock = readAt<file::ClockPayload>(cursor);
        formatter.setTimeBase({clock.tick, clock.unixNanoseconds, clock.ticksPerNanosecond, clock.utcOffsetMinutes});
        return;
    }
    case file::FrameType::TypeDef: {
        if (size < 4) return;
        const auto id = readAt<uint32_t>(cursor);
        typeNames[id] = std::string(cursor, end);
        return;
    }
    case file::FrameType::SiteDef: {
        if (size < sizeof(file::SiteDefPayload)) return;
        const auto payload = readAt<file::SiteDefPayload>(cursor);
        if (static_cast<size_t>(end - cursor) < payload.tagCount) return;
        auto owned = std::make_unique<OwnedSite>();
        owned->tags.assign(reinterpret_cast<const ArgTag*>(cursor),
                           reinterpret_cast<const ArgTag*>(cursor) + payload.tagCount);
        cursor += payload.tagCount;
        owned->format = readString(cursor, end);
        owned->fileName = readString(cursor, end);
        owned->function = readString(cursor, end);
        for (uint32_t i = 0; i < payload.structCount; ++i) {
            owned->structNames.push_back(readString(cursor, end));
        }
        for (const std::string& names : owned->structNames) {
            owned->structNamePointers.push_back(names.c_str());
        }
        auto typeName = typeNames.find(payload.type);
        owned->type = typeName != typeNames.end() ? typeName->second : std::string(types::Log.name);
        owned->tags.push_back(ArgTag::None);
        owned->site = Site{static_cast<Level>(payload.level),
                           owned->type.c_str(),
                           owned->format.c_str(),
                           owned->fileName.c_str(),
                           owned->function.c_str(),
                           payload.line,
                           owned->tags.data(),
                           payload.argCount,
                           static_cast<Sampling>(payload.sampling),
                           payload.samplingValue,
                           payload.tagCount,
                           owned->structNamePointers.data(),
                           static_cast<uint8_t>(owned->structNamePointers.size())};
        sites[payload.id] = std::move(owned);
        return;
    }
    case file::FrameType::ThreadDef: {
        if (size < sizeof(file::ThreadDefPayload)) return;
        const auto payload = readAt<file::ThreadDefPayload>(cursor);
        threads[payload.index] = std::string(cursor, end);
        return;
    }
    case file::FrameType::Block: {
        if (size < sizeof(file::BlockPayload)) return;
        const auto block = readAt<file::BlockPayload>(cursor);
        while (end - cursor >= static_cast<std::ptrdiff_t>(sizeof(RecordHeader))) {
            RecordHeader header;
            std::memcpy(&header, cursor, sizeof(header));
            if (header.size < sizeof(RecordHeader) || header.size > static_cast<size_t>(end - cursor)) {
                break;  // 壊れている
            }
            if (header.kind != RecordKind::Padding) {
                std::vector<uint64_t> record((header.size + 7) / 8);
                std::memcpy(record.data(), cursor, header.size);
                const RecordView view = viewRecord(*reinterpret_cast<const RecordHeader*>(record.data()));
                const uint64_t order = view.tick != 0 ? view.tick : block.drainTick;
                if (header.kind == RecordKind::Frame) {
                    Entry entry{order, view.tick, EntryKind::Frame, block.thread, nullptr, {}, {}};
                    std::memcpy(&entry.frame, view.payload, 8);
                    entries.push_back(std::move(entry));
                } else if (header.kind == RecordKind::Log || header.kind == RecordKind::Batch) {
                    auto found = sites.find(header.site);
                    const bool batch = header.kind == RecordKind::Batch;
                    if (found != sites.end() && found->second->site.level >= level &&
                        typeSelected(found->second->type, batch)) {
                        entries.push_back(Entry{order, view.tick, batch ? EntryKind::Batch : EntryKind::Record,
                                                block.thread, found->second.get(), std::move(record), {}});
                    }
                }
            }
            cursor += header.size;
        }
        return;
    }
    case file::FrameType::Event: {
        if (size < 8) return;
        const auto tick = readAt<uint64_t>(cursor);
        std::string body(cursor, end);
        if (eventLevel(body) < level || !typeSelected(eventType(body), false)) return;
        entries.push_back(Entry{tick, tick, EntryKind::Event, 0, nullptr, {}, std::move(body)});
        return;
    }
    }
}

LogReader::LogReader(ViewFormat format, const ViewFilter& filter) : state_(new State(format, filter)) {}

LogReader::~LogReader() {
    delete state_;
}

bool LogReader::invalid() const noexcept {
    return state_->invalid;
}

void LogReader::feed(const void* data, size_t size) {
    State& s = *state_;
    if (s.invalid) return;
    s.pending.append(static_cast<const char*>(data), size);
    size_t offset = 0;
    if (!s.headerRead) {
        if (s.pending.size() < sizeof(file::kMagic) + 4) return;
        uint32_t version = 0;
        std::memcpy(&version, s.pending.data() + sizeof(file::kMagic), 4);
        if (std::memcmp(s.pending.data(), file::kMagic, sizeof(file::kMagic)) != 0 || version != file::kVersion) {
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

const char* LogReader::take(size_t& size) {
    State& s = *state_;
    s.out.clear();
    std::stable_sort(s.entries.begin(), s.entries.end(),
                     [](const Entry& a, const Entry& b) { return a.order < b.order; });
    std::string line;
    for (const Entry& entry : s.entries) {
        if (entry.kind == EntryKind::Frame) {
            s.currentFrame = entry.frame;  // ここから後の記録はこのフレーム
            continue;
        }
        if (entry.kind != EntryKind::Event && (s.currentFrame < s.firstFrame || s.currentFrame > s.lastFrame)) {
            continue;
        }
        line.clear();
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
            if (!s.threadNames.empty() && !listed(s.threadNames, thread)) continue;
            const auto* header = reinterpret_cast<const RecordHeader*>(entry.record.data());
            const RecordView view = viewRecord(*header);
            if (entry.kind == EntryKind::Record) {
                s.formatter.appendRecord(line, view.tick, s.currentFrame, view.aux, entry.site->site, view.payload,
                                         thread);
            } else {
                const size_t used = static_cast<size_t>(view.payload - reinterpret_cast<const unsigned char*>(header));
                s.formatter.appendBatch(line, view.tick, s.currentFrame, entry.site->site, view.payload,
                                        header->size > used ? header->size - used : 0, thread, s.batchRows);
            }
        }
        if (!s.contains.empty() && line.find(s.contains) == std::string::npos) continue;
        s.out.append(line);
    }
    s.entries.clear();
    size = s.out.size();
    return s.out.c_str();
}

bool readLog(const char* path, ViewFormat format, const ViewFilter& filter, TextSink sink, void* user) {
    if (!path || !sink) return false;
    std::ifstream input(std::filesystem::path(std::u8string(path, path + std::strlen(path))), std::ios::binary);
    if (!input) return false;
    LogReader reader(format, filter);
    std::vector<char> buffer(size_t{1} << 20);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        reader.feed(buffer.data(), static_cast<size_t>(input.gcount()));
    }
    if (reader.invalid()) return false;
    size_t size = 0;
    const char* text = reader.take(size);
    sink(text, size, user);
    return true;
}

const char* latestLog(const char* directory) {
    static thread_local std::string latest;
    latest.clear();
    if (!directory) return nullptr;
    std::filesystem::file_time_type latestTime{};
    std::error_code error;
    const std::filesystem::path folder(std::u8string(directory, directory + std::strlen(directory)));
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() != ".ecolog") continue;
        const auto time = entry.last_write_time(error);
        if (latest.empty() || time > latestTime) {
            const auto text = entry.path().u8string();
            latest.assign(reinterpret_cast<const char*>(text.data()), text.size());
            latestTime = time;
        }
    }
    return latest.empty() ? nullptr : latest.c_str();
}

}  // namespace lg
