#include <windows.h>
#include "present_hook.h"

typedef int scs_result_t;
typedef unsigned int scs_u32_t;
constexpr scs_result_t SCS_RESULT_ok = 0;

extern "C" __declspec(dllexport) scs_result_t scs_telemetry_init(const scs_u32_t, const void* const) {
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        StartPresentHook();
        return 0;
    }, nullptr, 0, nullptr);
    return SCS_RESULT_ok;
}

extern "C" __declspec(dllexport) void scs_telemetry_shutdown() {
    StopPresentHook();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            StopPresentHook();
            break;
    }
    return TRUE;
}