#include "Crash.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <typeinfo>
#include <vector>

#include "ECOCppLogger/ThreadBuffer.h"
#include "Format.h"
#include "Registry.h"
#include "Stack.h"
#include "Writer.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "version.lib")
#endif

namespace lg::detail {

namespace {

std::atomic<bool> g_installed{false};
CrashOptions g_options;
std::terminate_handler g_previousTerminate = nullptr;

void appendHex(std::string& out, uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    appendJsonString(out, text);
}

// C++ の型名（"std::runtime_error"）。MSVC の装飾名 ".?AVruntime_error@std@@" も直す。
std::string readableTypeName(std::string_view name) {
    if (name.starts_with(".?AV") || name.starts_with(".?AU")) {
        name.remove_prefix(4);
        std::vector<std::string_view> parts;
        while (!name.empty() && name.front() != '@') {
            const size_t at = name.find('@');
            parts.push_back(name.substr(0, at));
            name = at == std::string_view::npos ? std::string_view() : name.substr(at + 1);
        }
        std::string out;
        for (size_t i = parts.size(); i-- > 0;) {
            out.append(parts[i]);
            if (i != 0) out.append("::");
        }
        return out;
    }
    for (std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    }
    return std::string(name);
}

// 例外の連なり（std::throw_with_nested で包んだもの）を、外から順に [{"type":…,"what":…},…] にする。
void appendExceptionChain(std::string& out, std::exception_ptr pointer) {
    out.append(",\"exceptions\":[");
    bool first = true;
    for (int depth = 0; pointer && depth < 16; ++depth) {
        std::exception_ptr next;
        if (!first) out.push_back(',');
        first = false;
        try {
            std::rethrow_exception(pointer);
        } catch (const std::exception& error) {
            out.append("{\"type\":");
            appendJsonString(out, readableTypeName(typeid(error).name()));
            out.append(",\"what\":");
            appendJsonString(out, error.what());
            out.push_back('}');
            try {
                std::rethrow_if_nested(error);
            } catch (...) {
                next = std::current_exception();
            }
        } catch (...) {
            out.append("{\"type\":\"(not std::exception)\"}");
        }
        pointer = next;
    }
    out.push_back(']');
}

#if defined(_WIN32)

LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
constexpr DWORD kCppException = 0xE06D7363;

// 文脈からスタックをたどる（x64：関数表で戻り先をたどる。止めたスレッドの上でも使える）。
size_t walkStack(const CONTEXT& start, uint64_t* addresses, size_t capacity) {
    size_t count = 0;
#if defined(_M_X64)
    CONTEXT context = start;
    while (count < capacity && context.Rip != 0) {
        addresses[count++] = context.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function) {
            // 葉の関数：戻り先はスタックの一番上
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        } else {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                             &establisher, nullptr);
        }
    }
#else
    (void)start;
    (void)addresses;
    (void)capacity;
#endif
    return count;
}

void appendRegisters(std::string& out, const CONTEXT& context) {
    out.append(",\"registers\":{");
#if defined(_M_X64)
    const std::pair<const char*, DWORD64> registers[] = {
        {"rip", context.Rip}, {"rsp", context.Rsp}, {"rbp", context.Rbp}, {"rax", context.Rax}, {"rbx", context.Rbx},
        {"rcx", context.Rcx}, {"rdx", context.Rdx}, {"rsi", context.Rsi}, {"rdi", context.Rdi}, {"r8", context.R8},
        {"r9", context.R9},   {"r10", context.R10}, {"r11", context.R11}, {"r12", context.R12}, {"r13", context.R13},
        {"r14", context.R14}, {"r15", context.R15}, {"eflags", context.EFlags}};
    bool first = true;
    for (const auto& [name, value] : registers) {
        if (!first) out.push_back(',');
        first = false;
        appendJsonString(out, name);
        out.push_back(':');
        appendHex(out, value);
    }
#else
    (void)context;
#endif
    out.push_back('}');
}

std::string threadName(DWORD osThreadId) {
    for (const auto& entry : Registry::instance().snapshot(true)) {
        if (entry.buffer->osThreadId() == osThreadId) return entry.name;
    }
    return {};
}

