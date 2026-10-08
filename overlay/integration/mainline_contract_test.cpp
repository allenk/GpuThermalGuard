#include "session_policy.hpp"
#include "settings/ui_preferences.hpp"
#include "target_admission.hpp"
#include <cstdio>

int main() {
    HANDLE owned =
        OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, GetCurrentProcessId());
    const bool admitted = gtg::overlay::integration::TargetAdmitted(owned);
    if (owned) CloseHandle(owned);
    if (gtg::overlay::integration::TargetAdmitted(nullptr) ||
        gtg::overlay::integration::TargetAdmitted(INVALID_HANDLE_VALUE) || !admitted) {
        std::fputs("FAIL fail-closed target queries / owned native process admission\n", stderr);
        return 1;
    }
    for (unsigned modifiers = 0; modifiers < 32; ++modifiers) {
        for (unsigned key = 0; key < 256; ++key) {
            if (gtg::overlay::integration::ValidHotkey(modifiers, key) !=
                gtg::settings::IsAcceptableOverlayHotkey({modifiers, key})) {
                std::fprintf(stderr, "FAIL UI/runtime hotkey disagreement: %u %u\n", modifiers,
                             key);
                return 1;
            }
        }
    }
    std::puts("PASS shared UI/runtime hotkey contract (8192 combinations)");
}
