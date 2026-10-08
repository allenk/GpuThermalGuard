#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace gtg::research {
struct MethodAddress {
    const char* role;
    void* address;
    void* object = nullptr;
};

// A research oracle: called by the helper and, only when requested through an
// environment variable, by our generators. The loader never receives COM objects.
inline bool WriteMethodReport(const wchar_t* path, const std::vector<MethodAddress>& methods) {
    std::ofstream out{std::filesystem::path(path), std::ios::trunc};
    if (!out) return false;
    out << "{\"pid\":" << GetCurrentProcessId() << ",\"methods\":[";
    bool first = true;
    for (const auto& method : methods) {
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(method.address), &module))
            return false;
        wchar_t module_path[32768]{};
        const DWORD n = GetModuleFileNameW(module, module_path, 32768);
        if (!n || n == 32768) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto address = reinterpret_cast<std::uintptr_t>(method.address);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (address < base || address - base > nt->OptionalHeader.SizeOfImage - 16) return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(method.address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(memory.Protect &
              (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
            address + 16 > reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize)
            return false;
        if (!first) out << ',';
        first = false;
        out << "{\"role\":\"" << method.role << "\",\"module\":\"";
        // generic_u8string uses forward slashes; escape remaining JSON controls.
        for (const auto c : std::filesystem::path(module_path).generic_u8string()) {
            if (c == '"' || c == '\\') out << '\\';
            if (c < 0x20) return false;
            out << static_cast<char>(c);
        }
        out << "\",\"object\":" << reinterpret_cast<std::uintptr_t>(method.object)
            << ",\"base\":" << base << ",\"address\":" << address << ",\"rva\":" << address - base
            << ",\"image_size\":" << nt->OptionalHeader.SizeOfImage
            << ",\"timestamp\":" << nt->FileHeader.TimeDateStamp << ",\"bytes\":\"";
        const auto* bytes = static_cast<const unsigned char*>(method.address);
        for (int i = 0; i < 16; ++i)
            out << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(bytes[i]);
        out << std::dec << "\"}";
    }
    out << "]}\n";
    return out.good();
}

inline bool ReportRequestedMethods(const std::vector<MethodAddress>& methods) {
    wchar_t path[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"GTG_RESEARCH_OFFSET_REPORT", path, 32768);
    return n == 0 || (n < 32768 && WriteMethodReport(path, methods));
}
}  // namespace gtg::research
