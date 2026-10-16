// VectorXR motion vector capture: a dxgi.dll the game loads from its own
// folder. It forwards every DXGI export to the system's dxgi.dll and, from a
// thread of its own, hooks DLSS (see ngx_hook.cpp).

#include <windows.h>

#include "ngx_hook.h"

// The system's dxgi.dll, by full path so these forwarders do not resolve back
// to this module. Regenerate the list from the machine's own dxgi.dll with
// `dumpbin /exports C:\Windows\System32\dxgi.dll` when another export is needed.
#if defined(_MSC_VER)
#define MVC_FORWARD(name) __pragma(comment(linker, "/export:" #name "=C:\\Windows\\System32\\dxgi." #name))
MVC_FORWARD(CreateDXGIFactory)
MVC_FORWARD(CreateDXGIFactory1)
MVC_FORWARD(CreateDXGIFactory2)
MVC_FORWARD(DXGIGetDebugInterface1)
MVC_FORWARD(DXGIDeclareAdapterRemovalSupport)
MVC_FORWARD(DXGIDisableVBlankVirtualization)
MVC_FORWARD(DXGIReportAdapterConfiguration)
MVC_FORWARD(DXGID3D10CreateDevice)
MVC_FORWARD(DXGID3D10CreateLayeredDevice)
MVC_FORWARD(DXGID3D10GetLayeredDeviceSize)
MVC_FORWARD(DXGID3D10RegisterLayers)
#endif

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Not under the loader lock: the thread only starts once DllMain returns.
        if (HANDLE thread = CreateThread(nullptr, 0, mvcapture::WatchForNgx, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
