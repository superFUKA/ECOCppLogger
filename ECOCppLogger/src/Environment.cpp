#include "Environment.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

#include "Format.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#pragma comment(lib, "user32.lib")
#else
#include <unistd.h>
#endif

namespace lg::detail {

namespace {

#if defined(_WIN32)
std::string utf8(const wchar_t* text, int length = -1) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
    if (length < 0 && !out.empty()) out.pop_back();  // 終わりの '\0'
    return out;
}

std::string osVersion() {
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto function = reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            function(&info);
        }
    }
    return "Windows " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
           std::to_string(info.dwBuildNumber);
}

std::string cpuName() {
    int registers[4] = {};
    __cpuid(registers, static_cast<int>(0x80000000));
    if (static_cast<unsigned>(registers[0]) < 0x80000004) return {};
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(registers, static_cast<int>(0x80000002 + i));
        memcpy(brand + i * 16, registers, 16);
    }
    std::string name = brand;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    return name;
}

std::vector<std::string> gpuNames() {
    std::vector<std::string> names;
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
        const std::string name = utf8(device.DeviceString);
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        device.cb = sizeof(device);
    }
    return names;
}
#endif

std::string compilerName() {
    char text[64];
#if defined(__clang__)
    std::snprintf(text, sizeof(text), "clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(_MSC_VER)
    std::snprintf(text, sizeof(text), "MSVC %d.%d.%d", _MSC_FULL_VER / 10000000, (_MSC_FULL_VER / 100000) % 100,
                  _MSC_FULL_VER % 100000);
#elif defined(__GNUC__)
    std::snprintf(text, sizeof(text), "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    std::snprintf(text, sizeof(text), "unknown");
#endif
    return text;
}

void appendField(std::string& out, const char* key, std::string_view value) {
    if (value.empty()) return;
    out.append(",\"");
    out.append(key);
    out.append("\":");
    appendJsonString(out, value);
}

}  // namespace

std::string collectEnvironment(const AppInfo& app) {
    std::string out = "\"lv\":\"info\",\"event\":\"environment\"";
    if (app.name || app.version || app.commit) {
        out.append(",\"app\":{");
        bool first = true;
        for (const auto& [key, value] : {std::pair{"name", app.name}, std::pair{"version", app.version},
                                         std::pair{"commit", app.commit}}) {
            if (!value) continue;
            if (!first) out.push_back(',');
            first = false;
            appendJsonString(out, key);
            out.push_back(':');
            appendJsonString(out, value);
        }
        out.push_back('}');
    }
#if defined(_WIN32)
    appendField(out, "args", utf8(GetCommandLineW()));
#endif
    std::error_code error;
    const auto cwd = std::filesystem::current_path(error).u8string();
    appendField(out, "cwd", std::string_view(reinterpret_cast<const char*>(cwd.data()), cwd.size()));
#if defined(NDEBUG)
    appendField(out, "build", "release");
#else
    appendField(out, "build", "debug");
#endif
    appendField(out, "compiler", compilerName());
    out.append(",\"cpp\":");
    appendUnsigned(out, static_cast<uint64_t>(__cplusplus));
#if defined(_M_X64) || defined(__x86_64__)
    appendField(out, "arch", "x64");
#elif defined(_M_ARM64) || defined(__aarch64__)
    appendField(out, "arch", "arm64");
#endif
#if defined(_WIN32)
    appendField(out, "os", osVersion());
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD computerLength = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(computer, &computerLength)) appendField(out, "host", utf8(computer));
    appendField(out, "cpu", cpuName());
    out.append(",\"cores\":");
    appendUnsigned(out, std::thread::hardware_concurrency());
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        out.append(",\"memory_mb\":");
        appendUnsigned(out, memory.ullTotalPhys >> 20);
        out.append(",\"memory_free_mb\":");
        appendUnsigned(out, memory.ullAvailPhys >> 20);
    }
    const auto gpus = gpuNames();
    if (!gpus.empty()) {
        out.append(",\"gpu\":[");
        for (size_t i = 0; i < gpus.size(); ++i) {
            if (i != 0) out.push_back(',');
            appendJsonString(out, gpus[i]);
        }
        out.push_back(']');
    }
#else
    char host[256] = {};
    if (gethostname(host, sizeof(host)) == 0) appendField(out, "host", host);
    out.append(",\"cores\":");
    appendUnsigned(out, std::thread::hardware_concurrency());
#endif
    return out;
}

std::string newRunId() {
    std::random_device device;
    std::mt19937_64 random((static_cast<uint64_t>(device()) << 32) ^ device() ^
                           static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const uint64_t high = (random() & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;  // 版 4
    const uint64_t low = (random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;   // 変種
    char text[40];
    std::snprintf(text, sizeof(text), "%08llx-%04llx-%04llx-%04llx-%012llx",
                  static_cast<unsigned long long>(high >> 32), static_cast<unsigned long long>((high >> 16) & 0xFFFF),
                  static_cast<unsigned long long>(high & 0xFFFF), static_cast<unsigned long long>(low >> 48),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFull));
    return text;
}

}  // namespace lg::detail
