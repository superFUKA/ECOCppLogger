#include "Format.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <ctime>

namespace lg::detail {

namespace {

constexpr char kHex[] = "0123456789abcdef";

// 1970-01-01 からの日数 → 年月日（Howard Hinnant の civil_from_days）。
void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned mp = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = static_cast<int>(static_cast<int64_t>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0));
}

void appendPadded(std::string& out, unsigned value, int width) {
    char buffer[16];
    for (int i = width - 1; i >= 0; --i) {
        buffer[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    out.append(buffer, static_cast<size_t>(width));
}

template <class T>
T readValue(const unsigned char*& cursor) {
    T value;
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return value;
}

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

bool hasType(const Site& site) {
    return site.type && std::string_view(site.type) != types::Log.name;
}

// {名前:書式} の書式。[0][幅][.桁][x|X]
struct Spec {
    bool zero = false;
    int width = 0;
    int precision = -1;
    char kind = 0;  // 'x' 'X'

    static Spec parse(std::string_view text) {
        Spec spec;
        size_t i = 0;
        if (i < text.size() && text[i] == '0') {
            spec.zero = true;
            ++i;
        }
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            spec.width = spec.width * 10 + (text[i++] - '0');
        }
        if (i < text.size() && text[i] == '.') {
            ++i;
            spec.precision = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                spec.precision = spec.precision * 10 + (text[i++] - '0');
            }
        }
        if (i < text.size()) {
            spec.kind = text[i];
        }
        return spec;
    }
};

void appendFormattedInteger(std::string& out, uint64_t magnitude, bool negative, const Spec& spec) {
    char buffer[32];
    const int base = spec.kind == 'x' || spec.kind == 'X' ? 16 : 10;
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), magnitude, base);
    if (spec.kind == 'X') {
        for (char* c = buffer; c < result.ptr; ++c) {
            if (*c >= 'a' && *c <= 'f') *c = static_cast<char>(*c - 'a' + 'A');
        }
    }
    const int digits = static_cast<int>(result.ptr - buffer) + (negative ? 1 : 0);
    if (negative) {
        out.push_back('-');
    }
    for (int i = digits; i < spec.width; ++i) {
        out.push_back(spec.zero ? '0' : ' ');
    }
    out.append(buffer, result.ptr);
}

void appendFormattedDouble(std::string& out, double value, const Spec& spec) {
    if (spec.precision < 0 && spec.width == 0) {
        appendDouble(out, value);
        return;
    }
    char buffer[64];
    auto result = spec.precision >= 0
                      ? std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, spec.precision)
                      : std::to_chars(buffer, buffer + sizeof(buffer), value);
    const int length = static_cast<int>(result.ptr - buffer);
    for (int i = length; i < spec.width; ++i) {
        out.push_back(spec.zero ? '0' : ' ');
    }
    out.append(buffer, result.ptr);
}

// 引数1つ：本文へ書式どおりに、JSON へは値として。
void appendArgument(std::string& message, std::string* json, ArgTag tag, const unsigned char*& cursor,
                    const Spec& spec) {
    switch (tag) {
    case ArgTag::Bool: {
        const bool value = readValue<bool>(cursor);
        message.append(value ? "true" : "false");
        if (json) json->append(value ? "true" : "false");
        break;
    }
    case ArgTag::Char: {
        const char value = readValue<char>(cursor);
        message.push_back(value);
        if (json) appendJsonString(*json, std::string_view(&value, 1));
        break;
    }
    case ArgTag::Int: {
        const auto value = readValue<int64_t>(cursor);
        const uint64_t magnitude = value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
        appendFormattedInteger(message, magnitude, value < 0, spec);
        if (json) appendSigned(*json, value);
        break;
    }
    case ArgTag::UInt: {
        const auto value = readValue<uint64_t>(cursor);
        appendFormattedInteger(message, value, false, spec);
        if (json) appendUnsigned(*json, value);
        break;
    }
    case ArgTag::Double: {
        const auto value = readValue<double>(cursor);
        appendFormattedDouble(message, value, spec);
        if (json) appendDouble(*json, value);
        break;
    }
    case ArgTag::String: {
        const auto stored = readValue<uint32_t>(cursor);
        const uint32_t length = stored & 0x7FFFFFFFu;
        const std::string_view text(reinterpret_cast<const char*>(cursor), length);
        cursor += length;
        message.append(text);
        if (stored & 0x80000000u) message.append("…");
        if (json) appendJsonString(*json, text);
        break;
    }
    case ArgTag::Pointer: {
        const auto address = readValue<uint64_t>(cursor);
        char buffer[24] = {'0', 'x'};
        const auto result = std::to_chars(buffer + 2, buffer + sizeof(buffer), address, 16);
        message.append(buffer, result.ptr);
        if (json) appendJsonString(*json, std::string_view(buffer, static_cast<size_t>(result.ptr - buffer)));
        break;
    }
    default: break;  // 元の大きさの型はまとめ書きだけで使う
    }
}

}  // namespace

