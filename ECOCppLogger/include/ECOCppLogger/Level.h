#pragma once

#include <stdint.h>

namespace lg {

// ログの重さ。Off は「何も出さない」ための値で、ログの重さには使わない。
enum class Level : uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
};

constexpr const char* levelName(Level level) noexcept {
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

// マクロで短く書くための名前（ECO_LOG(Warn, …)・ECO_LOG(Warn | Physics, …)）。
namespace levels {
inline constexpr Level Trace = Level::Trace;
inline constexpr Level Debug = Level::Debug;
inline constexpr Level Info = Level::Info;
inline constexpr Level Warn = Level::Warn;
inline constexpr Level Error = Level::Error;
inline constexpr Level Fatal = Level::Fatal;
}  // namespace levels

}  // namespace lg