// 落ちたスレッド以外のスレッドを1つずつ止めてスタックを取る（名前を引くのは、動かし直した後）。
void appendOtherThreads(std::string& out) {
    const DWORD self = GetCurrentThreadId();
    const DWORD process = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    struct Captured {
        DWORD id;
        std::vector<uint64_t> addresses;
    };
    std::vector<Captured> captured;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more && captured.size() < 256; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   entry.th32ThreadID);
        if (!thread) continue;
        Captured item{entry.th32ThreadID, {}};
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                uint64_t addresses[48];
                const size_t count = walkStack(context, addresses, 48);
                item.addresses.assign(addresses, addresses + count);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
        captured.push_back(std::move(item));
    }
    CloseHandle(snapshot);
    out.append(",\"threads\":[");
    for (size_t i = 0; i < captured.size(); ++i) {
        if (i != 0) out.push_back(',');
        out.append("{\"tid\":");
        appendUnsigned(out, captured[i].id);
        const std::string name = threadName(captured[i].id);
        if (!name.empty()) {
            out.append(",\"th\":");
            appendJsonString(out, name);
        }
        out.append(",\"stack\":");
        appendStackJson(out, captured[i].addresses.data(), captured[i].addresses.size());
        out.push_back('}');
    }
    out.push_back(']');
}

std::string fileVersion(const wchar_t* path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) return {};
    std::vector<unsigned char> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path, 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return {};
    }
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}

std::string utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

void appendModules(std::string& out) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;
    out.append(",\"modules\":[");
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool first = true;
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        if (!first) out.push_back(',');
        first = false;
        out.append("{\"name\":");
        appendJsonString(out, utf8(entry.szModule));
        out.append(",\"base\":");
        appendHex(out, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry.modBaseAddr)));
        out.append(",\"size\":");
        appendUnsigned(out, entry.modBaseSize);
        const std::string version = fileVersion(entry.szExePath);
        if (!version.empty()) {
            out.append(",\"version\":");
            appendJsonString(out, version);
        }
        out.append(",\"path\":");
        appendJsonString(out, utf8(entry.szExePath));
        out.push_back('}');
    }
    out.push_back(']');
    CloseHandle(snapshot);
}

void writeMinidump(std::string& out, EXCEPTION_POINTERS* pointers) {
    const std::wstring path = writer().crashDumpPath();
    if (path.empty()) return;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), pointers, FALSE};
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                 MiniDumpWithIndirectlyReferencedMemory);
    const BOOL written =
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, pointers ? &exception : nullptr,
                          nullptr, nullptr);
    CloseHandle(file);
    if (written) {
        out.append(",\"minidump\":");
        appendJsonString(out, utf8(path.c_str()));
    }
}

// MSVC の C++ 例外（0xE06D7363）の型と what() を、例外の記録から読む（std::terminate を通らずに落ちるとき用）。
void appendCppExceptionFromRecord(std::string& out, const EXCEPTION_RECORD& record) {
    if (record.NumberParameters < 4) return;
    struct ThrowInfo {
        uint32_t attributes;
        int32_t unwind;
        int32_t forwardCompat;
        int32_t catchableTypes;
    };
    struct CatchableTypeArray {
        int32_t count;
        int32_t types[1];
    };
    struct CatchableType {
        uint32_t properties;
        int32_t type;
        int32_t mdisp;
        int32_t pdisp;
        int32_t vdisp;
        int32_t size;
        int32_t copy;
    };
    struct TypeDescriptor {
        const void* vftable;
        void* spare;
        char name[1];
    };
    const auto* object = reinterpret_cast<const unsigned char*>(record.ExceptionInformation[1]);
    const auto* info = reinterpret_cast<const ThrowInfo*>(record.ExceptionInformation[2]);
    const auto base = static_cast<uintptr_t>(record.ExceptionInformation[3]);
    if (!object || !info) return;
    const auto* array = reinterpret_cast<const CatchableTypeArray*>(base + info->catchableTypes);
    std::string thrown;
    const std::exception* standard = nullptr;
    for (int32_t i = 0; i < array->count && i < 64; ++i) {
        const auto* type = reinterpret_cast<const CatchableType*>(base + array->types[i]);
        const auto* descriptor = reinterpret_cast<const TypeDescriptor*>(base + type->type);
        const std::string_view name = descriptor->name;
        if (i == 0) thrown = readableTypeName(name);
        if (name == ".?AVexception@std@@") {
            standard = reinterpret_cast<const std::exception*>(object + type->mdisp);
        }
    }
    out.append(",\"exceptions\":[{\"type\":");
    appendJsonString(out, thrown);
    if (standard) {
        out.append(",\"what\":");
        appendJsonString(out, standard->what());
    }
    out.append("}]");
}

