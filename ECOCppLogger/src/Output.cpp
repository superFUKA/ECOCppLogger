#include "Output.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <ctime>

namespace eco::log::detail {

namespace {

constexpr char kHex[] = "0123456789abcdef";

// 1970-01-01 からの日数 → 年月日（Howard Hinnant の civil_from_days）。
void civilFromDays(std::int64_t days, int& year, unsigned& month, unsigned& day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned mp = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = static_cast<int>(static_cast<std::int64_t>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0));
}

void appendPadded(std::string& out, unsigned value, int width) {
    char buffer[16];
    for (int i = width - 1; i >= 0; --i) {
        buffer[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    out.append(buffer, static_cast<std::size_t>(width));
}

template <class T>
T readValue(const std::byte*& cursor) {
    T value;
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return value;
}

// 人向けの重さの欄（幅をそろえる）。
std::string_view levelColumn(std::string_view level) {
    if (level == "trace") return "TRACE";
    if (level == "debug") return "DEBUG";
    if (level == "info") return "INFO ";
    if (level == "warn") return "WARN ";
    if (level == "error") return "ERROR";
    if (level == "fatal") return "FATAL";
    if (level == "stat") return "STAT ";
    if (level == "history") return "HIST ";
    return level;
}

// JSON の中身（1段だけ："k":v,"k":v,…）を key=value の並びにする（人向け）。lv は別に返す。
void flatJsonToText(std::string& out, std::string_view body, std::string& level) {
    std::size_t i = 0;
    bool first = true;
    while (i < body.size()) {
        if (body[i] != '"') {
            ++i;
            continue;
        }
        const std::size_t keyEnd = body.find('"', i + 1);
        if (keyEnd == std::string_view::npos || keyEnd + 1 >= body.size() || body[keyEnd + 1] != ':') {
            break;
        }
        const std::string_view key = body.substr(i + 1, keyEnd - i - 1);
        std::size_t valueStart = keyEnd + 2;
        std::size_t j = valueStart;
        int depth = 0;
        bool inString = false;
        for (; j < body.size(); ++j) {
            const char c = body[j];
            if (inString) {
                if (c == '\\') ++j;
                else if (c == '"') inString = false;
            } else if (c == '"') inString = true;
            else if (c == '[' || c == '{') ++depth;
            else if (c == ']' || c == '}') --depth;
            else if (c == ',' && depth == 0) break;
        }
        std::string_view value = body.substr(valueStart, j - valueStart);
        std::string unescaped;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            // 文字列は " を外し、\\ \" \n 等を元に戻す（人向け）
            value = value.substr(1, value.size() - 2);
            for (std::size_t k = 0; k < value.size(); ++k) {
                if (value[k] == '\\' && k + 1 < value.size()) {
                    const char escaped = value[++k];
                    unescaped.push_back(escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped);
                } else {
                    unescaped.push_back(value[k]);
                }
            }
            value = unescaped;
        }
        if (key == "lv") {
            level = value;
        } else {
            if (!first) out.push_back(' ');
            first = false;
            out.append(key);
            out.push_back('=');
            out.append(value);
        }
        i = j + 1;
    }
}

}  // namespace

void appendJsonString(std::string& out, std::string_view text) {
    out.push_back('"');
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c != '"' && c != '\\') {
            continue;
        }
        out.append(text.data() + start, i - start);
        switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            out.append("\\u00");
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
            break;
        }
        start = i + 1;
    }
    out.append(text.data() + start, text.size() - start);
    out.push_back('"');
}

