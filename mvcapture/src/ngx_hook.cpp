#include "ngx_hook.h"

#include <windows.h>
#include <d3d11.h>

#include <MinHook.h>
#include <nvsdk_ngx.h>

#include <cstring>

#include "capture.h"
#include "log.h"

namespace mvcapture {
namespace {

using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                  PFN_NVSDK_NGX_ProgressCallback);

EvaluateFn g_original = nullptr;

NVSDK_NGX_Result NVSDK_CONV EvaluateDetour(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle,
                                            const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
    try {
        OnEvaluate(context, handle, parameters);
    } catch (...) {
        // Capture must never change what the game sees.
    }
    const NVSDK_NGX_Result result = g_original(context, handle, parameters, callback);
    try {
        OnEvaluateDone(context, handle, parameters);
    } catch (...) {
        // Capture must never change what the game sees.
    }
    return result;
}

// Logs the module's path and the exports that matter for finding the hook point.
void LogExports(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    Log("NGX core library: %ls", path);

    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress) {
        Log("it has no export table");
        return;
    }
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    Log("it exports %lu names; those mentioning Evaluate or D3D11:", exports->NumberOfNames);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (std::strstr(name, "Evaluate") || std::strstr(name, "D3D11")) {
            Log("  %s", name);
        }
    }
}

bool TryHook(HMODULE module) {
    LogExports(module);
    void* target = reinterpret_cast<void*>(GetProcAddress(module, "NVSDK_NGX_D3D11_EvaluateFeature"));
    if (!target) {
        Log("NVSDK_NGX_D3D11_EvaluateFeature is not exported by that library; nothing hooked (see the list above)");
        return false;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&EvaluateDetour), reinterpret_cast<void**>(&g_original)) != MH_OK) {
        Log("MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("MH_EnableHook failed");
        return false;
    }
    Log("hooked NVSDK_NGX_D3D11_EvaluateFeature at %p", target);
    return true;
}

} // namespace

DWORD WINAPI WatchForNgx(LPVOID) {
    Log("VectorXR motion vector capture loaded in %lu", GetCurrentProcessId());
    if (!Enabled()) {
        Log("VECTORXR_MVCAPTURE=0: capture disabled");
        return 0;
    }
    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize failed");
        return 0;
    }
    // The game loads the NGX core library when it starts DLSS (from its own
    // folder first, then the driver's); poll for it instead of hooking
    // LoadLibrary.
    static const wchar_t* const kNames[] = {L"_nvngx.dll", L"nvngx.dll"};
    for (;;) {
        for (const wchar_t* name : kNames) {
            if (const HMODULE module = GetModuleHandleW(name)) {
                TryHook(module);
                return 0;
            }
        }
        Sleep(100);
    }
}

} // namespace mvcapture