const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in page error";
    case kCppException: return "C++ exception not caught";
    default: return nullptr;
    }
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    std::string extra;
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", record.ExceptionCode);
    extra.append(",\"code\":");
    appendJsonString(extra, code);
    if (const char* name = exceptionName(record.ExceptionCode)) {
        extra.append(",\"name\":");
        appendJsonString(extra, name);
    }
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const ULONG_PTR kind = record.ExceptionInformation[0];
        extra.append(",\"access\":");
        appendJsonString(extra, kind == 0 ? "read" : kind == 1 ? "write" : "execute");
        extra.append(",\"address\":");
        appendHex(extra, record.ExceptionInformation[1]);
    }
    if (record.ExceptionCode == kCppException) {
        appendCppExceptionFromRecord(extra, record);
    }
    extra.append(",\"th\":");
    appendJsonString(extra, threadName(GetCurrentThreadId()));
    // スタックオーバーフローでは、ここで深く呼ぶとまた溢れるので、スタック・ほかのスレッドは取らない。
    if (record.ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        uint64_t addresses[64];
        const size_t count = walkStack(*pointers->ContextRecord, addresses, 64);
        extra.append(",\"stack\":");
        appendStackJson(extra, addresses, count);
        if (g_options.registers) appendRegisters(extra, *pointers->ContextRecord);
        if (g_options.allThreadStacks) appendOtherThreads(extra);
        if (g_options.modules) appendModules(extra);
        if (g_options.minidump) writeMinidump(extra, pointers);
    }
    writer().emergency("unhandled exception", extra);
    return g_previousFilter ? g_previousFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

#endif  // _WIN32

// std::terminate・シグナルで落ちるとき：今のスレッドのスタックから。
void appendCurrentThread(std::string& extra) {
    uint64_t addresses[64];
    const size_t count = captureStack(addresses, 64, 2);
    extra.append(",\"stack\":");
    appendStackJson(extra, addresses, count);
#if defined(_WIN32)
    extra.append(",\"th\":");
    appendJsonString(extra, threadName(GetCurrentThreadId()));
    if (g_options.registers) {
        CONTEXT context{};
        RtlCaptureContext(&context);
        appendRegisters(extra, context);
    }
    if (g_options.allThreadStacks) appendOtherThreads(extra);
    if (g_options.modules) appendModules(extra);
    if (g_options.minidump) writeMinidump(extra, nullptr);
#endif
}

void onTerminate() {
    std::string extra;
    if (std::exception_ptr current = std::current_exception()) {
        appendExceptionChain(extra, current);
    }
    appendCurrentThread(extra);
    writer().emergency("std::terminate", extra);
    if (g_previousTerminate) {
        g_previousTerminate();
    }
    std::abort();
}

void onSignal(int signal) {
    const char* name = signal == SIGABRT  ? "SIGABRT"
                       : signal == SIGSEGV ? "SIGSEGV"
                       : signal == SIGFPE  ? "SIGFPE"
                       : signal == SIGILL  ? "SIGILL"
                                           : "signal";
    std::string extra = ",\"signal\":";
    appendJsonString(extra, name);
    appendCurrentThread(extra);
    writer().emergency(name, extra);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

}  // namespace

void installCrashHandler(const CrashOptions& options) {
    g_options = options;
    static std::once_flag once;
    std::call_once(once, [] {
        initializeSymbols();  // 落ちてからでは遅いので、名前を引く準備は今
#if defined(_WIN32)
        g_previousFilter = SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif
        g_previousTerminate = std::set_terminate(onTerminate);
        std::signal(SIGABRT, onSignal);
        std::signal(SIGSEGV, onSignal);
        std::signal(SIGFPE, onSignal);
        std::signal(SIGILL, onSignal);
        g_installed.store(true, std::memory_order_release);
    });
    installThreadCrashHandler();
}

void installThreadCrashHandler() noexcept {
    if (g_installed.load(std::memory_order_acquire) && std::get_terminate() != onTerminate) {
        std::set_terminate(onTerminate);
    }
}

}  // namespace lg::detail
