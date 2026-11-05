#include "CrashHandler.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <exception>
#include <mutex>

#include "Output.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace eco::log::detail {

namespace {

std::terminate_handler g_previousTerminate = nullptr;
std::atomic<bool> g_installed{false};

#if defined(_WIN32)

LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
std::mutex g_symbolMutex;  // dbghelp は複数のスレッドから同時に使えない

void appendFrame(std::string& out, DWORD64 address, bool& first) {
    char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    std::string text;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        text.assign(symbol->Name, symbol->NameLen);
    } else {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(address));
        text = hex;
    }
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line)) {
        text += " (";
        text += sourceFileName(line.FileName, false);
        text += ":" + std::to_string(line.LineNumber) + ")";
    } else {
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        if (SymGetModuleInfo64(GetCurrentProcess(), address, &module)) {
            char offset[32];
            std::snprintf(offset, sizeof(offset), "+0x%llx", static_cast<unsigned long long>(address - module.BaseOfImage));
            text += std::string(" (") + module.ModuleName + offset + ")";
        }
    }
    if (!first) {
        out.push_back(',');
    }
    first = false;
    appendJsonString(out, text);
}

std::string stackFromContext(const CONTEXT& original) {
    std::lock_guard lock(g_symbolMutex);
    CONTEXT context = original;
    STACKFRAME64 frame{};
    DWORD machine = 0;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
    std::string out = "[";
    bool first = true;
    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        appendFrame(out, frame.AddrPC.Offset, first);
    }
    out.push_back(']');
    return out;
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* pointers) {
    std::string extra;
    char code[32];
    std::snprintf(code, sizeof(code), "0x%08lX", pointers->ExceptionRecord->ExceptionCode);
    extra += ",\"code\":";
    appendJsonString(extra, code);
    const char* name = nullptr;
    switch (pointers->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: name = "access violation"; break;
    case EXCEPTION_STACK_OVERFLOW: name = "stack overflow"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: name = "integer divide by zero"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: name = "illegal instruction"; break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: name = "array bounds exceeded"; break;
    case 0xE06D7363: name = "C++ exception not caught"; break;
    default: break;
    }
    if (name) {
        extra += ",\"name\":";
        appendJsonString(extra, name);
    }
    if (pointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        pointers->ExceptionRecord->NumberParameters >= 2) {
        char access[64];
        std::snprintf(access, sizeof(access), "%s 0x%llx",
                      pointers->ExceptionRecord->ExceptionInformation[0] == 0   ? "read"
                      : pointers->ExceptionRecord->ExceptionInformation[0] == 1 ? "write"
                                                                                 : "execute",
                      static_cast<unsigned long long>(pointers->ExceptionRecord->ExceptionInformation[1]));
        extra += ",\"access\":";
        appendJsonString(extra, access);
    }
    // スタックオーバーフローでは、ここで深く呼ぶとまた溢れるのでスタックは取らない。
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        extra += ",\"stack\":" + stackFromContext(*pointers->ContextRecord);
    }
    emergencyFlush("unhandled exception", extra);
    return g_previousFilter ? g_previousFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

#endif

void onTerminate() {
    std::string extra;
    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& error) {
            extra += ",\"exception\":";
            appendJsonString(extra, error.what());
        } catch (...) {
            extra += ",\"exception\":\"(not std::exception)\"";
        }
    }
    extra += ",\"stack\":" + currentStackJson(1);
    emergencyFlush("std::terminate", extra);
    if (g_previousTerminate) {
        g_previousTerminate();
    }
    std::abort();
}

void onSignal(int signal) {
    const char* name = signal == SIGABRT ? "SIGABRT" : signal == SIGSEGV ? "SIGSEGV" : signal == SIGFPE ? "SIGFPE"
                                                                       : signal == SIGILL ? "SIGILL" : "signal";
    std::string extra = ",\"signal\":";
    appendJsonString(extra, name);
    extra += ",\"stack\":" + currentStackJson(1);
    emergencyFlush(name, extra);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

}  // namespace

std::string currentStackJson(int skip) {
#if defined(_WIN32)
    void* addresses[64];
    const USHORT count = CaptureStackBackTrace(static_cast<DWORD>(skip + 1), 64, addresses, nullptr);
    std::lock_guard lock(g_symbolMutex);
    std::string out = "[";
    bool first = true;
    for (USHORT i = 0; i < count; ++i) {
        appendFrame(out, reinterpret_cast<DWORD64>(addresses[i]), first);
    }
    out.push_back(']');
    return out;
#else
    (void)skip;
    return "[]";
#endif
}

void installCrashHandler() {
    static std::once_flag once;
    std::call_once(once, [] {
#if defined(_WIN32)
        // 落ちてからでは遅いので、シンボルはここで読み込む。
        SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        g_previousFilter = SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif
        g_previousTerminate = std::set_terminate(onTerminate);
        std::signal(SIGABRT, onSignal);
        std::signal(SIGSEGV, onSignal);
        std::signal(SIGFPE, onSignal);
        std::signal(SIGILL, onSignal);
        g_installed.store(true);
    });
}

void installThreadTerminateHandler() {
    if (g_installed.load() && std::get_terminate() != onTerminate) {
        std::set_terminate(onTerminate);
    }
}

}  // namespace eco::log::detail
