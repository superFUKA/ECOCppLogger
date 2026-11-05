#include "Stack.h"

#include <cstdio>
#include <mutex>

#include "Format.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace lg::detail {

namespace {

#if defined(_WIN32)
std::recursive_mutex& symbolMutex() {
    static auto* mutex = new std::recursive_mutex();  // 終了処理・落ちている最中にも使うので消さない
    return *mutex;
}

bool g_symbolsReady = false;
#endif

}  // namespace

void initializeSymbols() noexcept {
#if defined(_WIN32)
    std::lock_guard lock(symbolMutex());
    if (!g_symbolsReady) {
        SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        g_symbolsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }
#endif
}

size_t captureStack(uint64_t* addresses, size_t capacity, size_t skip) noexcept {
#if defined(_WIN32)
    void* frames[128];
    const DWORD wanted = static_cast<DWORD>(capacity < 128 ? capacity : 128);
    const USHORT count = RtlCaptureStackBackTrace(static_cast<DWORD>(skip + 1), wanted, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        addresses[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frames[i]));
    }
    return count;
#else
    (void)addresses;
    (void)capacity;
    (void)skip;
    return 0;
#endif
}

std::string describeAddress(uint64_t address) {
    char hex[32];
    std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(address));
#if defined(_WIN32)
    initializeSymbols();
    std::lock_guard lock(symbolMutex());
    HANDLE process = GetCurrentProcess();
    std::string text;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        text.assign(symbol->Name, symbol->NameLen);
    }
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (!text.empty() && SymGetLineFromAddr64(process, address, &lineDisplacement, &line)) {
        text += " (";
        text += sourceFileName(line.FileName, false);
        text += ":" + std::to_string(line.LineNumber) + ")";
        return text;
    }
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (SymGetModuleInfo64(process, address, &module)) {
        char offset[32];
        std::snprintf(offset, sizeof(offset), "+0x%llx", static_cast<unsigned long long>(address - module.BaseOfImage));
        const std::string place = std::string(module.ModuleName) + offset;
        return text.empty() ? place : text + " (" + place + ")";
    }
    return text.empty() ? std::string(hex) : text;
#else
    return hex;
#endif
}

void appendStackJson(std::string& out, const uint64_t* addresses, size_t count) {
    out.push_back('[');
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) out.push_back(',');
        appendJsonString(out, describeAddress(addresses[i]));
    }
    out.push_back(']');
}

}  // namespace lg::detail