void appendDouble(std::string& out, double value) {
    if (!std::isfinite(value)) {
        // JSON に NaN・Infinity はないので文字列にする。
        out.append(value != value ? "\"nan\"" : value > 0 ? "\"inf\"" : "\"-inf\"");
        return;
    }
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

void appendUnsigned(std::string& out, std::uint64_t value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

void appendSigned(std::string& out, std::int64_t value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

void appendTimestamp(std::string& out, std::int64_t unixNanoseconds, int utcOffsetMinutes) {
    const std::int64_t local = unixNanoseconds + static_cast<std::int64_t>(utcOffsetMinutes) * 60'000'000'000LL;
    std::int64_t seconds = local / 1'000'000'000LL;
    std::int64_t nanoseconds = local % 1'000'000'000LL;
    if (nanoseconds < 0) {
        nanoseconds += 1'000'000'000LL;
        --seconds;
    }
    std::int64_t days = seconds / 86400;
    std::int64_t secondOfDay = seconds % 86400;
    if (secondOfDay < 0) {
        secondOfDay += 86400;
        --days;
    }
    int year;
    unsigned month, day;
    civilFromDays(days, year, month, day);
    appendPadded(out, static_cast<unsigned>(year), 4);
    out.push_back('-');
    appendPadded(out, month, 2);
    out.push_back('-');
    appendPadded(out, day, 2);
    out.push_back('T');
    appendPadded(out, static_cast<unsigned>(secondOfDay / 3600), 2);
    out.push_back(':');
    appendPadded(out, static_cast<unsigned>(secondOfDay / 60 % 60), 2);
    out.push_back(':');
    appendPadded(out, static_cast<unsigned>(secondOfDay % 60), 2);
    out.push_back('.');
    appendPadded(out, static_cast<unsigned>(nanoseconds / 1000), 6);
    const int offset = utcOffsetMinutes < 0 ? -utcOffsetMinutes : utcOffsetMinutes;
    out.push_back(utcOffsetMinutes < 0 ? '-' : '+');
    appendPadded(out, static_cast<unsigned>(offset / 60), 2);
    out.push_back(':');
    appendPadded(out, static_cast<unsigned>(offset % 60), 2);
}

std::string_view shortFunctionName(std::string_view function) noexcept {
    // 引数の括弧の前まで（テンプレート引数の <> の中の空白・括弧は飛ばす）。
    const auto findParenthesis = [&](std::size_t from) {
        int depth = 0;
        for (std::size_t i = from; i < function.size(); ++i) {
            const char c = function[i];
            if (c == '<') ++depth;
            else if (c == '>' && depth > 0) --depth;
            else if (c == '(' && depth == 0) return i;
        }
        return function.size();
    };
    std::size_t end = findParenthesis(0);
    // 名前の部分の終わり（"operator ()" の空白で切らないように、空白を探すのはここまで）。
    std::size_t nameEnd = end;
    std::string_view head = function.substr(0, end);
    while (!head.empty() && head.back() == ' ') {
        head.remove_suffix(1);
    }
    if (head.ends_with("operator") && end + 1 < function.size() && function[end + 1] == ')') {
        nameEnd = head.size() - 8;
        end = findParenthesis(end + 2);
    }
    // 戻り値の型・呼び出し規約（最後の空白より前）を落とす。
    std::size_t begin = 0;
    int depth = 0;
    for (std::size_t i = 0; i < nameEnd; ++i) {
        const char c = function[i];
        if (c == '<') ++depth;
        else if (c == '>' && depth > 0) --depth;
        else if (c == ' ' && depth == 0) begin = i + 1;
    }
    return function.substr(begin, end - begin);
}

std::string_view sourceFileName(std::string_view file, bool fullPath) noexcept {
    if (fullPath) {
        return file;
    }
    const std::size_t slash = file.find_last_of("/\\");
    return slash == std::string_view::npos ? file : file.substr(slash + 1);
}

int localUtcOffsetMinutes() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    std::tm utc{};
#if defined(_WIN32)
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
#else
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
#endif
    local.tm_isdst = 0;
    utc.tm_isdst = 0;
    const double difference = std::difftime(std::mktime(&local), std::mktime(&utc));
    return static_cast<int>(difference / 60.0);
}

void renderMessage(std::string& out, const Site& site, const std::byte* cursor) {
    std::size_t argument = 0;
    const std::string_view format = site.format;
    for (std::size_t i = 0; i < format.size(); ++i) {
        const char c = format[i];
        if ((c == '{' || c == '}') && i + 1 < format.size() && format[i + 1] == c) {
            out.push_back(c);
            ++i;
            continue;
        }
        if (c != '{' || i + 1 >= format.size() || format[i + 1] != '}' || argument >= site.argCount) {
            out.push_back(c);
            continue;
        }
        ++i;
        switch (site.tags[argument++]) {
        case ArgTag::Bool: out.append(readValue<bool>(cursor) ? "true" : "false"); break;
        case ArgTag::Char: out.push_back(readValue<char>(cursor)); break;
        case ArgTag::Int: appendSigned(out, readValue<std::int64_t>(cursor)); break;
        case ArgTag::UInt: appendUnsigned(out, readValue<std::uint64_t>(cursor)); break;
        case ArgTag::Double: appendDouble(out, readValue<double>(cursor)); break;
        case ArgTag::String: {
            const auto stored = readValue<std::uint32_t>(cursor);
            const std::uint32_t length = stored & 0x7FFFFFFFu;
            out.append(reinterpret_cast<const char*>(cursor), length);
            cursor += length;
            if (stored & 0x80000000u) {
                out.append("…");
            }
            break;
        }
        case ArgTag::Pointer: {
            const auto address = readValue<std::uint64_t>(cursor);
            char buffer[24] = {'0', 'x'};
            const auto result = std::to_chars(buffer + 2, buffer + sizeof(buffer), address, 16);
            out.append(buffer, result.ptr);
            break;
        }
        case ArgTag::None: break;
        }
    }
}

void LineFormatter::appendTime(std::string& out, std::uint64_t tick) {
    if (tick == 0) {
        // 時刻を記録しない設定
        if (format_ == OutputFormat::Text) {
            out.append("--:--:--.------");
        }
        return;
    }
    // 秒までの部分は、同じ秒の間は使い回す。
    const std::int64_t unixNanoseconds = timeBase_.toUnixNanoseconds(tick);
    std::int64_t second = unixNanoseconds / 1'000'000'000LL;
    std::int64_t nanoseconds = unixNanoseconds % 1'000'000'000LL;
    if (nanoseconds < 0) {
        nanoseconds += 1'000'000'000LL;
        --second;
    }
    if (second != cachedSecond_ || cachedPrefix_.empty()) {
        std::string full;
        appendTimestamp(full, second * 1'000'000'000LL, timeBase_.utcOffsetMinutes);
        const std::size_t dot = full.find('.');
        if (format_ == OutputFormat::Json) {
            cachedPrefix_ = "\"t\":\"" + full.substr(0, dot + 1);
            cachedSuffix_ = full.substr(dot + 7) + "\"";
        } else {
            cachedPrefix_ = full.substr(dot - 8, 9);  // "HH:MM:SS."
            cachedSuffix_.clear();
        }
        cachedSecond_ = second;
    }
    out.append(cachedPrefix_);
    appendPadded(out, static_cast<unsigned>(nanoseconds / 1000), 6);
    out.append(cachedSuffix_);
}

void LineFormatter::appendRecord(std::string& out, std::uint64_t tick, std::uint64_t frame, std::uint64_t aux,
                                 const Site& site, const std::byte* arguments, std::string_view thread) {
    message_.clear();
    renderMessage(message_, site, arguments);
    source_.assign(sourceFileName(site.file, fullPath_));
    source_.push_back(':');
    appendUnsigned(source_, site.line);
    const auto sampling = static_cast<Sampling>(site.sampling);

    if (format_ == OutputFormat::Text) {
        appendTime(out, tick);
        if (frame != 0) {
            out.append(" f");
            appendUnsigned(out, frame);
        }
        out.push_back(' ');
        out.append(levelColumn(levelName(site.level)));
        out.append(" [");
        out.append(thread);
        out.append("] ");
        out.append(source_);
        out.push_back(' ');
        out.append(shortFunctionName(site.function));
        out.append(": ");
        out.append(message_);
        switch (sampling) {
        case Sampling::EveryN:
        case Sampling::FirstN:
            out.append(sampling == Sampling::EveryN ? "  (every " : "  (first ");
            appendUnsigned(out, site.samplingValue);
            out.append(", #");
            appendUnsigned(out, aux);
            out.push_back(')');
            break;
        case Sampling::EveryMs:
            out.append("  (every ");
            appendUnsigned(out, site.samplingValue);
            out.append("ms, skipped ");
            appendUnsigned(out, aux);
            out.push_back(')');
            break;
        case Sampling::OnChange:
        case Sampling::None: break;
        }
        out.push_back('\n');
        return;
    }

    out.push_back('{');
    const std::size_t start = out.size();
    appendTime(out, tick);
    if (frame != 0) {
        if (out.size() != start) out.push_back(',');
        out.append("\"f\":");
        appendUnsigned(out, frame);
    }
    if (out.size() != start) out.push_back(',');
    out.append("\"lv\":\"");
    out.append(levelName(site.level));
    out.append("\",\"th\":");
    appendJsonString(out, thread);
    out.append(",\"src\":");
    appendJsonString(out, source_);
    out.append(",\"fn\":");
    appendJsonString(out, shortFunctionName(site.function));
    out.append(",\"msg\":");
    appendJsonString(out, message_);
    switch (sampling) {
    case Sampling::EveryN:
    case Sampling::FirstN:
        out.append(sampling == Sampling::EveryN ? ",\"sample\":\"every " : ",\"sample\":\"first ");
        appendUnsigned(out, site.samplingValue);
        out.append("\",\"n\":");
        appendUnsigned(out, aux);
        break;
    case Sampling::EveryMs:
        out.append(",\"sample\":\"every ");
        appendUnsigned(out, site.samplingValue);
        out.append("ms\",\"skipped\":");
        appendUnsigned(out, aux);
        break;
    case Sampling::OnChange:
        out.append(",\"sample\":\"on change\",\"change\":");
        appendUnsigned(out, aux);
        break;
    case Sampling::None: break;
    }
    out.append("}\n");
}

void LineFormatter::appendBatch(std::string& out, std::uint64_t tick, std::uint64_t frame, const Site& site,
                                const BatchInfo& info, const std::byte* elements, std::string_view thread,
                                std::size_t maxRows) {
    source_.assign(sourceFileName(site.file, fullPath_));
    source_.push_back(':');
    appendUnsigned(source_, site.line);
    const double durationUs =
        tick != 0 && info.endTick > tick
            ? static_cast<double>(info.endTick - tick) / timeBase_.ticksPerNanosecond / 1000.0
            : 0.0;

    if (format_ == OutputFormat::Text) {
        appendTime(out, tick);
        if (frame != 0) {
            out.append(" f");
            appendUnsigned(out, frame);
        }
        out.push_back(' ');
        out.append(levelColumn(levelName(site.level)));
        out.append(" [");
        out.append(thread);
        out.append("] ");
        out.append(source_);
        out.push_back(' ');
        out.append(shortFunctionName(site.function));
        out.append(": batch ");
        appendUnsigned(out, info.count);
        if (tick != 0) {
            out.append(" in ");
            appendDouble(out, std::round(durationUs * 10.0) / 10.0);
            out.append("us");
        }
        out.append(": ");
        out.append(site.format);
        out.push_back('\n');
        const std::size_t rows = maxRows == 0 ? info.count : std::min<std::size_t>(maxRows, info.count);
        for (std::size_t i = 0; i < rows; ++i) {
            out.append("    #");
            appendUnsigned(out, i);
            out.push_back(' ');
            renderMessage(out, site, elements + i * info.elementSize);
            out.push_back('\n');
        }
        if (rows < info.count) {
            out.append("    … ");
            appendUnsigned(out, info.count - rows);
            out.append(" more\n");
        }
        return;
    }

    out.push_back('{');
    const std::size_t start = out.size();
    appendTime(out, tick);
    if (frame != 0) {
        if (out.size() != start) out.push_back(',');
        out.append("\"f\":");
        appendUnsigned(out, frame);
    }
    if (out.size() != start) out.push_back(',');
    out.append("\"lv\":\"");
    out.append(levelName(site.level));
    out.append("\",\"th\":");
    appendJsonString(out, thread);
    out.append(",\"src\":");
    appendJsonString(out, source_);
    out.append(",\"fn\":");
    appendJsonString(out, shortFunctionName(site.function));
    out.append(",\"msg\":");
    appendJsonString(out, site.format);
    out.append(",\"batch\":");
    appendUnsigned(out, info.count);
    if (tick != 0) {
        out.append(",\"duration_us\":");
        appendDouble(out, std::round(durationUs * 1000.0) / 1000.0);
    }
    // 1件ずつの値（書式の {} の順）。文字列は使えないので、数値・真偽・文字・アドレス。
    out.append(",\"rows\":[");
    for (std::uint32_t i = 0; i < info.count; ++i) {
        if (i > 0) out.push_back(',');
        out.push_back('[');
        const std::byte* cursor = elements + static_cast<std::size_t>(i) * info.elementSize;
        for (std::uint8_t a = 0; a < site.argCount; ++a) {
            if (a > 0) out.push_back(',');
            switch (site.tags[a]) {
            case ArgTag::Bool: out.append(readValue<bool>(cursor) ? "true" : "false"); break;
            case ArgTag::Char: {
                const char c = readValue<char>(cursor);
                appendJsonString(out, std::string_view(&c, 1));
                break;
            }
            case ArgTag::Int: appendSigned(out, readValue<std::int64_t>(cursor)); break;
            case ArgTag::UInt: appendUnsigned(out, readValue<std::uint64_t>(cursor)); break;
            case ArgTag::Double: appendDouble(out, readValue<double>(cursor)); break;
            case ArgTag::Pointer: {
                const auto address = readValue<std::uint64_t>(cursor);
                char buffer[24] = {'"', '0', 'x'};
                auto result = std::to_chars(buffer + 3, buffer + sizeof(buffer) - 1, address, 16);
                *result.ptr++ = '"';
                out.append(buffer, result.ptr);
                break;
            }
            case ArgTag::String:
            case ArgTag::None: out.append("null"); break;
            }
        }
        out.push_back(']');
    }
    out.append("]}\n");
}

void LineFormatter::appendEvent(std::string& out, std::uint64_t tick, std::string_view body) {
    if (format_ == OutputFormat::Text) {
        appendTime(out, tick);
        std::string level;
        std::string fields;
        flatJsonToText(fields, body, level);
        out.push_back(' ');
        out.append(levelColumn(level));
        out.push_back(' ');
        out.append(fields);
        out.push_back('\n');
        return;
    }
    out.push_back('{');
    const std::size_t start = out.size();
    appendTime(out, tick);
    if (out.size() != start && !body.empty()) out.push_back(',');
    out.append(body);
    out.append("}\n");
}

}  // namespace eco::log::detail
