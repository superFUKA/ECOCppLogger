#include "../include/ECOCppLogger/Assert.h"

#include <cstdlib>
#include <string>

#include "ECOCppLogger/Logger.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#else
#include <csignal>
#endif

namespace lg {

namespace detail {

std::atomic<uint8_t> g_assertAction{static_cast<uint8_t>(AssertAction::Break)};

namespace {

bool debuggerPresent() noexcept {
#if defined(_WIN32)
    return IsDebuggerPresent() != 0;
#else
    return false;
#endif
}

}  // namespace

void debugBreak() noexcept {
#if defined(_WIN32)
    __debugbreak();
#else
    std::raise(SIGTRAP);
#endif
}

bool assertFailed(const Site& site, uint64_t failure, bool applyAction) noexcept {
    if (failure == 1) {
        // 最初の失敗：その時点の最近の値とスタックを残す（次の writeOut で書く）。
        std::string reason = "assert ";
        const std::string_view file = site.file;
        const size_t slash = file.find_last_of("/\\");
        reason.append(slash == std::string_view::npos ? file : file.substr(slash + 1));
        reason.push_back(':');
        reason.append(std::to_string(site.line));
        dumpHistory(reason.c_str());
        writeStack(reason.c_str());
    }
    if (!applyAction) {
        return false;
    }
    switch (assertAction()) {
    case AssertAction::Continue: return false;
    case AssertAction::Break:
        if (debuggerPresent()) return true;
        [[fallthrough]];
    case AssertAction::Abort:
        writeOut();  // 落とす前に書き出す（落ちたときの書き出しも走る）
        std::abort();
    }
    return false;
}

}  // namespace detail

void setAssertAction(AssertAction action) noexcept {
    detail::g_assertAction.store(static_cast<uint8_t>(action), std::memory_order_relaxed);
}

AssertAction assertAction() noexcept {
    return static_cast<AssertAction>(detail::g_assertAction.load(std::memory_order_relaxed));
}

}  // namespace lg
