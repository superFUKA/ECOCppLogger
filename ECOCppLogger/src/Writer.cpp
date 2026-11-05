#include "Writer.h"

#include <algorithm>
#include <cstring>

#include "ECOCppLogger/Logger.h"
#include "Registry.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace lg::detail {

namespace {

int64_t processId() {
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

std::string toUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// "physics,ai" → {"physics", "ai"}
std::vector<std::string> splitList(const char* text) {
    std::vector<std::string> list;
    if (!text) {
        return list;
    }
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

}  // namespace

Writer& writer() {
    // わざと消さない：終了時・落ちている最中にも使うため。
    static Writer* instance = new Writer();
    return *instance;
}

// ---- 書き出し先の判定 -----------------------------------------------------------------------------------

bool Writer::Output::acceptsSite(uint32_t id, const Site& site, bool batch) {
    if (acceptsAll) {
        return true;
    }
    if (accepts.size() <= id) {
        accepts.resize(id + 1024, 0);
    }
    if (accepts[id] == 0) {
        const std::string_view type = site.type ? site.type : types::Log.name;
        const bool included = types.empty() || listed(types, type) || (batch && listed(types, "batch"));
        const bool excluded = listed(excludeTypes, type) || (batch && listed(excludeTypes, "batch"));
        accepts[id] = included && !excluded && site.level >= level ? 1 : -1;
    }
    return accepts[id] > 0;
}

bool Writer::Output::acceptsEvent(EventKind kind) const {
    if (kind == EventKind::Event) {
        return true;  // 始め・終わり・落ちた・捨てた は、どの書き出し先にも残す
    }
    const std::string_view type = kind == EventKind::Stat ? "stat" : "history";
    return (types.empty() || listed(types, type)) && !listed(excludeTypes, type) && Level::Info >= level;
}

// ---- フレーム ---------------------------------------------------------------------------------------------

void Writer::appendFrame(Output& output, file::FrameType type,
                         std::initializer_list<std::pair<const void*, size_t>> parts) {
    size_t size = 0;
    for (const auto& part : parts) {
        size += part.second;
    }
    const file::FrameHeader header{type, static_cast<uint32_t>(size)};
    output.out.append(reinterpret_cast<const char*>(&header), sizeof(header));
    for (const auto& part : parts) {
        output.out.append(static_cast<const char*>(part.first), part.second);
    }
}

void Writer::writeClock(Output& output) {
    const file::ClockPayload payload{clock_->baseTick(), clock_->baseUnixNanoseconds(), clock_->ticksPerNanosecond(),
                                     localUtcOffsetMinutes(), 0};
    appendFrame(output, file::FrameType::Clock, {{&payload, sizeof(payload)}});
}

void Writer::ensureSite(Output& output, uint32_t id) {
    if (id < output.knownSites.size() && output.knownSites[id]) {
        return;
    }
    const Site* site = Registry::instance().site(id);
    if (!site) {
        return;
    }
    // 型の名前に、このファイルの中の連番を振る（初めての名前なら TypeDef を先に書く）。
    const std::string_view typeName = site->type ? site->type : types::Log.name;
    auto [type, inserted] =
        output.typeIds.try_emplace(std::string(typeName), static_cast<uint32_t>(output.typeIds.size() + 1));
    if (inserted) {
        appendFrame(output, file::FrameType::TypeDef, {{&type->second, 4}, {typeName.data(), typeName.size()}});
    }
    const file::SiteDefPayload payload{id,
                                       site->line,
                                       site->samplingValue,
                                       type->second,
                                       static_cast<uint8_t>(site->level),
                                       static_cast<uint8_t>(site->sampling),
                                       site->argCount,
                                       site->structCount,
                                       site->tagCount,
                                       0,
                                       0};
    const std::string_view format = site->format;
    const std::string_view fileName = site->file;
    const std::string_view function = site->function;
    const auto formatLength = static_cast<uint32_t>(format.size());
    const auto fileLength = static_cast<uint32_t>(fileName.size());
    const auto functionLength = static_cast<uint32_t>(function.size());
    std::string structNames;  // ECO_LOG_FIELDS の型の項目名（uint32 の長さ＋バイト列 を個数ぶん）
    for (uint32_t i = 0; i < site->structCount; ++i) {
        const std::string_view names = site->structNames[i] ? site->structNames[i] : "";
        const auto length = static_cast<uint32_t>(names.size());
        structNames.append(reinterpret_cast<const char*>(&length), 4);
        structNames.append(names);
    }
    appendFrame(output, file::FrameType::SiteDef,
                {{&payload, sizeof(payload)},
                 {site->tags, site->tagCount},
                 {&formatLength, 4},
                 {format.data(), formatLength},
                 {&fileLength, 4},
                 {fileName.data(), fileLength},
                 {&functionLength, 4},
                 {function.data(), functionLength},
                 {structNames.data(), structNames.size()}});
    if (output.knownSites.size() <= id) {
        output.knownSites.resize(id + 1024, false);
    }
    output.knownSites[id] = true;
}

void Writer::ensureThread(Output& output, uint32_t index, uint64_t osThreadId, const std::string& name) {
    auto known = output.knownThreads.find(index);
    if (known == output.knownThreads.end() || known->second != name) {
        const file::ThreadDefPayload payload{index, 0, osThreadId};
        appendFrame(output, file::FrameType::ThreadDef, {{&payload, sizeof(payload)}, {name.data(), name.size()}});
        output.knownThreads[index] = name;
    }
}

void Writer::writeEventTo(Output& output, uint64_t tick, std::string_view body) {
    appendFrame(output, file::FrameType::Event, {{&tick, 8}, {body.data(), body.size()}});
}

void Writer::writeEvent(EventKind kind, uint64_t tick, std::string_view body) {
    for (auto& output : outputs_) {
        if (output->acceptsEvent(kind)) {
            writeEventTo(*output, tick, body);
        }
    }
}

// ---- ファイル ---------------------------------------------------------------------------------------------

bool Writer::openFile(Output& output) {
    std::error_code error;
    std::filesystem::create_directories(output.directory, error);
    std::string name = baseName_;
    if (output.part > 1) {
        name += "_part" + std::to_string(output.part);
    }
    name += ".ecolog";
    output.path = output.directory / fromUtf8(name);
    output.pathUtf8 = toUtf8(output.path);
#if defined(_WIN32)
    output.file = _wfsopen(output.path.c_str(), L"wb", _SH_DENYWR);  // ほかのプログラムが読むのは許す
#else
    output.file = fopen(output.path.c_str(), "wb");
#endif
    if (!output.file) {
        return false;
    }
    setvbuf(output.file, nullptr, _IOFBF, 1 << 20);
    output.fileBytes = 0;
    output.files.push_back(output.path);
    while (config_.maxFiles > 0 && output.files.size() > config_.maxFiles) {
        std::filesystem::remove(output.files.front(), error);
        output.files.pop_front();
    }
    // ファイルごとに読めるように、頭・時刻の換算・始めを書き、呼び出し位置・型・スレッドは書き直す。
    output.knownSites.clear();
    output.typeIds.clear();
    output.knownThreads.clear();
    output.out.append(file::kMagic, sizeof(file::kMagic));
    output.out.append(reinterpret_cast<const char*>(&file::kVersion), sizeof(file::kVersion));
    writeClock(output);
    writeSessionStart(output);
    return true;
}

void Writer::writeOutFile(Output& output) {
    if (output.out.empty() || !output.file) {
        output.out.clear();
        return;
    }
    fwrite(output.out.data(), 1, output.out.size(), output.file);
    output.fileBytes += output.out.size();
    output.out.clear();
    if (config_.maxFileBytes > 0 && output.fileBytes >= config_.maxFileBytes) {
        fclose(output.file);
        output.file = nullptr;
        ++output.part;
        if (openFile(output)) {
            writeOutFile(output);
        }
    }
}

void Writer::flushAll(bool close) {
    for (auto& output : outputs_) {
        writeOutFile(*output);
        if (output->file) {
            fflush(output->file);
            if (close) {
                fclose(output->file);
                output->file = nullptr;
            }
        }
    }
}

void Writer::writeSessionStart(Output& output) {
    event_ = "\"lv\":\"info\",\"event\":\"session.start\",\"pid\":";
    appendSigned(event_, processId());
    event_.append(",\"exe\":");
    appendJsonString(event_, toUtf8(executablePath()));
    if (!output.name.empty()) {
        event_.append(",\"destination\":");
        appendJsonString(event_, output.name);
    }
    event_.append(",\"part\":");
    appendUnsigned(event_, output.part);
    event_.append(",\"level\":\"");
    event_.append(levelName(static_cast<Level>(g_level.load())));
    event_.append("\",\"record_time\":");
    event_.append(config_.recordTime ? "true" : "false");
    event_.append(",\"thread_buffer_bytes\":");
    appendUnsigned(event_, config_.threadBufferBytes);
    writeEventTo(output, ticks(), event_);
}

void Writer::writeSessionEnd(bool crashed) {
    event_ = crashed ? "\"lv\":\"fatal\"" : "\"lv\":\"info\"";
    event_.append(",\"event\":\"session.end\",\"crashed\":");
    event_.append(crashed ? "true" : "false");
    event_.append(",\"records\":");
    appendUnsigned(event_, records_);
    event_.append(",\"dropped\":");
    appendUnsigned(event_, dropped_);
    writeEvent(EventKind::Event, ticks(), event_);
}

// ---- 始める・止める・書き出す ------------------------------------------------------------------------------

bool Writer::start(const Config& config) {
    std::lock_guard lock(io_);
    if (running_) {
        return false;
    }
    config_ = config;
    directory_ = config.directory ? config.directory : "logs";
    config_.directory = directory_.c_str();
    clock_ = std::make_unique<Clock>();
    console_.reset();
    if (config_.consoleLevel != Level::Off) {
        console_ = std::make_unique<LineFormatter>(OutputFormat::Text, false);
    }

    std::string stem = config.stem && *config.stem ? config.stem : toUtf8(executablePath().stem());
    if (stem.empty()) {
        stem = "log";
    }
    std::string started;
    appendTimestamp(started, clock_->toUnixNanoseconds(ticks()), localUtcOffsetMinutes());
    // "2026-11-05T21:03:04.123456+09:00" → "20261105-210304"
    baseName_ = stem + "_" + started.substr(0, 4) + started.substr(5, 2) + started.substr(8, 2) + "-" +
                started.substr(11, 2) + started.substr(14, 2) + started.substr(17, 2) + "_" +
                std::to_string(processId());
    records_ = 0;
    dropped_ = 0;

    outputs_.clear();
    const Destination all{};
    const Destination* destinations = config.destinationCount > 0 ? config.destinations : &all;
    const uint32_t count = config.destinationCount > 0 ? config.destinationCount : 1;
    for (uint32_t i = 0; i < count; ++i) {
        const Destination& destination = destinations[i];
        auto output = std::make_unique<Output>();
        output->name = destination.name ? destination.name : "";
        output->level = destination.level;
        output->types = splitList(destination.types);
        output->excludeTypes = splitList(destination.excludeTypes);
        output->acceptsAll = output->level == Level::Trace && output->types.empty() && output->excludeTypes.empty();
        output->directory = output->name.empty() ? fromUtf8(directory_) : fromUtf8(directory_) / fromUtf8(output->name);
        if (!openFile(*output)) {
            outputs_.clear();
            return false;
        }
        outputs_.push_back(std::move(output));
    }
    flushAll(false);
    running_ = true;
    return true;
}

void Writer::stop() {
    std::lock_guard lock(io_);
    if (!running_) {
        return;
    }
    running_ = false;
    drain(false);
    writeSessionEnd(false);
    flushAll(true);
}

void Writer::writeOut() {
    std::lock_guard lock(io_);
    if (!running_) {
        return;
    }
    drain(false);
    flushAll(false);
}

const char* Writer::currentFile(const char* destination) {
    std::lock_guard lock(io_);
    for (auto& output : outputs_) {
        if (!destination || output->name == destination) {
            return output->pathUtf8.c_str();
        }
    }
    return nullptr;
}

void Writer::writeConsole(const RecordHeader& header, std::string_view thread) {
    const Site* site = Registry::instance().site(header.site);
    if (!site || site->level < config_.consoleLevel) {
        return;
    }
    std::string line;
    console_->setTimeBase(
        {clock_->baseTick(), clock_->baseUnixNanoseconds(), clock_->ticksPerNanosecond(), localUtcOffsetMinutes()});
    const RecordView view = viewRecord(header);
    console_->appendRecord(line, view.tick, 0, view.aux, *site, view.payload, thread);
    fwrite(line.data(), 1, line.size(), stderr);
}

// すべてのスレッドのバッファを書き写す。書き出し先が絞り込みなしなら中身をそのまま、絞り込みありなら合う記録だけ。
void Writer::drain(bool emergency) {
    Registry& registry = Registry::instance();
    std::vector<ThreadBuffer*> finished;
    for (auto& entry : registry.snapshot(emergency)) {
        ThreadBuffer* buffer = entry.buffer;
        const bool retired = buffer->retired.load(std::memory_order_acquire);
        const file::BlockPayload block{buffer->index(), 0, ticks()};
        buffer->consume([&](const unsigned char* data, size_t length) {
            for (size_t offset = 0; offset < length;) {
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
                    uint32_t lastSite = 0;
                    for (size_t offset = 0; offset < length;) {
                        const auto& header = *reinterpret_cast<const RecordHeader*>(data + offset);
                        if (header.site != 0 && header.site != lastSite) {
                            ensureSite(output, header.site);
                            lastSite = header.site;
                        }
                        offset += header.size;
                    }
                    ensureThread(output, buffer->index(), buffer->osThreadId(), entry.name);
                    appendFrame(output, file::FrameType::Block, {{&block, sizeof(block)}, {data, length}});
                } else {
                    // 絞り込みあり：要る記録（と、ログでない印）だけを並べ直す。文字列にはしない。
                    output.filtered.clear();
                    bool anyRecord = false;
                    for (size_t offset = 0; offset < length;) {
                        const auto& header = *reinterpret_cast<const RecordHeader*>(data + offset);
                        bool keep = header.kind != RecordKind::Padding && header.site == 0;
                        if (header.site != 0) {
                            const Site* site = registry.site(header.site);
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
                        ensureThread(output, buffer->index(), buffer->osThreadId(), entry.name);
                    }
                    if (!output.filtered.empty()) {
                        appendFrame(output, file::FrameType::Block,
                                    {{&block, sizeof(block)}, {output.filtered.data(), output.filtered.size()}});
                    }
                }
                if (output.out.size() >= (size_t{4} << 20)) {
                    writeOutFile(output);
                }
            }
        });
        const uint64_t dropped = buffer->dropped();
        if (dropped > buffer->reportedDropped) {
            event_ = "\"lv\":\"warn\",\"event\":\"dropped\",\"th\":";
            appendJsonString(event_, entry.name);
            event_.append(",\"count\":");
            appendUnsigned(event_, dropped - buffer->reportedDropped);
            event_.append(",\"total\":");
            appendUnsigned(event_, dropped);
            writeEvent(EventKind::Event, ticks(), event_);
            dropped_ += dropped - buffer->reportedDropped;
            buffer->reportedDropped = dropped;
        }
        if (retired && buffer->empty()) {
            finished.push_back(buffer);
        }
    }
    if (!emergency) {
        registry.removeRetired(finished);
    }
}

}  // namespace lg::detail
