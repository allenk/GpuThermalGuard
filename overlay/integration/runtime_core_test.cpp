// What the runtime selection refuses. A target whose identity, image or
// signature cannot be established is never discovered against a guess.
#include "runtime_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
int failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}

// A refusal is only correct if it is the refusal it claims to be: the reason is
// the evidence the helper publishes.
template <typename Action>
void Refuses(Action&& action, std::string_view reason, const char* message) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Check(std::string_view(error.what()) == reason, message);
        return;
    } catch (...) {
    }
    Check(false, message);
}
}  // namespace

int main() {
    using namespace gtg::overlay::integration;

    Refuses([] { (void)CoreRuntime::Open({}); }, "target runtime identity required",
            "no identity is a refusal, not the old behaviour");
    Refuses([] { (void)CoreRuntime::Open({GetCurrentProcessId(), 0}); },
            "target runtime identity required", "half an identity is no identity");
    Refuses([] { (void)CoreRuntime::Open({GetCurrentProcessId(), 1}); },
            "target lifetime changed before runtime discovery",
            "a creation time that does not match is another process");

    const Identity self{GetCurrentProcessId(), CreationTime(GetCurrentProcess())};
    Check(self.created != 0, "own creation time");
    Check(CoreRuntime::Open(self) == nullptr,
          "a target with no Core of its own yields nothing and refuses nothing");

    MODULEENTRY32W module{};
    module.modBaseSize = 4096;
    wchar_t directory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(directory, MAX_PATH);
    Check(length && length < MAX_PATH, "system directory");
    const auto system_core = std::filesystem::path(directory) / L"D3D12Core.dll";
    wcscpy_s(module.szExePath, system_core.c_str());
    // A null process handle proves the point: recognising the system runtime
    // needs no remote read, so the existing route costs nothing new.
    CoreRuntime system(module, nullptr);
    Check(system.IsSystem() && system.SdkVersion() == 0,
          "the system Core is recognised without inspecting the target");

    Check(GetModuleFileNameW(nullptr, module.szExePath, MAX_PATH) != 0, "own image path");
    Refuses([&] { CoreRuntime unsigned_image(module, nullptr); },
            "app-local Core signature is not a trusted Microsoft runtime",
            "the helper refuses to load an image it cannot verify");

    if (failures == 0) std::puts("PASS runtime selection refusals");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
