#pragma once

#include <cstdint>
#include <string_view>

namespace eco::log {

// ログの重さ。Off は「何も出さない」ための値で、ログの重さには使わない。
enum class Level : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
};

constexpr std::string_view levelName(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
    case Level::Off: return "off";
    }
    return "unknown";
}

}  // namespace eco::log
