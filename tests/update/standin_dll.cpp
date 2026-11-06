// Stand-in for gtg_overlay.dll in the update script's tests: something a
// running "game" keeps loaded.
#include <windows.h>

extern "C" __declspec(dllexport) int StandinOverlayVersion() {
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