void appendJsonString(std::string& out, std::string_view text) {
    out.push_back('"');
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
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

void appendUnsigned(std::string& out, uint64_t value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

void appendSigned(std::string& out, int64_t value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

void appendTimestamp(std::string& out, int64_t unixNanoseconds, int utcOffsetMinutes) {
    const int64_t local = unixNanoseconds + static_cast<int64_t>(utcOffsetMinutes) * 60'000'000'000LL;
    int64_t seconds = local / 1'000'000'000LL;
    int64_t nanoseconds = local % 1'000'000'000LL;
    if (nanoseconds < 0) {
        nanoseconds += 1'000'000'000LL;
        --seconds;
    }
    int64_t days = seconds / 86400;
    int64_t secondOfDay = seconds % 86400;
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
    const auto findParenthesis = [&](size_t from) {
        int depth = 0;
        for (size_t i = from; i < function.size(); ++i) {
            const char c = function[i];
            if (c == '<') ++depth;
            else if (c == '>' && depth > 0) --depth;
            else if (c == '(' && depth == 0) return i;
        }
        return function.size();
    };
    size_t end = findParenthesis(0);
    // "operator ()" の空白で切らないように、空白を探すのは名前の部分の終わりまで。
    size_t nameEnd = end;
    std::string_view head = function.substr(0, end);
    while (!head.empty() && head.back() == ' ') {
        head.remove_suffix(1);
    }
    if (head.ends_with("operator") && end + 1 < function.size() && function[end + 1] == ')') {
        nameEnd = head.size() - 8;
        end = findParenthesis(end + 2);
    }
    // 戻り値の型・呼び出し規約（最後の空白より前）を落とす。
    size_t begin = 0;
    int depth = 0;
    for (size_t i = 0; i < nameEnd; ++i) {
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
    const size_t slash = file.find_last_of("/\\");
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
    return static_cast<int>(std::difftime(std::mktime(&local), std::mktime(&utc)) / 60.0);
}

void renderMessage(std::string& message, std::string* argsJson, const Site& site, const unsigned char* cursor) {
    const std::string_view format = site.format;
    std::string values;  // 名前付きの値の JSON（"名前":値,…）
    size_t argument = 0;
    for (size_t i = 0; i < format.size(); ++i) {
        const char c = format[i];
        if ((c == '{' || c == '}') && i + 1 < format.size() && format[i + 1] == c) {
            message.push_back(c);
            ++i;
            continue;
        }
        if (c != '{' || argument >= site.argCount) {
            message.push_back(c);
            continue;
        }
        const size_t close = format.find('}', i + 1);
        if (close == std::string_view::npos) {
            message.append(format.substr(i));
            break;
        }
        const std::string_view inside = format.substr(i + 1, close - i - 1);
        const size_t colon = inside.find(':');
        const std::string_view name = inside.substr(0, colon);
        const Spec spec = colon == std::string_view::npos ? Spec{} : Spec::parse(inside.substr(colon + 1));
        std::string* json = nullptr;
        if (argsJson && !name.empty()) {
            if (!values.empty()) values.push_back(',');
            appendJsonString(values, name);
            values.push_back(':');
            json = &values;
        }
        appendArgument(message, json, site.tags[argument++], cursor, spec);
        i = close;
    }
    if (argsJson && !values.empty()) {
        argsJson->push_back('{');
        argsJson->append(values);
        argsJson->push_back('}');
    }
}

// ---- 1行 ----------------------------------------------------------------------------------------------

void LineFormatter::appendTime(std::string& out, uint64_t tick) {
    if (tick == 0) {
        if (format_ == OutputFormat::Text) {
            out.append("--:--:--.------");  // 時刻を残さない設定
        }
        return;
    }
    // 秒までの部分は、同じ秒の間は使い回す。
    const int64_t unixNanoseconds = timeBase_.toUnixNanoseconds(tick);
    int64_t second = unixNanoseconds / 1'000'000'000LL;
    int64_t nanoseconds = unixNanoseconds % 1'000'000'000LL;
    if (nanoseconds < 0) {
        nanoseconds += 1'000'000'000LL;
        --second;
    }
    if (second != cachedSecond_ || cachedPrefix_.empty()) {
        std::string full;
        appendTimestamp(full, second * 1'000'000'000LL, timeBase_.utcOffsetMinutes);
        const size_t dot = full.find('.');
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

void LineFormatter::appendTextPrefix(std::string& out, uint64_t tick, uint64_t frame, const Site& site,
                                     std::string_view thread) {
    appendTime(out, tick);
    if (frame != 0) {
        out.append(" f");
        appendUnsigned(out, frame);
    }
    out.push_back(' ');
    out.append(levelColumn(levelName(site.level)));
    if (hasType(site)) {
        out.append(" #");
        out.append(site.type);
    }
    out.append(" [");
    out.append(thread);
    out.append("] ");
    out.append(sourceFileName(site.file, fullPath_));
    out.push_back(':');
    appendUnsigned(out, site.line);
    out.push_back(' ');
    out.append(shortFunctionName(site.function));
    out.append(": ");
}

void LineFormatter::appendJsonPrefix(std::string& out, uint64_t tick, uint64_t frame, const Site& site,
                                     std::string_view thread) {
    out.push_back('{');
    const size_t start = out.size();
    appendTime(out, tick);
    if (frame != 0) {
        if (out.size() != start) out.push_back(',');
        out.append("\"f\":");
        appendUnsigned(out, frame);
    }
    if (out.size() != start) out.push_back(',');
    out.append("\"lv\":\"");
    out.append(levelName(site.level));
    out.push_back('"');
    if (hasType(site)) {
        out.append(",\"type\":");
        appendJsonString(out, site.type);
    }
    out.append(",\"th\":");
    appendJsonString(out, thread);
    source_.assign(sourceFileName(site.file, fullPath_));
    source_.push_back(':');
    appendUnsigned(source_, site.line);
    out.append(",\"src\":");
    appendJsonString(out, source_);
    out.append(",\"fn\":");
    appendJsonString(out, shortFunctionName(site.function));
}

void LineFormatter::appendRecord(std::string& out, uint64_t tick, uint64_t frame, uint64_t aux, const Site& site,
                                 const unsigned char* arguments, std::string_view thread) {
    message_.clear();
    args_.clear();
    renderMessage(message_, format_ == OutputFormat::Json ? &args_ : nullptr, site, arguments);

    if (format_ == OutputFormat::Text) {
        appendTextPrefix(out, tick, frame, site, thread);
        out.append(message_);
        switch (site.sampling) {
        case Sampling::EveryN:
        case Sampling::FirstN:
            out.append(site.sampling == Sampling::EveryN ? "  (every " : "  (first ");
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

    appendJsonPrefix(out, tick, frame, site, thread);
    out.append(",\"msg\":");
    appendJsonString(out, message_);
    if (!args_.empty()) {
        out.append(",\"args\":");
        out.append(args_);
    }
    switch (site.sampling) {
    case Sampling::EveryN:
    case Sampling::FirstN:
        out.append(site.sampling == Sampling::EveryN ? ",\"sample\":\"every " : ",\"sample\":\"first ");
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

void LineFormatter::appendEvent(std::string& out, uint64_t tick, std::string_view body) {
    if (format_ == OutputFormat::Json) {
        out.push_back('{');
        const size_t start = out.size();
        appendTime(out, tick);
        if (out.size() != start && !body.empty()) out.push_back(',');
        out.append(body);
        out.append("}\n");
        return;
    }
    // 人向け：時刻 重さ key=value …（JSON の中身を1段だけ開く）
    appendTime(out, tick);
    std::string level;
    std::string fields;
    size_t i = 0;
    while (i < body.size()) {
        if (body[i] != '"') {
            ++i;
            continue;
        }
        const size_t keyEnd = body.find('"', i + 1);
        if (keyEnd == std::string_view::npos || keyEnd + 1 >= body.size() || body[keyEnd + 1] != ':') break;
        const std::string_view key = body.substr(i + 1, keyEnd - i - 1);
        const size_t valueStart = keyEnd + 2;
        size_t j = valueStart;
        int depth = 0;
        bool inString = false;
        for (; j < body.size(); ++j) {
            const char ch = body[j];
            if (inString) {
                if (ch == '\\') ++j;
                else if (ch == '"') inString = false;
            } else if (ch == '"') inString = true;
            else if (ch == '[' || ch == '{') ++depth;
            else if (ch == ']' || ch == '}') --depth;
            else if (ch == ',' && depth == 0) break;
        }
        std::string_view value = body.substr(valueStart, j - valueStart);
        std::string unescaped;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
            for (size_t k = 0; k < value.size(); ++k) {
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
            if (!fields.empty()) fields.push_back(' ');
            fields.append(key);
            fields.push_back('=');
            fields.append(value);
        }
        i = j + 1;
    }
    out.push_back(' ');
    out.append(levelColumn(level));
    out.push_back(' ');
    out.append(fields);
    out.push_back('\n');
}

}  // namespace lg::detail
