#include "Output.h"

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

LineFormatter::LineFormatter(const Clock& clock, bool fullPath)
    : clock_(clock), fullPath_(fullPath), utcOffsetMinutes_(localUtcOffsetMinutes()) {}

void LineFormatter::appendLineStart(std::string& out, std::uint64_t tick) const {
    out.append("{\"t\":\"");
    appendTimestamp(out, clock_.toUnixNanoseconds(tick), utcOffsetMinutes_);
    out.push_back('"');
}

void LineFormatter::renderMessage(const RecordHeader& header) {
    const Site& site = *header.site;
    message_.clear();
    const std::byte* cursor = reinterpret_cast<const std::byte*>(&header) + sizeof(RecordHeader);
    std::size_t argument = 0;
    const std::string_view format = site.format;
    for (std::size_t i = 0; i < format.size(); ++i) {
        const char c = format[i];
        if ((c == '{' || c == '}') && i + 1 < format.size() && format[i + 1] == c) {
            message_.push_back(c);
            ++i;
            continue;
        }
        if (c != '{' || i + 1 >= format.size() || format[i + 1] != '}' || argument >= site.argCount) {
            message_.push_back(c);
            continue;
        }
        ++i;
        switch (site.tags[argument++]) {
        case ArgTag::Bool: message_.append(readValue<bool>(cursor) ? "true" : "false"); break;
        case ArgTag::Char: message_.push_back(readValue<char>(cursor)); break;
        case ArgTag::Int: appendSigned(message_, readValue<std::int64_t>(cursor)); break;
        case ArgTag::UInt: appendUnsigned(message_, readValue<std::uint64_t>(cursor)); break;
        case ArgTag::Double: appendDouble(message_, readValue<double>(cursor)); break;
        case ArgTag::String: {
            const auto stored = readValue<std::uint32_t>(cursor);
            const std::uint32_t length = stored & 0x7FFFFFFFu;
            message_.append(reinterpret_cast<const char*>(cursor), length);
            cursor += length;
            if (stored & 0x80000000u) {
                message_.append("…");
            }
            break;
        }
        case ArgTag::Pointer: {
            const auto address = readValue<std::uint64_t>(cursor);
            char buffer[24] = {'0', 'x'};
            const auto result = std::to_chars(buffer + 2, buffer + sizeof(buffer), address, 16);
            message_.append(buffer, result.ptr);
            break;
        }
        case ArgTag::None: break;
        }
    }
}

void LineFormatter::appendRecord(std::string& out, const RecordHeader& header, std::string_view threadName) {
    const Site& site = *header.site;
    renderMessage(header);
    appendLineStart(out, header.tick);
    if (header.frame != 0) {
        out.append(",\"f\":");
        appendUnsigned(out, header.frame);
    }
    out.append(",\"lv\":\"");
    out.append(levelName(site.level));
    out.append("\",\"th\":");
    appendJsonString(out, threadName);
    out.append(",\"src\":");
    source_.assign(sourceFileName(site.file, fullPath_));  // フルパスの \ はエスケープが要る
    source_.push_back(':');
    appendUnsigned(source_, site.line);
    appendJsonString(out, source_);
    out.append(",\"fn\":");
    appendJsonString(out, shortFunctionName(site.function));
    out.append(",\"msg\":");
    appendJsonString(out, message_);
    switch (static_cast<Sampling>(site.sampling)) {
    case Sampling::EveryN:
        out.append(",\"sample\":\"every ");
        appendUnsigned(out, site.samplingValue);
        out.append("\",\"n\":");
        appendUnsigned(out, header.aux);
        break;
    case Sampling::FirstN:
        out.append(",\"sample\":\"first ");
        appendUnsigned(out, site.samplingValue);
        out.append("\",\"n\":");
        appendUnsigned(out, header.aux);
        break;
    case Sampling::EveryMs:
        out.append(",\"sample\":\"every ");
        appendUnsigned(out, site.samplingValue);
        out.append("ms\",\"skipped\":");
        appendUnsigned(out, header.aux);
        break;
    case Sampling::None: break;
    }
    out.append("}\n");
}

}  // namespace eco::log::detail
